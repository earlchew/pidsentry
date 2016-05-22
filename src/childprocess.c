/* -*- c-basic-offset:4; indent-tabs-mode:nil -*- vi: set sw=4 et: */
/*
// Copyright (c) 2016, Earl Chew
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the names of the authors of source code nor the names
//       of the contributors to the source code may be used to endorse or
//       promote products derived from this software without specific
//       prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL EARL CHEW BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "childprocess.h"
#include "umbilical.h"
#include "tether.h"
#include "pidserver.h"

#include "thread_.h"
#include "fd_.h"
#include "error_.h"
#include "process_.h"
#include "macros_.h"
#include "test_.h"
#include "socketpair_.h"
#include "bellsocketpair_.h"
#include "stdfdfiller_.h"
#include "eventpipe_.h"
#include "printf_.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/un.h>

/* -------------------------------------------------------------------------- */
enum PollFdChildKind
{
    POLL_FD_CHILD_TETHER,
    POLL_FD_CHILD_UMBILICAL,
    POLL_FD_CHILD_PARENT,
    POLL_FD_CHILD_EVENT_PIPE,
    POLL_FD_CHILD_KINDS
};

static const char *pollFdNames_[POLL_FD_CHILD_KINDS] =
{
    [POLL_FD_CHILD_TETHER]     = "tether",
    [POLL_FD_CHILD_UMBILICAL]  = "umbilical",
    [POLL_FD_CHILD_PARENT]     = "parent",
    [POLL_FD_CHILD_EVENT_PIPE] = "event pipe",
};

/* -------------------------------------------------------------------------- */
enum PollFdChildTimerKind
{
    POLL_FD_CHILD_TIMER_TETHER,
    POLL_FD_CHILD_TIMER_UMBILICAL,
    POLL_FD_CHILD_TIMER_TERMINATION,
    POLL_FD_CHILD_TIMER_DISCONNECTION,
    POLL_FD_CHILD_TIMER_KINDS
};

static const char *pollFdTimerNames_[POLL_FD_CHILD_TIMER_KINDS] =
{
    [POLL_FD_CHILD_TIMER_TETHER]        = "tether",
    [POLL_FD_CHILD_TIMER_UMBILICAL]     = "umbilical",
    [POLL_FD_CHILD_TIMER_TERMINATION]   = "termination",
    [POLL_FD_CHILD_TIMER_DISCONNECTION] = "disconnection",
};

/* -------------------------------------------------------------------------- */
int
createChildProcess(struct ChildProcess *self)
{
    int rc = - 1;

    self->mPid  = Pid(0);
    self->mPgid = Pgid(0);

    self->mTetherPipe     = 0;
    self->mChildLatch     = 0;
    self->mUmbilicalLatch = 0;

    self->mChildMonitor.mMutex   = 0;
    self->mChildMonitor.mMonitor = 0;

    ERROR_IF(
        createEventLatch(&self->mChildLatch_));
    self->mChildLatch = &self->mChildLatch_;

    ERROR_IF(
        createEventLatch(&self->mUmbilicalLatch_));
    self->mUmbilicalLatch = &self->mUmbilicalLatch_;

    self->mChildMonitor.mMutex = createThreadSigMutex(
        &self->mChildMonitor.mMutex_);

    /* Only the reading end of the tether is marked non-blocking. The
     * writing end must be used by the child process (and perhaps inherited
     * by any subsequent process that it forks), so only the reading
     * end is marked non-blocking. */

    ERROR_IF(
        createPipe(&self->mTetherPipe_, 0));
    self->mTetherPipe = &self->mTetherPipe_;

    ERROR_IF(
        closeFileOnExec(self->mTetherPipe->mRdFile, O_CLOEXEC));

    ERROR_IF(
        nonBlockingFile(self->mTetherPipe->mRdFile));

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            self->mTetherPipe          = closePipe(self->mTetherPipe);
            self->mChildMonitor.mMutex =
                destroyThreadSigMutex(self->mChildMonitor.mMutex);
            self->mUmbilicalLatch      = closeEventLatch(self->mUmbilicalLatch);
            self->mChildLatch          = closeEventLatch(self->mChildLatch);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
printChildProcess(const struct ChildProcess *self, FILE *aFile)
{
    return fprintf(aFile,
                   "<child %p pid %" PRId_Pid " pgid %" PRId_Pgid ">",
                   self,
                   FMTd_Pid(self->mPid),
                   FMTd_Pgid(self->mPgid));
}

/* -------------------------------------------------------------------------- */
static struct ChildProcessState
superviseChildProcess_(const struct ChildProcess *self,
                       const char                *aRole,
                       struct Pid                 aPid,
                       struct EventLatch         *aLatch)
{
    int rc = -1;

    struct ChildProcessState processState;

    /* Check that the process being monitored is the one
     * is the subject of the signal. Here is a way for a parent
     * to be surprised by the presence of an adopted child:
     *
     *  sleep 5 & exec sh -c 'sleep 1 & wait'
     *
     * The new shell inherits the earlier sleep as a child even
     * though it did not create it. */

    ERROR_IF(
        (processState = monitorProcessChild(aPid),
         ChildProcessStateError == processState.mChildState));

    if (ChildProcessStateRunning == processState.mChildState)
    {
        debug(1,
              "%s pid %" PRId_Pid " running",
              aRole,
              FMTd_Pid(aPid));

        ERROR_IF(
            EventLatchSettingError == setEventLatch(aLatch));
    }
    else if (ChildProcessStateStopped == processState.mChildState ||
             ChildProcessStateTrapped == processState.mChildState)
    {
        debug(1,
              "%s pid %" PRId_Pid " state %" PRIs_ChildProcessState,
              aRole,
              FMTd_Pid(aPid),
              FMTs_ChildProcessState(processState));
    }
    else
    {
        struct ProcessSignalName sigName;

        switch (processState.mChildState)
        {
        default:
            debug(1, "%s " "pid %" PRId_Pid " state %" PRIs_ChildProcessState,
                  aRole,
                  FMTd_Pid(aPid),
                  FMTs_ChildProcessState(processState));
            break;

        case ChildProcessStateExited:
            debug(1,
                  "%s "
                  "pid %" PRId_Pid " "
                  "state %" PRIs_ChildProcessState " "
                  "status %d",
                  aRole,
                  FMTd_Pid(aPid),
                  FMTs_ChildProcessState(processState),
                  processState.mChildStatus);
            break;

        case ChildProcessStateKilled:
            debug(1,
                  "%s "
                  "pid %" PRId_Pid " "
                  "state %" PRIs_ChildProcessState " "
                  "killed by %s",
                  aRole,
                  FMTd_Pid(aPid),
                  FMTs_ChildProcessState(processState),
                  formatProcessSignalName(&sigName, processState.mChildStatus));
            break;
        }

        ERROR_IF(
            EventLatchSettingError == disableEventLatch(aLatch));
    }

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc,
                        printChildProcess, self,
                        "role %s pid %" PRId_Pid, aRole, FMTd_Pid(aPid));

        if (rc)
            processState.mChildStatus = ChildProcessStateError;
    });

    return processState;
}

int
superviseChildProcess(struct ChildProcess *self, struct Pid aUmbilicalPid)
{
    int rc = -1;

    struct ChildProcessState processState;

    if (aUmbilicalPid.mPid)
        ERROR_IF(
            (processState = superviseChildProcess_(
                self, "umbilical", aUmbilicalPid, self->mUmbilicalLatch),
             ChildProcessStateError == processState.mChildStatus));

    ERROR_IF(
        (processState = superviseChildProcess_(
            self, "child", self->mPid, self->mChildLatch),
         ChildProcessStateError == processState.mChildStatus));

    /* If the monitored child process has been killed by SIGQUIT and
     * dumped core, then dump core in sympathy. */

    if (ChildProcessStateDumped == processState.mChildState &&
        SIGQUIT == processState.mChildStatus)
    {
        quitProcess();
    }

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcess, self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
killChildProcess(struct ChildProcess *self, int aSigNum)
{
    int rc = -1;

    struct ProcessSignalName sigName;

    ensure(self->mPid.mPid);

    debug(0,
          "sending %s to child pid %" PRId_Pid,
          formatProcessSignalName(&sigName, aSigNum),
          FMTd_Pid(self->mPid));

    ERROR_IF(
        kill(self->mPid.mPid, aSigNum));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcess, self,
                        "signal %s",
                        formatProcessSignalName(&sigName, aSigNum));
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
killChildProcessGroup(struct ChildProcess *self)
{
    int rc = -1;

    ERROR_IF(
        signalProcessGroup(self->mPgid, SIGKILL));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc,
                        printChildProcess, self,
                        "child pgid %" PRId_Pgid, FMTd_Pgid(self->mPgid));
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
pauseChildProcessGroup(struct ChildProcess *self)
{
    int rc = -1;

    ensure(self->mPgid.mPgid);

    ERROR_IF(
        killpg(self->mPgid.mPgid, SIGSTOP));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc,
                        printChildProcess, self,
                        "child pgid %" PRId_Pgid, FMTd_Pgid(self->mPgid));
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
resumeChildProcessGroup(struct ChildProcess *self)
{
    int rc = -1;

    ensure(self->mPgid.mPgid);

    ERROR_IF(
        killpg(self->mPgid.mPgid, SIGCONT));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc,
                        printChildProcess, self,
                        "child pgid %" PRId_Pgid, FMTd_Pgid(self->mPgid));
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct ForkChildProcess_
{
    struct ChildProcess   *mChildProcess;
    char                 **mCmd;
    struct StdFdFiller    *mStdFdFiller;
    struct BellSocketPair *mSyncSocket;
    struct SocketPair     *mUmbilicalSocket;
};

static CHECKED int
runChildProcess_(struct ForkChildProcess_ *self)
{
    int rc = -1;

    debug(
        0,
        "starting child process pid %" PRId_Pid, FMTd_Pid(ownProcessId()));

    int err;

    do
    {
        /* The forked child has all its signal handlers reset, but
         * note that the parent will wait for the child to synchronise
         * before sending it signals, so that there is no race here.
         *
         * Close the StdFdFiller in case this will free up stdin, stdout or
         * stderr. The remaining operations will close the remaining
         * unwanted file descriptors. */

        self->mStdFdFiller = closeStdFdFiller(self->mStdFdFiller);

        /* Wait until the parent has created the pidfile. This
         * invariant can be used to determine if the pidfile
         * is really associated with the process possessing
         * the specified pid. */

        debug(0, "synchronising child process");

        closeBellSocketPairParent(self->mSyncSocket);

        err = 0;
        TEST_RACE
        ({
            if ( ! err)
                ERROR_IF(
                    (err = waitBellSocketPairChild(self->mSyncSocket, 0),
                     err && EPIPE != errno && ENOENT != errno));

            if ( ! err)
                ERROR_IF(
                    (err = ringBellSocketPairChild(self->mSyncSocket),
                     err && EPIPE != errno));
        });
        if (err)
            break;

        do
        {
            /* Close the reading end of the tether pipe separately
             * because it might turn out that the writing end
             * will not need to be duplicated. */

            closePipeReader(self->mChildProcess->mTetherPipe);

            self->mUmbilicalSocket = closeSocketPair(self->mUmbilicalSocket);

            if (gOptions.mTether)
            {
                int tetherFd = *gOptions.mTether;

                if (0 > tetherFd)
                    tetherFd = self->mChildProcess->mTetherPipe->mWrFile->mFd;

                char tetherArg[sizeof(int) * CHAR_BIT + 1];

                sprintf(tetherArg, "%d", tetherFd);

                if (gOptions.mName)
                {
                    bool useEnv = isupper(gOptions.mName[0]);

                    for (unsigned ix = 1; useEnv && gOptions.mName[ix]; ++ix)
                    {
                        unsigned char ch = gOptions.mName[ix];

                        if ( ! isupper(ch) && ! isdigit(ch) && ch != '_')
                            useEnv = false;
                    }

                    if (useEnv)
                    {
                        ERROR_IF(
                            setenv(gOptions.mName, tetherArg, 1));
                    }
                    else
                    {
                        /* Start scanning from the first argument, leaving
                         * the command name intact. */

                        char *matchArg = 0;

                        for (unsigned ix = 1; self->mCmd[ix]; ++ix)
                        {
                            matchArg = strstr(self->mCmd[ix], gOptions.mName);

                            if (matchArg)
                            {
                                char replacedArg[
                                    strlen(self->mCmd[ix]) -
                                    strlen(gOptions.mName) +
                                    strlen(tetherArg)      + 1];

                                sprintf(replacedArg,
                                        "%.*s%s%s",
                                        matchArg - self->mCmd[ix],
                                        self->mCmd[ix],
                                        tetherArg,
                                        matchArg + strlen(gOptions.mName));

                                self->mCmd[ix] = strdup(replacedArg);

                                ERROR_UNLESS(
                                    self->mCmd[ix],
                                    {
                                        terminate(
                                            errno,
                                            "Unable to duplicate '%s'",
                                            replacedArg);
                                    });
                                break;
                            }
                        }

                        ERROR_UNLESS(
                            matchArg,
                            {
                                terminate(
                                    0,
                                    "Unable to find matching argument '%s'",
                                    gOptions.mName);
                            });
                    }
                }

                if (tetherFd == self->mChildProcess->mTetherPipe->mWrFile->mFd)
                    break;

                ERROR_IF(
                    dup2(self->mChildProcess->mTetherPipe->mWrFile->mFd,
                         tetherFd) != tetherFd);
            }

            self->mChildProcess->mTetherPipe = closePipe(
                self->mChildProcess->mTetherPipe);

        } while (0);

        /* Wait until the watchdog has had a chance to announce the
         * child pid before proceeding. This allows external programs,
         * notably the unit test, to know that the child process
         * is fully initialised. */

        TEST_RACE
        ({
            ERROR_IF(
                (err = waitBellSocketPairChild(self->mSyncSocket, 0),
                 err && EPIPE != errno && ENOENT != errno));
        });
        if (err)
            break;

        /* Rely on the upcoming exec() to provide the final synchronisation
         * indication to the waiting watchdog. The watchdog relies on this
         * to know that the child will no longer share any file descriptors
         * and locks with the parent. */

        ensure(
            ownFileCloseOnExec(
                self->mSyncSocket->mSocketPair->mChildSocket->mFile));

        debug(0, "child process synchronised");

        /* The child process does not close the process lock because it
         * might need to emit a diagnostic if execProcess() fails. Rely on
         * O_CLOEXEC to close the underlying file descriptors. */

        execProcess(self->mCmd[0], self->mCmd);
        message(errno, "Unable to execute '%s'", self->mCmd[0]);

    } while (0);

    rc = EXIT_FAILURE;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
forkChildProcess(
    struct ChildProcess   *self,
    char                 **aCmd,
    struct StdFdFiller    *aStdFdFiller,
    struct BellSocketPair *aSyncSocket,
    struct SocketPair     *aUmbilicalSocket)
{
    int rc = -1;

    /* Both the parent and child share the same signal handler configuration.
     * In particular, no custom signal handlers are configured, so
     * signals delivered to either will likely caused them to terminate.
     *
     * This is safe because that would cause one of end the synchronisation
     * pipe to close, and the other end will eventually notice. */

    struct ForkChildProcess_ childProcess =
    {
        .mChildProcess    = self,
        .mCmd             = aCmd,
        .mStdFdFiller     = aStdFdFiller,
        .mSyncSocket      = aSyncSocket,
        .mUmbilicalSocket = aUmbilicalSocket,
    };

    struct Pid childPid;
    ERROR_IF(
        (childPid = forkProcessChild(
            ForkProcessSetProcessGroup,
            Pgid(0),
            ForkProcessMethod(runChildProcess_, &childProcess)),
         -1 == childPid.mPid));

    /* Do not try to place the watchdog in the process group of the child.
     * This allows the parent to supervise the watchdog, and the watchdog
     * to monitor the child process group.
     *
     * Trying to force the watchdog into the new process group of the child
     * will likely cause a race in an inattentive parent of the watchdog.
     * For example upstart(8) has:
     *
     *    pgid = getpgid(pid);
     *    kill(pgid > 0 ? -pgid : pid, signal);
     */


    /* Even if the child has terminated, it remains a zombie until reaped,
     * so it is safe to query it to determine its process group. */

    self->mPid  = childPid;
    self->mPgid = fetchProcessGroupId(self->mPid);

    debug(0,
          "running child pid %" PRId_Pid " in pgid %" PRId_Pgid,
          FMTd_Pid(self->mPid),
          FMTd_Pgid(self->mPgid));

    ensure(self->mPid.mPid == self->mPgid.mPgid);

    /* Beware of the inherent race here between the child starting and
     * terminating, and the recording of the child pid. To cover the
     * case that the child might have terminated before the child pid
     * is recorded, force a supervision run after the pid is recorded. */

    ERROR_IF(
        superviseChildProcess(self, Pid(0)));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcess, self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closeChildProcessTether(struct ChildProcess *self)
{
    int rc = -1;

    ensure(self->mTetherPipe);

    self->mTetherPipe = closePipe(self->mTetherPipe);

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcess, self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
closeChildFiles_(struct ChildProcess *self)
{
    self->mTetherPipe = closePipe(self->mTetherPipe);
}

/* -------------------------------------------------------------------------- */
int
reapChildProcess(struct ChildProcess *self, int *aStatus)
{
    int rc = -1;

    ERROR_IF(
        reapProcessChild(self->mPid, aStatus));

    /* Once the child process is reaped, the process no longer exists, so
     * the pid should no longer be used to refer to it. */

    self->mPid = Pid(0);

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcess, self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct ChildProcess *
closeChildProcess(struct ChildProcess *self)
{
    if (self)
    {
        if (self->mPid.mPid)
        {
            ABORT_IF(
                killChildProcess(self, SIGKILL));

            int status;
            ABORT_IF(
                reapChildProcess(self, &status));
        }

        ensure( ! self->mChildMonitor.mMonitor);
        self->mChildMonitor.mMutex =
            destroyThreadSigMutex(self->mChildMonitor.mMutex);

        closeChildFiles_(self);

        self->mUmbilicalLatch = closeEventLatch(self->mUmbilicalLatch);
        self->mChildLatch     = closeEventLatch(self->mChildLatch);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Child Process Monitoring
 *
 * The child process must be monitored for activity, and also for
 * termination.
 */

enum ChildTerminationAction
{
    ChildTermination_Terminate,
    ChildTermination_Abort,
    ChildTermination_Actions,
};

struct ChildSignalPlan
{
    struct Pid mPid;
    int        mSig;
};

struct ChildMonitor
{
    struct Pid mChildPid;

    struct TetherThread *mTetherThread;
    struct EventPipe    *mEventPipe;
    struct EventLatch   *mContLatch;

    struct
    {
        const struct ChildSignalPlan *mSignalPlans[ChildTermination_Actions];
        const struct ChildSignalPlan *mSignalPlan;
        struct Duration               mSignalPeriod;
    } mTermination;

    struct
    {
        struct File *mFile;
        struct Pid   mPid;
        bool         mPreempt;       /* Request back-to-back pings */
        unsigned     mCycleCount;    /* Current number of cycles */
        unsigned     mCycleLimit;    /* Cycles before triggering */
    } mUmbilical;

    struct
    {
        unsigned mCycleCount;       /* Current number of cycles */
        unsigned mCycleLimit;       /* Cycles before triggering */
    } mTether;

    struct
    {
        struct EventLatch *mChildLatch;
        struct EventLatch *mUmbilicalLatch;
    } mEvent;

    struct
    {
        struct Pid   mPid;
        struct Pipe *mPipe;
    } mParent;

    struct pollfd            mPollFds[POLL_FD_CHILD_KINDS];
    struct PollFdAction      mPollFdActions[POLL_FD_CHILD_KINDS];
    struct PollFdTimerAction mPollFdTimerActions[POLL_FD_CHILD_TIMER_KINDS];
};

/* -------------------------------------------------------------------------- */
int
printChildProcessMonitor(const struct ChildMonitor *self, FILE *aFile)
{
    return fprintf(aFile,
                   "<child monitor %p pid %" PRId_Pid ">",
                   self,
                   FMTd_Pid(self->mChildPid));
}

/* -------------------------------------------------------------------------- */
/* Child Termination State Machine
 *
 * When it is necessary to terminate the child process, run a state
 * machine to sequence through a signal plan that walks through
 * an escalating series of signals. */

static void
activateFdTimerTermination_(struct ChildMonitor         *self,
                            enum ChildTerminationAction  aAction,
                            const struct EventClockTime *aPollTime)
{
    /* When it is necessary to terminate the child process, the child
     * process might already have terminated. No special action is
     * taken with the expectation that the termination code should
     * fully expect that child the terminate at any time */

    struct PollFdTimerAction *tetherTimer =
        &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_TETHER];

    tetherTimer->mPeriod = Duration(NanoSeconds(0));

    struct PollFdTimerAction *terminationTimer =
        &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_TERMINATION];

    if ( ! terminationTimer->mPeriod.duration.ns)
    {
        debug(1, "activating termination timer");

        ensure( ! self->mTermination.mSignalPlan);

        self->mTermination.mSignalPlan =
            self->mTermination.mSignalPlans[aAction];

        terminationTimer->mPeriod = self->mTermination.mSignalPeriod;

        lapTimeTrigger(
            &terminationTimer->mSince, terminationTimer->mPeriod, aPollTime);
    }
}

static CHECKED int
pollFdTimerTermination_(struct ChildMonitor         *self,
                        const struct EventClockTime *aPollTime)
{
    int rc = -1;

    /* Remember that this function races termination of the child process.
     * The child process might have terminated by the time this function
     * attempts to deliver the next signal. This should be handled
     * correctly because the child process will remain as a zombie
     * and signals will be delivered successfully, but without effect. */

    struct Pid pidNum = self->mTermination.mSignalPlan->mPid;
    int        sigNum = self->mTermination.mSignalPlan->mSig;

    if (self->mTermination.mSignalPlan[1].mSig)
        ++self->mTermination.mSignalPlan;

    struct ProcessSignalName sigName;

    warn(
        0,
        "Killing child pid %" PRId_Pid " with %s",
        FMTd_Pid(pidNum),
        formatProcessSignalName(&sigName, sigNum));

    ERROR_IF(
        kill(pidNum.mPid, sigNum));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcessMonitor, self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
/* Maintain Parent Connection
 *
 * This connection allows for monitoring ofthe parent. The child will
 * terminate if the parent terminates. */

static CHECKED int
pollFdParent_(struct ChildMonitor         *self,
              const struct EventClockTime *aPollTime)
{
    int rc = -1;

    warn(0,
         "Parent pid %" PRId_Pid " has terminated",
         FMTd_Pid(self->mParent.mPid));

    self->mPollFds[POLL_FD_CHILD_PARENT].fd     = -1;
    self->mPollFds[POLL_FD_CHILD_PARENT].events = 0;

    activateFdTimerTermination_(self, ChildTermination_Terminate, aPollTime);

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcessMonitor, self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
/* Maintain Umbilical Connection
 *
 * This connection allows the umbilical monitor to terminate the child
 * process if it detects that the watchdog is no longer functioning
 * properly. This is important in scenarios where the supervisor
 * init(8) kills the watchdog without giving the watchdog a chance
 * to clean up, or if the watchdog fails catatrophically. */

static void
restartFdTimerUmbilical_(struct ChildMonitor         *self,
                         const struct EventClockTime *aPollTime)
{
    if (self->mUmbilical.mCycleCount != self->mUmbilical.mCycleLimit)
    {
        ensure(self->mUmbilical.mCycleCount < self->mUmbilical.mCycleLimit);

        self->mUmbilical.mCycleCount = 0;

        lapTimeRestart(
            &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_UMBILICAL].mSince,
            aPollTime);
    }
}

static void
pollFdCloseUmbilical_(struct ChildMonitor         *self,
                      const struct EventClockTime *aPollTime)
{
    struct PollFdTimerAction *umbilicalTimer =
        &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_UMBILICAL];

    self->mPollFds[POLL_FD_CHILD_UMBILICAL].fd     = -1;
    self->mPollFds[POLL_FD_CHILD_UMBILICAL].events = 0;

    umbilicalTimer->mPeriod = Duration(NanoSeconds(0));

    activateFdTimerTermination_(self, ChildTermination_Terminate, aPollTime);
}

static CHECKED int
pollFdUmbilical_(struct ChildMonitor         *self,
                 const struct EventClockTime *aPollTime)
{
    int rc = -1;

    struct PollFdTimerAction *umbilicalTimer =
        &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_UMBILICAL];

    ensure(self->mPollFds[POLL_FD_CHILD_UMBILICAL].events);

    char buf[1];

    /* If the far end did not read the previous ping, and simply closed its
     * end of the connection (likely because it either failed or was
     * inadvertently killed), then the read will return ECONNRESET. This
     * is equivalent to encountering the end of file. */

    ssize_t rdlen;
    ERROR_IF(
        (rdlen = read(
            self->mPollFds[POLL_FD_CHILD_UMBILICAL].fd, buf, sizeof(buf)),
         -1 == rdlen
         ? EINTR != errno && ECONNRESET != errno
         : (errno = 0, rdlen && sizeof(buf) != rdlen)));

    bool umbilicalClosed = false;

    if ( ! rdlen)
    {
        umbilicalClosed = true;

        errno = ECONNRESET;
        rdlen = -1;
    }

    if (-1 == rdlen)
    {
        if (ECONNRESET == errno)
        {
            if (umbilicalClosed)
                debug(0, "umbilical connection closed");
            else
                warn(0, "Umbilical connection broken");

            pollFdCloseUmbilical_(self, aPollTime);
        }
    }
    else
    {
        debug(1, "received umbilical connection echo %zd", rdlen);

        /* When the echo is received on the umbilical connection
         * schedule the next umbilical ping. The next ping is
         * scheduled immediately if the timer has been preempted. */

        ensure(self->mUmbilical.mCycleCount < self->mUmbilical.mCycleLimit);

        self->mUmbilical.mCycleCount = self->mUmbilical.mCycleLimit;

        if ( ! self->mUmbilical.mPreempt)
            lapTimeRestart(&umbilicalTimer->mSince, aPollTime);
        else
        {
            self->mUmbilical.mPreempt = false;

            lapTimeTrigger(&umbilicalTimer->mSince,
                           umbilicalTimer->mPeriod, aPollTime);
        }
    }

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcessMonitor, self);
    });

    return rc;
}

static CHECKED bool
pollFdWriteUmbilicalError_(int aErrno)
{
    bool error = false;

    FINALLY
    ({
        switch (aErrno)
        {
        default:
            error = true;
            break;

        case EPIPE:
            warn(1, "Umbilical connection closed");
            break;

        case EWOULDBLOCK:
            debug(1, "writing to umbilical blocked");
            break;

        case EINTR:
            debug(1, "umbilical write interrupted");
            break;
        }
    });

    return error;
}

static CHECKED int
pollFdWriteUmbilical_(struct ChildMonitor *self)
{
    int rc = -1;

    ensure(self->mUmbilical.mCycleCount == self->mUmbilical.mCycleLimit);

    char buf[1] = { '.' };

    ssize_t wrlen;
    ERROR_IF(
        (wrlen = write(
            self->mUmbilical.mFile->mFd, buf, sizeof(buf)),
         -1 == wrlen && pollFdWriteUmbilicalError_(errno)));

    if (-1 != wrlen)
    {
        debug(1, "sent umbilical ping %zd", wrlen);
        ensure(sizeof(buf) == wrlen);

        /* Once a message is written on the umbilical connection, expect
         * an echo to be returned from the umbilical monitor. */

        self->mUmbilical.mCycleCount = 0;
    }

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcessMonitor, self);
    });

    return rc;
}

static CHECKED int
pollFdReapUmbilicalEvent_(struct ChildMonitor         *self,
                          int                          aEvent,
                          const struct EventClockTime *aPollTime)
{
    int rc = -1;

    if (aEvent)
    {
        /* The umbilical process is running again after being stopped for
         * some time. Restart the tether timeout so that the stoppage
         * is not mistaken for a failure. */

        debug(0,
              "umbilical pid %" PRId_Pid " is running",
              FMTd_Pid(self->mUmbilical.mPid));

        restartFdTimerUmbilical_(self, aPollTime);
    }
    else
    {
        /* The umbilical process has terminated, so there is no longer
         * any need to monitor for SIGCHLD. */

        debug(0,
              "umbilical pid %" PRId_Pid " has terminated",
              FMTd_Pid(self->mUmbilical.mPid));
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
pollFdContUmbilical_(struct ChildMonitor         *self,
                     const struct EventClockTime *aPollTime)
{
    int rc = -1;

    /* This function is called after the process receives SIGCONT and
     * processes the event in the context of the event loop. The
     * function must indicate to the umbilical monitor that the
     * process has just woken, but there are two considerations:
     *
     *  a. The process is just about to receive the echo from the
     *     previous ping
     *  b. The process has yet to send the next ping */

    if (self->mUmbilical.mCycleCount != self->mUmbilical.mCycleLimit)
    {
        /* Accommodate the second case by expiring the timer
         * that controls the sending of the pings so that the
         * ping is sent immediately.  */

        struct PollFdTimerAction *umbilicalTimer =
            &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_UMBILICAL];

        lapTimeTrigger(&umbilicalTimer->mSince,
                       umbilicalTimer->mPeriod, aPollTime);
    }
    else
    {
        /* Handle the first case by indicating that another ping
         * should be scheduled immediately after the echo is
         * received. */

        self->mUmbilical.mPreempt = true;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
pollFdTimerUmbilical_(struct ChildMonitor         *self,
                      const struct EventClockTime *aPollTime)
{
    int rc = -1;

    if (self->mUmbilical.mCycleCount != self->mUmbilical.mCycleLimit)
    {
        ensure(self->mUmbilical.mCycleCount < self->mUmbilical.mCycleLimit);

        /* If waiting on a response from the umbilical monitor, apply
         * a timeout, and if the timeout is exceeded terminate the
         * child process. */

        struct ChildProcessState umbilicalState;
        ERROR_IF(
            (umbilicalState = monitorProcessChild(self->mUmbilical.mPid),
             ChildProcessStateError == umbilicalState.mChildState &&
             ECHILD != errno));

        /* Beware that the umbilical process might no longer be active.
         * If so, do nothing here, and rely on subsequent brokn umbilical
         * connection to trigger action. */

        if (ChildProcessStateError != umbilicalState.mChildState)
        {
            if (ChildProcessStateTrapped == umbilicalState.mChildState ||
                ChildProcessStateStopped == umbilicalState.mChildState)
            {
                debug(0,
                      "deferred timeout umbilical status %"
                      PRIs_ChildProcessState,
                      FMTs_ChildProcessState(umbilicalState));

                self->mUmbilical.mCycleCount = 0;
            }
            else
            {
                if (++self->mUmbilical.mCycleCount ==
                    self->mUmbilical.mCycleLimit)
                {
                    warn(0, "Umbilical connection timed out");

                    activateFdTimerTermination_(
                        self, ChildTermination_Terminate, aPollTime);
                }
            }
        }
    }
    else
    {
        if (pollFdWriteUmbilical_(self))
        {
            struct PollFdTimerAction *umbilicalTimer =
                &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_UMBILICAL];

            switch (errno)
            {
            default:
                break;

            case EPIPE:
                /* The umbilical monitor is no longer running and has
                 * closed the umbilical connection. */

                pollFdCloseUmbilical_(self, aPollTime);
                break;

            case EINTR:
                /* Do not loop here on EINTR since it is important
                 * to take care that the monitoring loop is
                 * non-blocking. Instead, mark the timer as expired
                 * for force the monitoring loop to retry immediately. */

                lapTimeTrigger(&umbilicalTimer->mSince,
                               umbilicalTimer->mPeriod, aPollTime);
                break;
            }
        }
    }

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcessMonitor, self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
/* Process Continuation
 *
 * This method is called soon after the process continues after being
 * stopped to alert the monitoring loop that timers must be re-synchronised
 * to compensate for the outage. */

static CHECKED int
pollFdContEvent_(struct ChildMonitor         *self,
                 int                          aEvent,
                 const struct EventClockTime *aPollTime)
{
    int rc = -1;

    ensure(aEvent);

    ERROR_IF(
        pollFdContUmbilical_(self, aPollTime));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
raiseFdContEvent_(struct ChildMonitor *self)
{
    int rc = -1;

    ERROR_IF(
        EventLatchSettingError == setEventLatch(self->mContLatch));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcessMonitor, self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
/* Watchdog Tether
 *
 * The main tether used by the watchdog to monitor the child process requires
 * the child process to maintain some activity on the tether to demonstrate
 * that the child is functioning correctly. Data transfer on the tether
 * occurs in a separate thread since it might block. The main thread
 * is non-blocking and waits for the tether to be closed. */

static void
disconnectPollFdTether_(struct ChildMonitor *self)
{
    debug(0, "disconnect tether control");

    self->mPollFds[POLL_FD_CHILD_TETHER].fd     = -1;
    self->mPollFds[POLL_FD_CHILD_TETHER].events = 0;
}

static CHECKED int
pollFdTether_(struct ChildMonitor         *self,
              const struct EventClockTime *aPollTime)
{
    int rc = -1;

    /* The tether thread control pipe will be closed when the tether
     * between the child process and watchdog is shut down. */

    disconnectPollFdTether_(self);

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcessMonitor, self);
    });

    return rc;
}

static void
restartFdTimerTether_(struct ChildMonitor         *self,
                      const struct EventClockTime *aPollTime)
{
    /* If the child process is running without a tether, there will
     * be no active tether timer to restart. */

    struct PollFdTimerAction *tetherTimer =
        &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_TETHER];

    if (tetherTimer->mPeriod.duration.ns)
    {
        self->mTether.mCycleCount = 0;

        lapTimeRestart(&tetherTimer->mSince, aPollTime);
    }
}

static CHECKED int
pollFdTimerTether_(struct ChildMonitor         *self,
                   const struct EventClockTime *aPollTime)
{
    int rc = -1;

    /* The tether timer is only active if there is a tether and it was
     * configured with a timeout. The timeout expires if there was
     * no activity on the tether with the consequence that the monitored
     * child will be terminated. */

    do
    {
        struct PollFdTimerAction *tetherTimer =
            &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_TETHER];

        struct ChildProcessState childState;

        ERROR_IF(
            (childState = monitorProcessChild(self->mChildPid),
             ChildProcessStateError == childState.mChildState &&
             ECHILD != errno));

        /* Be aware if the child process is no longer active, it makes
         * sense to proceed as if the child process should be terminated. */

        if (ChildProcessStateError != childState.mChildState)
        {
            if (ChildProcessStateTrapped == childState.mChildState ||
                ChildProcessStateStopped == childState.mChildState)
            {
                debug(0,
                      "deferred timeout child status %"
                      PRIs_ChildProcessState,
                      FMTs_ChildProcessState(childState));

                self->mTether.mCycleCount = 0;
                break;
            }
            else
            {
                /* Find when the tether was last active and use it to
                 * determine if a timeout has actually occurred. If
                 * there was recent activity, use the time of that
                 * activity to reschedule the timer in order to align
                 * the timeout with the activity. */

                struct EventClockTime since;
                {
                    pthread_mutex_t *lock =
                        lockMutex(self->mTetherThread->mActivity.mMutex);
                    since = self->mTetherThread->mActivity.mSince;
                    lock = unlockMutex(lock);
                }

                if (aPollTime->eventclock.ns <
                    since.eventclock.ns + tetherTimer->mPeriod.duration.ns)
                {
                    lapTimeRestart(&tetherTimer->mSince, &since);
                    self->mTether.mCycleCount = 0;
                    break;
                }

                if (++self->mTether.mCycleCount < self->mTether.mCycleLimit)
                    break;

                self->mTether.mCycleCount = self->mTether.mCycleLimit;
            }
        }

        /* Once the timeout has expired, the timer can be cancelled because
         * there is no further need to run this state machine. */

        debug(0, "timeout after %ds", gOptions.mTimeout.mTether_s);

        activateFdTimerTermination_(
            self, ChildTermination_Abort, aPollTime);

    } while (0);

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcessMonitor, self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static bool
pollFdCompletion_(struct ChildMonitor *self)
{
    /* Wait until the child process has terminated, and the tether thread
     * has completed. */

    return
        ! (self->mEvent.mChildLatch ||
           self->mPollFds[POLL_FD_CHILD_TETHER].events);
}

/* -------------------------------------------------------------------------- */
/* Child Termination
 *
 * The watchdog will receive SIGCHLD when the child process terminates,
 * though no direct indication will be received if the child process
 * performs an execv(2). The SIGCHLD signal will be delivered to the
 * event loop on a pipe, at which point the child process is known
 * to be dead. */

static CHECKED int
pollFdReapChildEvent_(struct ChildMonitor         *self,
                      int                          aEvent,
                      const struct EventClockTime *aPollTime)
{
    int rc = -1;

    if (aEvent)
    {
        /* The child process is running again after being stopped for
         * some time. Restart the tether timeout so that the stoppage
         * is not mistaken for a failure. */

        debug(0,
              "child pid %" PRId_Pid " is running",
              FMTd_Pid(self->mChildPid));

        restartFdTimerTether_(self, aPollTime);
    }
    else
    {
        /* The child process has terminated, so there is no longer
         * any need to monitor for SIGCHLD. */

        debug(0,
              "child pid %" PRId_Pid " has terminated",
              FMTd_Pid(self->mChildPid));

        /* Record when the child has terminated, but do not exit
         * the event loop until all the IO has been flushed. With the
         * child terminated, no further input can be produced so indicate
         * to the tether thread that it should start flushing data now. */

        ERROR_IF(
            flushTetherThread(self->mTetherThread));

        /* Once the child process has terminated, start the disconnection
         * timer that sends a periodic signal to the tether thread
         * to ensure that it will not block. */

        self->mPollFdTimerActions[
            POLL_FD_CHILD_TIMER_DISCONNECTION].mPeriod = Duration(
                NSECS(Seconds(1)));
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
pollFdTimerChild_(struct ChildMonitor         *self,
                  const struct EventClockTime *aPollTime)
{
    int rc = -1;

    debug(0, "disconnecting tether thread");

    ERROR_IF(
        pingTetherThread(self->mTetherThread));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcessMonitor, self);
    });

    return rc;

}

/* -------------------------------------------------------------------------- */
/* Event Pipe
 *
 * An event pipe is used to trigger activity on the event loop so that
 * a single rather expensive file descriptor can be used to service
 * multiple events. */

static CHECKED int
pollFdEventLatch_(
    struct EventLatch **aLatch, const char *aRole, int *aSignalled)
{
    int rc = -1;

    int signalled = 0;

    if (*aLatch)
    {
        enum EventLatchSetting setting;
        ERROR_IF(
            (setting = resetEventLatch(*aLatch),
             EventLatchSettingError == setting),
            {
                warn(errno, "Unable to reset %s event latch", aRole);
            });

        if (EventLatchSettingOn == setting)
            signalled = 1;
        else if (EventLatchSettingDisabled == setting)
        {
            signalled = -1;
            *aLatch = 0;
        }
    }

    *aSignalled = signalled;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
pollFdEventPipe_(struct ChildMonitor         *self,
                 const struct EventClockTime *aPollTime)
{
    int rc = -1;

    /* There is a race here between receiving the indication that there
     * is an event, and other watchdog actions that might be taking place
     * to actively monitor or terminate the child process. In other words,
     * those actions might be attempting to manage a child process that
     * is already dead, or declare the child process errant when it has
     * already exited.
     *
     * Actively test the race by occasionally delaying this activity
     * when in test mode. */

    if ( ! testSleep(TestLevelRace))
    {
        do
        {
            debug(0, "checking event pipe");

            int err;
            ERROR_IF(
                (err = resetEventPipe(self->mEventPipe),
                 -1 == err && EINTR != errno));

            if ( ! err)
                continue;

            struct
            {
                const char         *mRole;
                struct EventLatch **mLatch;
                int               (*mPoll)(
                    struct ChildMonitor *,
                    int,
                    const struct EventClockTime *);
            }
            eventLatches[] =
            {
                { "child",
                  &self->mEvent.mChildLatch, pollFdReapChildEvent_ },

                { "umbilical",
                  &self->mEvent.mUmbilicalLatch, pollFdReapUmbilicalEvent_ },

                { "continuation",
                  &self->mContLatch, pollFdContEvent_ },
            };

            for (unsigned ix = 0; NUMBEROF(eventLatches) > ix; ++ix)
            {
                AUTO(eventLatch, &eventLatches[ix]);

                int signalled;

                ERROR_IF(
                    pollFdEventLatch_(
                        eventLatch->mLatch, eventLatch->mRole, &signalled));

                if (signalled)
                    ERROR_IF(
                        eventLatch->mPoll(self, signalled > 0, aPollTime),
                        {
                            warn(errno,
                                 "Unable to poll %s event latch",
                                 eventLatch->mRole);
                        });
            }
        }
        while (0);
    }

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcessMonitor, self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
updateChildProcessMonitor_(
    struct ChildProcess *self, struct ChildMonitor *aMonitor)
{
    struct ThreadSigMutex *lock =
        lockThreadSigMutex(self->mChildMonitor.mMutex);

    self->mChildMonitor.mMonitor = aMonitor;

    lock = unlockThreadSigMutex(lock);
}

int
raiseChildProcessSigCont(struct ChildProcess *self)
{
    int rc = -1;

    struct ThreadSigMutex *lock = 0;

    lock = lockThreadSigMutex(self->mChildMonitor.mMutex);

    if (self->mChildMonitor.mMonitor)
        ERROR_IF(
            raiseFdContEvent_(self->mChildMonitor.mMonitor));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcess, self);

        lock = unlockThreadSigMutex(lock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
monitorChildProcess(struct ChildProcess     *self,
                    struct UmbilicalProcess *aUmbilicalProcess,
                    struct File             *aUmbilicalFile,
                    struct Pid               aParentPid,
                    struct Pipe             *aParentPipe)
{
    int rc = -1;

    debug(0, "start monitoring child");

    struct Pipe  nullPipe_;
    struct Pipe *nullPipe = 0;

    struct TetherThread  tetherThread_;
    struct TetherThread *tetherThread = 0;

    struct EventLatch  contLatch_;
    struct EventLatch *contLatch = 0;

    struct EventPipe  eventPipe_;
    struct EventPipe *eventPipe = 0;

    struct PollFd  pollfd_;
    struct PollFd *pollfd = 0;

    struct ChildMonitor *childMonitor = 0;

    ERROR_IF(
        createPipe(&nullPipe_, O_CLOEXEC | O_NONBLOCK));
    nullPipe = &nullPipe_;

    /* Create a thread to use a blocking copy to transfer data from a
     * local pipe to stdout. This is primarily because SPLICE_F_NONBLOCK
     * cannot guarantee that the operation is non-blocking unless both
     * source and destination file descriptors are also themselves non-blocking.
     *
     * The child thread is used to perform a potentially blocking
     * transfer between an intermediate pipe and stdout, while
     * the main monitoring thread deals exclusively with non-blocking
     * file descriptors. */

    ERROR_IF(
        createTetherThread(&tetherThread_, nullPipe));
    tetherThread = &tetherThread_;

    ERROR_IF(
        createEventLatch(&contLatch_));
    contLatch = &contLatch_;

    ERROR_IF(
        createEventPipe(&eventPipe_, O_CLOEXEC | O_NONBLOCK));
    eventPipe = &eventPipe_;

    ERROR_IF(
        EventLatchSettingError == bindEventLatchPipe(
            self->mChildLatch, eventPipe));

    ERROR_IF(
        EventLatchSettingError == bindEventLatchPipe(
            self->mUmbilicalLatch, eventPipe));

    ERROR_IF(
        EventLatchSettingError == bindEventLatchPipe(contLatch, eventPipe));

    /* Divide the timeout into two cycles so that if the child process is
     * stopped, the first cycle will have a chance to detect it and
     * defer the timeout. */

    const unsigned timeoutCycles = 2;

    struct ChildMonitor childMonitor_ =
    {
        .mChildPid     = self->mPid,
        .mTetherThread = tetherThread,
        .mEventPipe    = eventPipe,
        .mContLatch    = contLatch,

        .mParent =
        {
            .mPid  = aParentPid,
            .mPipe = aParentPipe,
        },

        .mEvent =
        {
            .mChildLatch     = self->mChildLatch,
            .mUmbilicalLatch = self->mUmbilicalLatch,
        },

        .mTermination =
        {
            .mSignalPlan   = 0,
            .mSignalPeriod = Duration(
                NSECS(Seconds(gOptions.mTimeout.mSignal_s))),
            .mSignalPlans  =
            {
                /* When terminating the child process, first request that
                 * the child terminate by sending it SIGTERM or other, and
                 * if the child does not terminate, resort to sending SIGKILL.
                 *
                 * Do not kill the child process group here since that would
                 * also terminate the umbilical process prematurely. Rely on
                 * the umbilical process to clean up the process group. */

                [ChildTermination_Terminate] = (struct ChildSignalPlan[])
                {
                    { self->mPid, SIGTERM },
                    { self->mPid, SIGKILL },
                    { Pid(0) }
                },

                /* Choose to send SIGABRT in the case that the tether
                 * connection has been inactive past the timeout period.
                 * The implication here is that the child might be
                 * stuck and unable to produce output, so a core file
                 * might be useful to diagnose the situation. */

                [ChildTermination_Abort] = (struct ChildSignalPlan[])
                {
                    { self->mPid, SIGABRT },
                    { self->mPid, SIGKILL },
                    { Pid(0) }
                },
            },
        },

        .mUmbilical =
        {
            .mFile       = aUmbilicalFile,
            .mPid        = aUmbilicalProcess->mPid,
            .mPreempt    = false,
            .mCycleCount = timeoutCycles,
            .mCycleLimit = timeoutCycles,
        },

        .mTether =
        {
            .mCycleCount = 0,
            .mCycleLimit = timeoutCycles,
        },

        /* Experiments at http://www.greenend.org.uk/rjk/tech/poll.html show
         * that it is best not to put too much trust in POLLHUP vs POLLIN,
         * and to treat the presence of either as a trigger to attempt to
         * read from the file descriptor.
         *
         * For the writing end of the pipe, Linux returns POLLERR if the
         * far end reader is no longer available (to match EPIPE), but
         * the documentation suggests that POLLHUP might also be reasonable
         * in this context. */

        .mPollFds =
        {
            [POLL_FD_CHILD_PARENT] =
            {
                .fd     = aParentPipe ? aParentPipe->mRdFile->mFd : -1,
                .events = aParentPipe ? POLL_DISCONNECTEVENT : 0,
            },

            [POLL_FD_CHILD_UMBILICAL] =
            {
                .fd     = aUmbilicalFile->mFd,
                .events = POLL_INPUTEVENTS,
            },

            [POLL_FD_CHILD_EVENT_PIPE] =
            {
                .fd     = eventPipe->mPipe->mRdFile->mFd,
                .events = POLL_INPUTEVENTS,
            },

            [POLL_FD_CHILD_TETHER] =
            {
                .fd     = tetherThread->mControlPipe->mWrFile->mFd,
                .events = POLL_DISCONNECTEVENT,
            },
        },

        .mPollFdActions =
        {
            [POLL_FD_CHILD_UMBILICAL]  = {
                PollFdCallbackMethod(pollFdUmbilical_, &childMonitor_) },
            [POLL_FD_CHILD_PARENT]     = {
                PollFdCallbackMethod(pollFdParent_, &childMonitor_) },
            [POLL_FD_CHILD_EVENT_PIPE] = {
                PollFdCallbackMethod(pollFdEventPipe_, &childMonitor_) },
            [POLL_FD_CHILD_TETHER]     = {
                PollFdCallbackMethod(pollFdTether_, &childMonitor_) },
        },

        .mPollFdTimerActions =
        {
            [POLL_FD_CHILD_TIMER_TETHER] =
            {
                /* Note that a zero value for gOptions.mTimeout.mTether_s will
                 * disable the tether timeout in which case the watchdog will
                 * supervise the child, but not impose any timing requirements
                 * on activity on the tether. */

                .mAction = PollFdCallbackMethod(
                    pollFdTimerTether_, &childMonitor_),
                .mSince  = EVENTCLOCKTIME_INIT,
                .mPeriod = Duration(NanoSeconds(
                    NSECS(Seconds(gOptions.mTether
                                  ? gOptions.mTimeout.mTether_s
                                  : 0)).ns / timeoutCycles)),
            },

            [POLL_FD_CHILD_TIMER_UMBILICAL] =
            {
                .mAction = PollFdCallbackMethod(
                    pollFdTimerUmbilical_, &childMonitor_),
                .mSince  = EVENTCLOCKTIME_INIT,
                .mPeriod = Duration(
                    NanoSeconds(
                        NSECS(Seconds(gOptions.mTimeout.mUmbilical_s)).ns / 2)),
            },

            [POLL_FD_CHILD_TIMER_TERMINATION] =
            {
                .mAction = PollFdCallbackMethod(
                    pollFdTimerTermination_, &childMonitor_),
                .mSince  = EVENTCLOCKTIME_INIT,
                .mPeriod = Duration(NanoSeconds(0)),
            },

            [POLL_FD_CHILD_TIMER_DISCONNECTION] =
            {
                .mAction = PollFdCallbackMethod(
                    pollFdTimerChild_, &childMonitor_),
                .mSince  = EVENTCLOCKTIME_INIT,
                .mPeriod = Duration(NanoSeconds(0)),
            },
        },
    };
    childMonitor = &childMonitor_;

    if ( ! gOptions.mTether)
        disconnectPollFdTether_(childMonitor);

    /* Make the umbilical timer expire immediately so that the umbilical
     * process is activated to monitor the watchdog. */

    struct PollFdTimerAction *umbilicalTimer =
        &childMonitor->mPollFdTimerActions[POLL_FD_CHILD_TIMER_UMBILICAL];

    lapTimeTrigger(&umbilicalTimer->mSince, umbilicalTimer->mPeriod, 0);

    /* It is unfortunate that O_NONBLOCK is an attribute of the underlying
     * open file, rather than of each file descriptor. Since stdin and
     * stdout are typically inherited from the parent, setting O_NONBLOCK
     * would affect all file descriptors referring to the same open file,
     so this approach cannot be employed directly. */

    for (size_t ix = 0; NUMBEROF(childMonitor->mPollFds) > ix; ++ix)
    {
        ERROR_UNLESS(
            ownFdNonBlocking(childMonitor->mPollFds[ix].fd),
            {
                warn(
                    0,
                    "Expected %s fd %d to be non-blocking",
                    pollFdNames_[ix],
                    childMonitor->mPollFds[ix].fd);
            });
    }

    ERROR_IF(
        createPollFd(
            &pollfd_,

            childMonitor->mPollFds,
            childMonitor->mPollFdActions,
            pollFdNames_, POLL_FD_CHILD_KINDS,

            childMonitor->mPollFdTimerActions,
            pollFdTimerNames_, POLL_FD_CHILD_TIMER_KINDS,

            PollFdCompletionMethod(pollFdCompletion_, childMonitor)));
    pollfd = &pollfd_;

    updateChildProcessMonitor_(self, childMonitor);

    ERROR_IF(
        runPollFdLoop(pollfd));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printChildProcess, self);

        updateChildProcessMonitor_(self, 0);

        pollfd = closePollFd(pollfd);

        ABORT_IF(
            EventLatchSettingError == bindEventLatchPipe(
                self->mUmbilicalLatch, 0));

        ABORT_IF(
            EventLatchSettingError == bindEventLatchPipe(
                self->mChildLatch, 0));

        eventPipe = closeEventPipe(eventPipe);
        contLatch = closeEventLatch(contLatch);

        tetherThread = closeTetherThread(tetherThread);

        nullPipe = closePipe(nullPipe);
    });

    debug(0, "stop monitoring child");

    return rc;
}

/* -------------------------------------------------------------------------- */
