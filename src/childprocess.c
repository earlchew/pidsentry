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

#include "options_.h"

#include "ert/bellsocketpair.h"
#include "ert/process.h"
#include "ert/fdset.h"

#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>


/* -------------------------------------------------------------------------- */
enum PollFdChildKind
{
    POLL_FD_CHILD_TETHER,
    POLL_FD_CHILD_UMBILICAL,
    POLL_FD_CHILD_PARENT,
    POLL_FD_CHILD_EVENTPIPE,
    POLL_FD_CHILD_KINDS
};

static const char *pollFdNames_[POLL_FD_CHILD_KINDS] =
{
    [POLL_FD_CHILD_TETHER]    = "tether",
    [POLL_FD_CHILD_UMBILICAL] = "umbilical",
    [POLL_FD_CHILD_PARENT]    = "parent",
    [POLL_FD_CHILD_EVENTPIPE] = "event pipe",
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

    self->mPid  = Ert_Pid(0);
    self->mPgid = Ert_Pgid(0);

    self->mShellCommand     = 0;
    self->mTetherPipe       = 0;
    self->mLatch.mChild     = 0;
    self->mLatch.mUmbilical = 0;

    self->mChildMonitor.mMutex   = 0;
    self->mChildMonitor.mMonitor = 0;

    ERROR_IF(
        ert_createEventLatch(&self->mLatch.mChild_, "child"));
    self->mLatch.mChild = &self->mLatch.mChild_;

    ERROR_IF(
        ert_createEventLatch(&self->mLatch.mUmbilical_, "umbilical"));
    self->mLatch.mUmbilical = &self->mLatch.mUmbilical_;

    self->mChildMonitor.mMutex = ert_createThreadSigMutex(
        &self->mChildMonitor.mMutex_);

    /* Only the reading end of the tether is marked non-blocking. The
     * writing end must be used by the child process (and perhaps inherited
     * by any subsequent process that it forks), so only the reading
     * end is marked non-blocking. */

    ERROR_IF(
        ert_createPipe(&self->mTetherPipe_, O_CLOEXEC | O_NONBLOCK));
    self->mTetherPipe = &self->mTetherPipe_;

    ERROR_IF(
        ert_closeFileOnExec(self->mTetherPipe->mWrFile, 0));

    ERROR_IF(
        ert_nonBlockingFile(self->mTetherPipe->mWrFile, 0));

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            self->mTetherPipe          = ert_closePipe(self->mTetherPipe);
            self->mChildMonitor.mMutex =
                ert_destroyThreadSigMutex(self->mChildMonitor.mMutex);

            self->mLatch.mUmbilical =
                ert_closeEventLatch(self->mLatch.mUmbilical);
            self->mLatch.mChild     =
                ert_closeEventLatch(self->mLatch.mChild);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
printChildProcess(const struct ChildProcess *self, FILE *aFile)
{
    return fprintf(aFile,
                   "<child %p pid %" PRId_Ert_Pid " pgid %" PRId_Ert_Pgid ">",
                   self,
                   FMTd_Ert_Pid(self->mPid),
                   FMTd_Ert_Pgid(self->mPgid));
}

/* -------------------------------------------------------------------------- */
static struct Ert_ChildProcessState
superviseChildProcess_(const struct ChildProcess *self,
                       const char                *aRole,
                       struct Ert_Pid             aPid,
                       struct Ert_EventLatch     *aLatch)
{
    int rc = -1;

    struct Ert_ChildProcessState processState;

    /* Check that the process being monitored is the one
     * is the subject of the signal. Here is a way for a parent
     * to be surprised by the presence of an adopted child:
     *
     *  sleep 5 & exec sh -c 'sleep 1 & wait'
     *
     * The new shell inherits the earlier sleep as a child even
     * though it did not create it. */

    ERROR_IF(
        (processState = ert_monitorProcessChild(aPid),
         Ert_ChildProcessStateError == processState.mChildState));

    if (Ert_ChildProcessStateRunning == processState.mChildState)
    {
        debug(
            1,
            "%s pid %" PRId_Ert_Pid " running",
            aRole,
            FMTd_Ert_Pid(aPid));

        ERROR_IF(
            Ert_EventLatchSettingError == ert_setEventLatch(aLatch));
    }
    else if (Ert_ChildProcessStateStopped == processState.mChildState ||
             Ert_ChildProcessStateTrapped == processState.mChildState)
    {
        debug(
            1,
            "%s pid %" PRId_Ert_Pid " state %" PRIs_Ert_ChildProcessState,
            aRole,
            FMTd_Ert_Pid(aPid),
            FMTs_Ert_ChildProcessState(processState));
    }
    else
    {
        struct Ert_ProcessSignalName sigName;

        switch (processState.mChildState)
        {
        default:
            debug(
                1,
                "%s " "pid %" PRId_Ert_Pid
                " state %" PRIs_Ert_ChildProcessState,
                aRole,
                FMTd_Ert_Pid(aPid),
                FMTs_Ert_ChildProcessState(processState));
            break;

        case Ert_ChildProcessStateExited:
            debug(
                1,
                "%s "
                "pid %" PRId_Ert_Pid " "
                "state %" PRIs_Ert_ChildProcessState " "
                "status %d",
                aRole,
                FMTd_Ert_Pid(aPid),
                FMTs_Ert_ChildProcessState(processState),
                processState.mChildStatus);
            break;

        case Ert_ChildProcessStateKilled:
            debug(
                1,
                "%s "
                "pid %" PRId_Ert_Pid " "
                "state %" PRIs_Ert_ChildProcessState " "
                "killed by %s",
                aRole,
                FMTd_Ert_Pid(aPid),
                FMTs_Ert_ChildProcessState(processState),
                ert_formatProcessSignalName(
                    &sigName, processState.mChildStatus));
            break;
        }

        ERROR_IF(
            Ert_EventLatchSettingError == ert_disableEventLatch(aLatch));
    }

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc,
                        self, printChildProcess,
                        "role %s pid %" PRId_Ert_Pid,
                        aRole, FMTd_Ert_Pid(aPid));

        if (rc)
            processState.mChildStatus = Ert_ChildProcessStateError;
    });

    return processState;
}

int
superviseChildProcess(struct ChildProcess *self, struct Ert_Pid aUmbilicalPid)
{
    int rc = -1;

    struct Ert_ChildProcessState processState;

    if (aUmbilicalPid.mPid)
        ERROR_IF(
            (processState = superviseChildProcess_(
                self, "umbilical", aUmbilicalPid, self->mLatch.mUmbilical),
             Ert_ChildProcessStateError == processState.mChildStatus));

    ERROR_IF(
        (processState = superviseChildProcess_(
            self, "child", self->mPid, self->mLatch.mChild),
         Ert_ChildProcessStateError == processState.mChildStatus));

    /* If the monitored child process has been killed by SIGQUIT and
     * dumped core, then dump core in sympathy. */

    if (Ert_ChildProcessStateDumped == processState.mChildState &&
        SIGQUIT == processState.mChildStatus)
    {
        ert_quitProcess();
    }

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcess);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
killChildProcess(struct ChildProcess *self, int aSigNum)
{
    int rc = -1;

    struct Ert_ProcessSignalName sigName;

    ensure(self->mPid.mPid);

    debug(
        0,
        "sending %s to child pid %" PRId_Ert_Pid,
        ert_formatProcessSignalName(&sigName, aSigNum),
        FMTd_Ert_Pid(self->mPid));

    ERROR_IF(
        kill(self->mPid.mPid, aSigNum));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcess,
                        "signal %s",
                        ert_formatProcessSignalName(&sigName, aSigNum));
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
killChildProcessGroup(struct ChildProcess *self)
{
    int rc = -1;

    ERROR_IF(
        ert_signalProcessGroup(self->mPgid, SIGKILL));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc,
                        self, printChildProcess,
                        "child pgid %" PRId_Ert_Pgid,
                        FMTd_Ert_Pgid(self->mPgid));
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
                        self, printChildProcess,
                        "child pgid %" PRId_Ert_Pgid,
                        FMTd_Ert_Pgid(self->mPgid));
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
                        self, printChildProcess,
                        "child pgid %" PRId_Ert_Pgid,
                        FMTd_Ert_Pgid(self->mPgid));
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct ForkChildProcess_
{
    struct ChildProcess       *mChildProcess;
    const char * const        *mCmd;
    struct Ert_BellSocketPair *mSyncSocket;
    struct Ert_SocketPair     *mUmbilicalSocket;
};

static ERT_CHECKED int
runChildProcess_(struct ForkChildProcess_ *self)
{
    int rc = -1;

    struct ShellCommand  shellCommand_;
    struct ShellCommand *shellCommand = 0;

    debug(
        0,
        "starting child process pid %" PRId_Ert_Pid,
        FMTd_Ert_Pid(ert_ownProcessId()));

    unsigned cmdLen = self->mChildProcess->mShellCommand->mArgList->mArgc;

    const char *cmd[cmdLen+1];

    for (size_t ix = 0; ix <= cmdLen; ++ix)
        cmd[ix] = self->mChildProcess->mShellCommand->mArgList->mArgv[ix];

    int err;

    do
    {
        /* The forked child has all its signal handlers reset, but
         * note that the parent will wait for the child to synchronise
         * before sending it signals, so that there is no race here.
         *
         * There is no need to manipulate the umbilical socket
         * within the contex of the child. */

        self->mUmbilicalSocket = ert_closeSocketPair(self->mUmbilicalSocket);

        /* Wait until the parent has created the pidfile. This
         * invariant can be used to determine if the pidfile
         * is really associated with the process possessing
         * the specified pid. */

        debug(0, "synchronising child process");

        ert_closeBellSocketPairParent(self->mSyncSocket);

        err = 0;
        ERT_TEST_RACE
        ({
            if ( ! err)
                ERROR_IF(
                    (err = ert_waitBellSocketPairChild(self->mSyncSocket, 0),
                     err && EPIPE != errno && ENOENT != errno));

            if ( ! err)
                ERROR_IF(
                    (err = ert_ringBellSocketPairChild(self->mSyncSocket),
                     err && EPIPE != errno));
        });
        if (err)
            break;

        do
        {
            /* Close the reading end of the tether pipe separately
             * because it might turn out that the writing end
             * will not need to be duplicated. */

            ert_closePipeReader(self->mChildProcess->mTetherPipe);

            if (gOptions.mServer.mTether)
            {
                int tetherFd = *gOptions.mServer.mTether;

                if (0 > tetherFd)
                    tetherFd = self->mChildProcess->mTetherPipe->mWrFile->mFd;

                char tetherArg[sizeof(int) * CHAR_BIT + 1];

                ERROR_IF(
                    0 > sprintf(tetherArg, "%d", tetherFd));

                if (gOptions.mServer.mName)
                {
                    bool useEnv = isupper(
                        (unsigned char) gOptions.mServer.mName[0]);

                    for (unsigned ix = 1;
                         useEnv && gOptions.mServer.mName[ix];
                         ++ix)
                    {
                        unsigned char ch = gOptions.mServer.mName[ix];

                        if ( ! isupper(ch) && ! isdigit(ch) && ch != '_')
                            useEnv = false;
                    }

                    if (useEnv)
                    {
                        ERROR_IF(
                            setenv(gOptions.mServer.mName, tetherArg, 1));
                    }
                    else
                    {
                        /* Start scanning from the first argument, leaving
                         * the command name intact. */

                        char *matchArg = 0;

                        for (unsigned ix = 1; cmd[ix]; ++ix)
                        {
                            matchArg = strstr(cmd[ix], gOptions.mServer.mName);

                            if (matchArg)
                            {
                                char replacedArg[
                                    strlen(cmd[ix])                -
                                    strlen(gOptions.mServer.mName) +
                                    strlen(tetherArg)              + 1];

                                int matchLen = matchArg - cmd[ix];

                                ERROR_UNLESS(
                                    cmd[ix] + matchLen == matchArg,
                                    {
                                        errno = ERANGE;
                                    });

                                ERROR_IF(
                                  0 > sprintf(
                                    replacedArg,
                                    "%.*s%s%s",
                                    matchLen,
                                    cmd[ix],
                                    tetherArg,
                                    matchArg + strlen(gOptions.mServer.mName)));

                                char *dupArg = 0;
                                ERROR_UNLESS(
                                    dupArg = strdup(replacedArg),
                                    {
                                        terminate(
                                            errno,
                                            "Unable to duplicate '%s'",
                                            replacedArg);
                                    });

                                cmd[ix] = dupArg;

                                break;
                            }
                        }

                        ERROR_UNLESS(
                            matchArg,
                            {
                                terminate(
                                    0,
                                    "Unable to find matching argument '%s'",
                                    gOptions.mServer.mName);
                            });
                    }
                }

                if (tetherFd == self->mChildProcess->mTetherPipe->mWrFile->mFd)
                    break;

                ERROR_IF(
                    ert_duplicateFd(
                        self->mChildProcess->mTetherPipe->mWrFile->mFd,
                         tetherFd) != tetherFd);
            }

            self->mChildProcess->mTetherPipe = ert_closePipe(
                self->mChildProcess->mTetherPipe);

        } while (0);

        ERROR_IF(
            createShellCommand(&shellCommand_, cmd));
        shellCommand = &shellCommand_;

        /* Wait until the watchdog has had a chance to announce the
         * child pid before proceeding. This allows external programs,
         * notably the unit test, to know that the child process
         * is fully initialised. */

        ERT_TEST_RACE
        ({
            ERROR_IF(
                (err = ert_waitBellSocketPairChild(self->mSyncSocket, 0),
                 err && EPIPE != errno && ENOENT != errno));
        });
        if (err)
            break;

        /* Rely on the upcoming exec() to provide the final synchronisation
         * indication to the waiting watchdog. The watchdog relies on this
         * to know that the child will no longer share any file descriptors
         * and locks with the parent. */

        ensure(
            ert_ownFileCloseOnExec(
                self->mSyncSocket->mSocketPair->mChildSocket->mSocket->mFile));

        debug(0, "child process synchronised");

        /* The child process does not close the process lock because it
         * might need to emit a diagnostic if execProcess() fails. Rely on
         * O_CLOEXEC to close the underlying file descriptors. */

        execShellCommand(shellCommand);
        message(errno,
                "Unable to execute '%s'", ownShellCommandText(shellCommand));

    } while (0);

    rc = EXIT_FAILURE;

Finally:

    FINALLY
    ({
        shellCommand = closeShellCommand(shellCommand);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
forkChildProcess(
    struct ChildProcess       *self,
    const char * const        *aCmd,
    struct Ert_BellSocketPair *aSyncSocket,
    struct Ert_SocketPair     *aUmbilicalSocket)
{
    int rc = -1;

    ensure( ! self->mPid.mPid);
    ensure( ! self->mPgid.mPgid);
    ensure( ! self->mShellCommand);

    ERROR_IF(
        createShellCommand(&self->mShellCommand_, aCmd));
    self->mShellCommand = &self->mShellCommand_;

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
        .mSyncSocket      = aSyncSocket,
        .mUmbilicalSocket = aUmbilicalSocket,
    };

    struct Ert_Pid childPid;
    ERROR_IF(
        (childPid = ert_forkProcessChild(
            Ert_ForkProcessSetProcessGroup,
            Ert_Pgid(0),
            Ert_PreForkProcessMethod(
                &childProcess,
                ERT_LAMBDA(
                    int, (struct ForkChildProcess_        *self_,
                          const struct Ert_PreForkProcess *aPreFork),
                    {
                        return ert_fillFdSet(aPreFork->mWhitelistFds);
                    })),
            Ert_PostForkChildProcessMethodNil(),
            Ert_PostForkParentProcessMethodNil(),
            Ert_ForkProcessMethod(
                &childProcess, runChildProcess_)),
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
    self->mPgid = ert_fetchProcessGroupId(self->mPid);

    debug(
        0,
        "running child pid %" PRId_Ert_Pid " in pgid %" PRId_Ert_Pgid,
        FMTd_Ert_Pid(self->mPid),
        FMTd_Ert_Pgid(self->mPgid));

    ensure(self->mPid.mPid == self->mPgid.mPgid);

    /* Beware of the inherent race here between the child starting and
     * terminating, and the recording of the child pid. To cover the
     * case that the child might have terminated before the child pid
     * is recorded, force a supervision run after the pid is recorded. */

    ERROR_IF(
        superviseChildProcess(self, Ert_Pid(0)));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcess);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closeChildProcessTether(struct ChildProcess *self)
{
    int rc = -1;

    ensure(self->mTetherPipe);

    self->mTetherPipe = ert_closePipe(self->mTetherPipe);

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcess);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
closeChildFiles_(struct ChildProcess *self)
{
    self->mTetherPipe = ert_closePipe(self->mTetherPipe);
}

/* -------------------------------------------------------------------------- */
int
reapChildProcess(struct ChildProcess *self, int *aStatus)
{
    int rc = -1;

    ERROR_IF(
        ert_reapProcessChild(self->mPid, aStatus));

    /* Once the child process is reaped, the process no longer exists, so
     * the pid should no longer be used to refer to it. */

    self->mPid = Ert_Pid(0);

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcess);
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
            ert_destroyThreadSigMutex(self->mChildMonitor.mMutex);

        closeChildFiles_(self);

        self->mLatch.mUmbilical = ert_closeEventLatch(self->mLatch.mUmbilical);
        self->mLatch.mChild     = ert_closeEventLatch(self->mLatch.mChild);

        self->mShellCommand = closeShellCommand(self->mShellCommand);
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
    struct Ert_Pid mPid;
    int            mSig;
};

struct ChildMonitor
{
    struct Ert_Pid mChildPid;

    struct TetherThread   *mTetherThread;
    struct Ert_EventPipe  *mEventPipe;
    struct Ert_EventLatch *mContLatch;

    struct
    {
        const struct ChildSignalPlan *mSignalPlans[ChildTermination_Actions];
        const struct ChildSignalPlan *mSignalPlan;
        struct Ert_Duration           mSignalPeriod;
    } mTermination;

    struct
    {
        struct Ert_File *mFile;
        struct Ert_Pid   mPid;
        bool             mPreempt;       /* Request back-to-back pings */
        unsigned         mCycleCount;    /* Current number of cycles */
        unsigned         mCycleLimit;    /* Cycles before triggering */
    } mUmbilical;

    struct
    {
        unsigned mCycleCount;       /* Current number of cycles */
        unsigned mCycleLimit;       /* Cycles before triggering */
    } mTether;

    struct
    {
        bool mChildLatchDisabled;
        bool mUmbilicalLatchDisabled;
    } mEvent;

    struct
    {
        struct Ert_Pid   mPid;
        struct Ert_Pipe *mPipe;
    } mParent;

    struct pollfd                mPollFds[POLL_FD_CHILD_KINDS];
    struct Ert_PollFdAction      mPollFdActions[POLL_FD_CHILD_KINDS];
    struct Ert_PollFdTimerAction mPollFdTimerActions[POLL_FD_CHILD_TIMER_KINDS];
};

/* -------------------------------------------------------------------------- */
int
printChildProcessMonitor(const struct ChildMonitor *self, FILE *aFile)
{
    return fprintf(aFile,
                   "<child monitor %p pid %" PRId_Ert_Pid ">",
                   self,
                   FMTd_Ert_Pid(self->mChildPid));
}

/* -------------------------------------------------------------------------- */
/* Child Termination State Machine
 *
 * When it is necessary to terminate the child process, run a state
 * machine to sequence through a signal plan that walks through
 * an escalating series of signals. */

static void
activateFdTimerTermination_(struct ChildMonitor             *self,
                            enum ChildTerminationAction      aAction,
                            const struct Ert_EventClockTime *aPollTime)
{
    /* When it is necessary to terminate the child process, the child
     * process might already have terminated. No special action is
     * taken with the expectation that the termination code should
     * fully expect that child the terminate at any time */

    struct Ert_PollFdTimerAction *tetherTimer =
        &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_TETHER];

    tetherTimer->mPeriod = Ert_ZeroDuration;

    struct Ert_PollFdTimerAction *terminationTimer =
        &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_TERMINATION];

    if ( ! terminationTimer->mPeriod.duration.ns)
    {
        debug(1, "activating termination timer");

        ensure( ! self->mTermination.mSignalPlan);

        self->mTermination.mSignalPlan =
            self->mTermination.mSignalPlans[aAction];

        terminationTimer->mPeriod = self->mTermination.mSignalPeriod;

        ert_lapTimeTrigger(
            &terminationTimer->mSince, terminationTimer->mPeriod, aPollTime);
    }
}

static ERT_CHECKED int
pollFdTimerTermination_(struct ChildMonitor             *self,
                        const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    /* Remember that this function races termination of the child process.
     * The child process might have terminated by the time this function
     * attempts to deliver the next signal. This should be handled
     * correctly because the child process will remain as a zombie
     * and signals will be delivered successfully, but without effect. */

    struct Ert_Pid pidNum = self->mTermination.mSignalPlan->mPid;
    int            sigNum = self->mTermination.mSignalPlan->mSig;

    if (self->mTermination.mSignalPlan[1].mSig)
        ++self->mTermination.mSignalPlan;

    struct Ert_ProcessSignalName sigName;

    warn(
        0,
        "Killing child pid %" PRId_Ert_Pid " with %s",
        FMTd_Ert_Pid(pidNum),
        ert_formatProcessSignalName(&sigName, sigNum));

    ERROR_IF(
        kill(pidNum.mPid, sigNum));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcessMonitor);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
/* Maintain Parent Connection
 *
 * This connection allows for monitoring ofthe parent. The child will
 * terminate if the parent terminates. */

static ERT_CHECKED int
pollFdParent_(struct ChildMonitor             *self,
              const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    warn(
        0,
        "Parent pid %" PRId_Ert_Pid " has terminated",
        FMTd_Ert_Pid(self->mParent.mPid));

    self->mPollFds[POLL_FD_CHILD_PARENT].fd     = -1;
    self->mPollFds[POLL_FD_CHILD_PARENT].events = 0;

    activateFdTimerTermination_(self, ChildTermination_Terminate, aPollTime);

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcessMonitor);
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
restartFdTimerUmbilical_(struct ChildMonitor             *self,
                         const struct Ert_EventClockTime *aPollTime)
{
    if (self->mUmbilical.mCycleCount != self->mUmbilical.mCycleLimit)
    {
        ensure(self->mUmbilical.mCycleCount < self->mUmbilical.mCycleLimit);

        self->mUmbilical.mCycleCount = 0;

        ert_lapTimeRestart(
            &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_UMBILICAL].mSince,
            aPollTime);
    }
}

static void
pollFdCloseUmbilical_(struct ChildMonitor             *self,
                      const struct Ert_EventClockTime *aPollTime)
{
    struct Ert_PollFdTimerAction *umbilicalTimer =
        &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_UMBILICAL];

    self->mPollFds[POLL_FD_CHILD_UMBILICAL].fd     = -1;
    self->mPollFds[POLL_FD_CHILD_UMBILICAL].events = 0;

    /* Since the umbilical connection is no longer being monitored, there
     * is no reason to run its associated timer. */

    umbilicalTimer->mPeriod = Ert_ZeroDuration;

    activateFdTimerTermination_(self, ChildTermination_Terminate, aPollTime);
}

static ERT_CHECKED int
pollFdUmbilical_(struct ChildMonitor             *self,
                 const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    struct Ert_PollFdTimerAction *umbilicalTimer =
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
            ert_lapTimeRestart(&umbilicalTimer->mSince, aPollTime);
        else
        {
            self->mUmbilical.mPreempt = false;

            ert_lapTimeTrigger(&umbilicalTimer->mSince,
                               umbilicalTimer->mPeriod, aPollTime);
        }
    }

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcessMonitor);
    });

    return rc;
}

static ERT_CHECKED int
pollFdWriteUmbilical_(struct ChildMonitor *self)
{
    int rc = -1;

    ensure(self->mUmbilical.mCycleCount == self->mUmbilical.mCycleLimit);

    char buf[1] = { '.' };

    ssize_t wrlen;
    ERROR_IF(
        (wrlen = write(
            self->mUmbilical.mFile->mFd, buf, sizeof(buf)),
         -1 == wrlen || (errno = EIO, sizeof(buf) != wrlen)));

    debug(0, "sent umbilical ping");

    /* Once a message is written on the umbilical connection, expect
     * an echo to be returned from the umbilical monitor. */

    self->mUmbilical.mCycleCount = 0;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static ERT_CHECKED int
pollFdReapUmbilicalEvent_(struct ChildMonitor             *self,
                          bool                             aEnabled,
                          const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    if (aEnabled)
    {
        /* The umbilical process is running again after being stopped for
         * some time. Restart the tether timeout so that the stoppage
         * is not mistaken for a failure. */

        debug(
            0,
            "umbilical pid %" PRId_Ert_Pid " is running",
            FMTd_Ert_Pid(self->mUmbilical.mPid));

        restartFdTimerUmbilical_(self, aPollTime);
    }
    else
    {
        /* The umbilical process has terminated, so there is no longer
         * any need to monitor for SIGCHLD. */

        debug(
            0,
            "umbilical pid %" PRId_Ert_Pid " has terminated",
            FMTd_Ert_Pid(self->mUmbilical.mPid));

        self->mEvent.mUmbilicalLatchDisabled = true;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static ERT_CHECKED int
pollFdContUmbilical_(struct ChildMonitor             *self,
                     const struct Ert_EventClockTime *aPollTime)
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

        struct Ert_PollFdTimerAction *umbilicalTimer =
            &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_UMBILICAL];

        ert_lapTimeTrigger(&umbilicalTimer->mSince,
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

static ERT_CHECKED int
pollFdTimerUmbilical_(struct ChildMonitor             *self,
                      const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    if (self->mUmbilical.mCycleCount != self->mUmbilical.mCycleLimit)
    {
        ensure(self->mUmbilical.mCycleCount < self->mUmbilical.mCycleLimit);

        /* If waiting on a response from the umbilical monitor, apply
         * a timeout, and if the timeout is exceeded terminate the
         * child process. */

        struct Ert_ChildProcessState umbilicalState;
        ERROR_IF(
            (umbilicalState = ert_monitorProcessChild(self->mUmbilical.mPid),
             Ert_ChildProcessStateError == umbilicalState.mChildState &&
             ECHILD != errno));

        /* Beware that the umbilical process might no longer be active.
         * If so, do nothing here, and rely on subsequent brokn umbilical
         * connection to trigger action. */

        if (Ert_ChildProcessStateError != umbilicalState.mChildState)
        {
            if (Ert_ChildProcessStateTrapped == umbilicalState.mChildState ||
                Ert_ChildProcessStateStopped == umbilicalState.mChildState)
            {
                debug(
                    0,
                    "deferred timeout umbilical status %"
                    PRIs_Ert_ChildProcessState,
                    FMTs_Ert_ChildProcessState(umbilicalState));

                self->mUmbilical.mCycleCount = 0;
            }
            else
            {
                if (++self->mUmbilical.mCycleCount ==
                    self->mUmbilical.mCycleLimit)
                {
                    warn(0, "Umbilical connection timed out");

                    pollFdCloseUmbilical_(self, aPollTime);
                }
            }
        }
    }
    else
    {
        int wrErr;
        ERROR_IF(
            (wrErr = pollFdWriteUmbilical_(self),
             -1 == wrErr && (EPIPE       != errno &&
                             EINTR       != errno &&
                             EWOULDBLOCK != errno)));

        if (-1 == wrErr)
        {
            struct Ert_PollFdTimerAction *umbilicalTimer =
                &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_UMBILICAL];

            switch (errno)
            {
            default:
                break;

            case EWOULDBLOCK:
                debug(1, "blocked write to umbilical");
                break;

            case EPIPE:
                /* The umbilical monitor is no longer running and has
                 * closed the umbilical connection. */

                warn(0, "Umbilical connection closed");

                pollFdCloseUmbilical_(self, aPollTime);
                break;

            case EINTR:
                /* Do not loop here on EINTR since it is important
                 * to take care that the monitoring loop is
                 * non-blocking. Instead, mark the timer as expired
                 * for force the monitoring loop to retry immediately. */

                debug(1, "interrupted write to umbilical");

                ert_lapTimeTrigger(&umbilicalTimer->mSince,
                                   umbilicalTimer->mPeriod, aPollTime);
                break;
            }
        }
    }

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcessMonitor);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
/* Process Continuation
 *
 * This method is called soon after the process continues after being
 * stopped to alert the monitoring loop that timers must be re-synchronised
 * to compensate for the outage. */

static ERT_CHECKED int
pollFdContEvent_(struct ChildMonitor             *self,
                 bool                             aEnabled,
                 const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    ensure(aEnabled);

    debug(0, "detected continuation after stoppage");

    ERROR_IF(
        pollFdContUmbilical_(self, aPollTime));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static ERT_CHECKED int
raiseFdContEvent_(struct ChildMonitor *self)
{
    int rc = -1;

    ERROR_IF(
        Ert_EventLatchSettingError == ert_setEventLatch(self->mContLatch));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcessMonitor);
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

static ERT_CHECKED int
pollFdTether_(struct ChildMonitor             *self,
              const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    /* The tether thread control pipe will be closed when the tether
     * between the child process and watchdog is shut down. */

    disconnectPollFdTether_(self);

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcessMonitor);
    });

    return rc;
}

static void
restartFdTimerTether_(struct ChildMonitor             *self,
                      const struct Ert_EventClockTime *aPollTime)
{
    /* If the child process is running without a tether, there will
     * be no active tether timer to restart. */

    struct Ert_PollFdTimerAction *tetherTimer =
        &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_TETHER];

    if (tetherTimer->mPeriod.duration.ns)
    {
        self->mTether.mCycleCount = 0;

        ert_lapTimeRestart(&tetherTimer->mSince, aPollTime);
    }
}

static ERT_CHECKED int
pollFdTimerTether_(struct ChildMonitor             *self,
                   const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    /* The tether timer is only active if there is a tether and it was
     * configured with a timeout. The timeout expires if there was
     * no activity on the tether with the consequence that the monitored
     * child will be terminated. */

    do
    {
        struct Ert_PollFdTimerAction *tetherTimer =
            &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_TETHER];

        struct Ert_ChildProcessState childState;

        ERROR_IF(
            (childState = ert_monitorProcessChild(self->mChildPid),
             Ert_ChildProcessStateError == childState.mChildState &&
             ECHILD != errno));

        /* Be aware if the child process is no longer active, it makes
         * sense to proceed as if the child process should be terminated. */

        if (Ert_ChildProcessStateError != childState.mChildState)
        {
            if (Ert_ChildProcessStateTrapped == childState.mChildState ||
                Ert_ChildProcessStateStopped == childState.mChildState)
            {
                debug(
                    0,
                    "deferred timeout child status %"
                    PRIs_Ert_ChildProcessState,
                    FMTs_Ert_ChildProcessState(childState));

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

                struct Ert_EventClockTime since;
                {
                    pthread_mutex_t *lock =
                        ert_lockMutex(self->mTetherThread->mActivity.mMutex);
                    since = self->mTetherThread->mActivity.mSince;
                    lock = ert_unlockMutex(lock);
                }

                if (aPollTime->eventclock.ns <
                    since.eventclock.ns + tetherTimer->mPeriod.duration.ns)
                {
                    ert_lapTimeRestart(&tetherTimer->mSince, &since);
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

        debug(0, "timeout after %ds", gOptions.mServer.mTimeout.mTether_s);

        activateFdTimerTermination_(
            self, ChildTermination_Abort, aPollTime);

    } while (0);

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcessMonitor);
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
        self->mEvent.mChildLatchDisabled &&
        ! self->mPollFds[POLL_FD_CHILD_TETHER].events;
}

/* -------------------------------------------------------------------------- */
/* Child Termination
 *
 * The watchdog will receive SIGCHLD when the child process terminates,
 * though no direct indication will be received if the child process
 * performs an execv(2). The SIGCHLD signal will be delivered to the
 * event loop on a pipe, at which point the child process is known
 * to be dead. */

static ERT_CHECKED int
pollFdReapChildEvent_(struct ChildMonitor             *self,
                      bool                             aEnabled,
                      const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    if (aEnabled)
    {
        /* The child process is running again after being stopped for
         * some time. Restart the tether timeout so that the stoppage
         * is not mistaken for a failure. */

        debug(
            0,
            "child pid %" PRId_Ert_Pid " is running",
            FMTd_Ert_Pid(self->mChildPid));

        restartFdTimerTether_(self, aPollTime);
    }
    else
    {
        /* The child process has terminated, so there is no longer
         * any need to monitor for SIGCHLD. */

        debug(
            0,
            "child pid %" PRId_Ert_Pid " has terminated",
            FMTd_Ert_Pid(self->mChildPid));

        self->mEvent.mChildLatchDisabled = true;

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
            POLL_FD_CHILD_TIMER_DISCONNECTION].mPeriod = Ert_Duration(
                ERT_NSECS(Ert_Seconds(1)));
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static ERT_CHECKED int
pollFdTimerChild_(struct ChildMonitor             *self,
                  const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    debug(0, "disconnecting tether thread");

    ERROR_IF(
        pingTetherThread(self->mTetherThread));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcessMonitor);
    });

    return rc;

}

/* -------------------------------------------------------------------------- */
/* Event Pipe
 *
 * An event pipe is used to trigger activity on the event loop so that
 * a single rather expensive file descriptor can be used to service
 * multiple events. */

static ERT_CHECKED int
pollFdEventPipe_(struct ChildMonitor             *self,
                 const struct Ert_EventClockTime *aPollTime)
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

    if ( ! ert_testSleep(Ert_TestLevelRace))
    {
        debug(0, "checking event pipe");

        ERROR_IF(
            -1 == ert_pollEventPipe(
                self->mEventPipe, aPollTime) && EINTR != errno);
    }

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcessMonitor);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
updateChildProcessMonitor_(
    struct ChildProcess *self, struct ChildMonitor *aMonitor)
{
    struct Ert_ThreadSigMutex *lock =
        ert_lockThreadSigMutex(self->mChildMonitor.mMutex);

    self->mChildMonitor.mMonitor = aMonitor;

    lock = ert_unlockThreadSigMutex(lock);
}

int
raiseChildProcessSigCont(struct ChildProcess *self)
{
    int rc = -1;

    struct Ert_ThreadSigMutex *lock = 0;

    lock = ert_lockThreadSigMutex(self->mChildMonitor.mMutex);

    if (self->mChildMonitor.mMonitor)
        ERROR_IF(
            raiseFdContEvent_(self->mChildMonitor.mMonitor));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcess);

        lock = ert_unlockThreadSigMutex(lock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
monitorChildProcess(struct ChildProcess     *self,
                    struct UmbilicalProcess *aUmbilicalProcess,
                    struct Ert_File         *aUmbilicalFile,
                    struct Ert_Pid           aParentPid,
                    struct Ert_Pipe         *aParentPipe)
{
    int rc = -1;

    debug(0, "start monitoring child");

    struct Ert_Pipe  nullPipe_;
    struct Ert_Pipe *nullPipe = 0;

    struct TetherThread  tetherThread_;
    struct TetherThread *tetherThread = 0;

    struct Ert_EventLatch  contLatch_;
    struct Ert_EventLatch *contLatch = 0;

    struct Ert_EventPipe  eventPipe_;
    struct Ert_EventPipe *eventPipe = 0;

    struct Ert_PollFd  pollfd_;
    struct Ert_PollFd *pollfd = 0;

    struct ChildMonitor *childMonitor = 0;

    ERROR_IF(
        ert_createPipe(&nullPipe_, O_CLOEXEC | O_NONBLOCK));
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
        ert_createEventPipe(&eventPipe_, O_CLOEXEC | O_NONBLOCK));
    eventPipe = &eventPipe_;

    ERROR_IF(
        ert_createEventLatch(&contLatch_, "continue"));
    contLatch = &contLatch_;

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
            .mChildLatchDisabled     = false,
            .mUmbilicalLatchDisabled = false,
        },

        .mTermination =
        {
            .mSignalPlan   = 0,
            .mSignalPeriod = Ert_Duration(
                ERT_NSECS(Ert_Seconds(gOptions.mServer.mTimeout.mSignal_s))),
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
                    { Ert_Pid(0) }
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
                    { Ert_Pid(0) }
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
                .events = aParentPipe ? ERT_POLL_DISCONNECTEVENT : 0,
            },

            [POLL_FD_CHILD_UMBILICAL] =
            {
                .fd     = aUmbilicalFile->mFd,
                .events = ERT_POLL_INPUTEVENTS,
            },

            [POLL_FD_CHILD_EVENTPIPE] =
            {
                .fd     = eventPipe->mPipe->mRdFile->mFd,
                .events = ERT_POLL_INPUTEVENTS,
            },

            [POLL_FD_CHILD_TETHER] =
            {
                .fd     = tetherThread->mControlPipe->mWrFile->mFd,
                .events = ERT_POLL_DISCONNECTEVENT,
            },
        },

        .mPollFdActions =
        {
            [POLL_FD_CHILD_UMBILICAL]  = {
                Ert_PollFdCallbackMethod(&childMonitor_, pollFdUmbilical_) },
            [POLL_FD_CHILD_PARENT]     = {
                Ert_PollFdCallbackMethod(&childMonitor_, pollFdParent_) },
            [POLL_FD_CHILD_EVENTPIPE] = {
                Ert_PollFdCallbackMethod(&childMonitor_, pollFdEventPipe_) },
            [POLL_FD_CHILD_TETHER]     = {
                Ert_PollFdCallbackMethod(&childMonitor_, pollFdTether_) },
        },

        .mPollFdTimerActions =
        {
            [POLL_FD_CHILD_TIMER_TETHER] =
            {
                /* Note that a zero for gOptions.mServer.mTimeout.mTether_s will
                 * disable the tether timeout in which case the watchdog will
                 * supervise the child, but not impose any timing requirements
                 * on activity on the tether. */

                .mAction = Ert_PollFdCallbackMethod(
                    &childMonitor_, pollFdTimerTether_),
                .mSince  = ERT_EVENTCLOCKTIME_INIT,
                .mPeriod = Ert_Duration(Ert_NanoSeconds(
                    ERT_NSECS(Ert_Seconds(gOptions.mServer.mTether
                                  ? gOptions.mServer.mTimeout.mTether_s
                                  : 0)).ns / timeoutCycles)),
            },

            [POLL_FD_CHILD_TIMER_UMBILICAL] =
            {
                .mAction = Ert_PollFdCallbackMethod(
                    &childMonitor_, pollFdTimerUmbilical_),
                .mSince  = ERT_EVENTCLOCKTIME_INIT,
                .mPeriod = Ert_Duration(
                    Ert_NanoSeconds(
                      ERT_NSECS(
                        Ert_Seconds(
                          gOptions.mServer.mTimeout.mUmbilical_s)).ns / 2)),
            },

            [POLL_FD_CHILD_TIMER_TERMINATION] =
            {
                .mAction = Ert_PollFdCallbackMethod(
                    &childMonitor_, pollFdTimerTermination_),
                .mSince  = ERT_EVENTCLOCKTIME_INIT,
                .mPeriod = Ert_ZeroDuration,
            },

            [POLL_FD_CHILD_TIMER_DISCONNECTION] =
            {
                .mAction = Ert_PollFdCallbackMethod(
                    &childMonitor_, pollFdTimerChild_),
                .mSince  = ERT_EVENTCLOCKTIME_INIT,
                .mPeriod = Ert_ZeroDuration,
            },
        },
    };
    childMonitor = &childMonitor_;

    ERROR_IF(
        Ert_EventLatchSettingError == ert_bindEventLatchPipe(
            self->mLatch.mChild, eventPipe,
            Ert_EventLatchMethod(
                childMonitor, pollFdReapChildEvent_)));

    ERROR_IF(
        Ert_EventLatchSettingError == ert_bindEventLatchPipe(
            self->mLatch.mUmbilical, eventPipe,
            Ert_EventLatchMethod(
                childMonitor, pollFdReapUmbilicalEvent_)));

    ERROR_IF(
        Ert_EventLatchSettingError == ert_bindEventLatchPipe(
            contLatch, eventPipe,
            Ert_EventLatchMethod(
                childMonitor, pollFdContEvent_)));

    if ( ! gOptions.mServer.mTether)
        disconnectPollFdTether_(childMonitor);

    /* Make the umbilical timer expire immediately so that the umbilical
     * process is activated to monitor the watchdog. */

    struct Ert_PollFdTimerAction *umbilicalTimer =
        &childMonitor->mPollFdTimerActions[POLL_FD_CHILD_TIMER_UMBILICAL];

    ert_lapTimeTrigger(&umbilicalTimer->mSince, umbilicalTimer->mPeriod, 0);

    /* It is unfortunate that O_NONBLOCK is an attribute of the underlying
     * open file, rather than of each file descriptor. Since stdin and
     * stdout are typically inherited from the parent, setting O_NONBLOCK
     * would affect all file descriptors referring to the same open file,
     so this approach cannot be employed directly. */

    for (size_t ix = 0; ERT_NUMBEROF(childMonitor->mPollFds) > ix; ++ix)
    {
        ERROR_UNLESS(
            ert_ownFdNonBlocking(childMonitor->mPollFds[ix].fd),
            {
                warn(
                    0,
                    "Expected %s fd %d to be non-blocking",
                    pollFdNames_[ix],
                    childMonitor->mPollFds[ix].fd);
            });
    }

    ERROR_IF(
        ert_createPollFd(
            &pollfd_,

            childMonitor->mPollFds,
            childMonitor->mPollFdActions,
            pollFdNames_, POLL_FD_CHILD_KINDS,

            childMonitor->mPollFdTimerActions,
            pollFdTimerNames_, POLL_FD_CHILD_TIMER_KINDS,

            Ert_PollFdCompletionMethod(childMonitor, pollFdCompletion_)));
    pollfd = &pollfd_;

    updateChildProcessMonitor_(self, childMonitor);

    ERROR_IF(
        ert_runPollFdLoop(pollfd));

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printChildProcess);

        updateChildProcessMonitor_(self, 0);

        pollfd = ert_closePollFd(pollfd);

        ABORT_IF(
            Ert_EventLatchSettingError == ert_unbindEventLatchPipe(
                self->mLatch.mUmbilical));

        ABORT_IF(
            Ert_EventLatchSettingError == ert_unbindEventLatchPipe(
                self->mLatch.mChild));

        contLatch = ert_closeEventLatch(contLatch);
        eventPipe = ert_closeEventPipe(eventPipe);

        tetherThread = closeTetherThread(tetherThread);

        nullPipe = ert_closePipe(nullPipe);
    });

    debug(0, "stop monitoring child");

    return rc;
}

/* -------------------------------------------------------------------------- */
