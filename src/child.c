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

#include "child.h"
#include "umbilical.h"
#include "tether.h"

#include "thread_.h"
#include "fd_.h"
#include "type_.h"
#include "error_.h"
#include "process_.h"
#include "macros_.h"
#include "test_.h"
#include "socketpair_.h"
#include "stdfdfiller_.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

/* -------------------------------------------------------------------------- */
enum PollFdChildKind
{
    POLL_FD_CHILD_TETHER,
    POLL_FD_CHILD_CHILD,
    POLL_FD_CHILD_UMBILICAL,
    POLL_FD_CHILD_KINDS
};

static const char *sPollFdNames[POLL_FD_CHILD_KINDS] =
{
    [POLL_FD_CHILD_TETHER]    = "tether",
    [POLL_FD_CHILD_CHILD]     = "child",
    [POLL_FD_CHILD_UMBILICAL] = "umbilical",
};

/* -------------------------------------------------------------------------- */
enum PollFdChildTimerKind
{
    POLL_FD_CHILD_TIMER_TETHER,
    POLL_FD_CHILD_TIMER_UMBILICAL,
    POLL_FD_CHILD_TIMER_ORPHAN,
    POLL_FD_CHILD_TIMER_TERMINATION,
    POLL_FD_CHILD_TIMER_DISCONNECTION,
    POLL_FD_CHILD_TIMER_KINDS
};

static const char *sPollFdTimerNames[POLL_FD_CHILD_TIMER_KINDS] =
{
    [POLL_FD_CHILD_TIMER_TETHER]        = "tether",
    [POLL_FD_CHILD_TIMER_UMBILICAL]     = "umbilical",
    [POLL_FD_CHILD_TIMER_ORPHAN]        = "orphan",
    [POLL_FD_CHILD_TIMER_TERMINATION]   = "termination",
    [POLL_FD_CHILD_TIMER_DISCONNECTION] = "disconnection",
};

/* -------------------------------------------------------------------------- */
void
createChild(struct ChildProcess *self)
{
    self->mPid = 0;
    self->mPgid = 0;

    /* Only the reading end of the tether is marked non-blocking. The
     * writing end must be used by the child process (and perhaps inherited
     * by any subsequent process that it forks), so only the reading
     * end is marked non-blocking. */

    if (createPipe(&self->mTetherPipe_, 0))
        terminate(
            errno,
            "Unable to create tether pipe");
    self->mTetherPipe = &self->mTetherPipe_;

    if (closeFileOnExec(self->mTetherPipe->mRdFile, O_CLOEXEC))
        terminate(
            errno,
            "Unable to set close on exec for tether");

    if (nonblockingFile(self->mTetherPipe->mRdFile))
        terminate(
            errno,
            "Unable to mark tether non-blocking");

    if (createPipe(&self->mChildPipe_, O_CLOEXEC | O_NONBLOCK))
        terminate(
            errno,
            "Unable to create child pipe");
    self->mChildPipe = &self->mChildPipe_;
}

/* -------------------------------------------------------------------------- */
void
reapChild(struct ChildProcess *self)
{
    /* Check that the child process being monitored is the one
     * is the subject of the signal. Here is a way for a parent
     * to be surprised by the presence of an adopted child:
     *
     *  sleep 5 & exec sh -c 'sleep 1 & wait'
     *
     * The new shell inherits the earlier sleep as a child even
     * though it did not create it. */

    enum ProcessStatus childstatus = monitorProcess(self->mPid);

    if (ProcessStatusError == childstatus)
        terminate(
            errno,
            "Unable to determine status of pid %jd",
            (intmax_t) self->mPid);

    if (ProcessStatusRunning == childstatus)
    {
        /* Only write when the child starts running again. This avoids
         * questionable semantics should the pipe overflow since the
         * only significance of the pipe is whether or not there is content. */

        char buf[1] = { 0 };

        if (-1 == writeFile(self->mChildPipe->mWrFile, buf, sizeof(buf)))
        {
            if (EWOULDBLOCK != errno)
                terminate(
                    errno,
                    "Unable to write status to child pipe");
        }
    }
    else if (ProcessStatusExited != childstatus &&
             ProcessStatusKilled != childstatus &&
             ProcessStatusDumped != childstatus)
    {
        debug(1,
              "child pid %jd status %c",
              (intmax_t) self->mPid, childstatus);
    }
    else
    {
        if (closePipeWriter(self->mChildPipe))
            terminate(
                errno,
                "Unable to close child pipe writer");
    }
}

/* -------------------------------------------------------------------------- */
void
killChild(struct ChildProcess *self, int aSigNum)
{
    if ( ! self->mPid)
        terminate(
            0,
            "Signal race when trying to deliver signal %d", aSigNum);

    debug(0,
          "sending signal %d to child pid %jd",
          aSigNum,
          (intmax_t) self->mPid);

    if (kill(self->mPid, aSigNum))
    {
        if (ESRCH != errno)
            terminate(
                errno,
                "Unable to deliver signal %d to child pid %jd",
                aSigNum,
                (intmax_t) self->mPid);
    }
}

/* -------------------------------------------------------------------------- */
int
forkChild(
    struct ChildProcess  *self,
    char                **aCmd,
    struct StdFdFiller   *aStdFdFiller,
    struct Pipe          *aSyncPipe,
    struct SocketPair    *aUmbilicalSocket)
{
    int rc = -1;

    /* Both the parent and child share the same signal handler configuration.
     * In particular, no custom signal handlers are configured, so
     * signals delivered to either will likely caused them to terminate.
     *
     * This is safe because that would cause one of end the synchronisation
     * pipe to close, and the other end will eventually notice. */

    pid_t childPid = forkProcess(
        gOptions.mSetPgid
        ? ForkProcessSetProcessGroup : ForkProcessShareProcessGroup, 0);

    /* Although it would be better to ensure that the child process and watchdog
     * in the same process group, switching the process group of the watchdog
     * will likely cause a race in an inattentive parent of the watchdog.
     * For example upstart(8) has:
     *
     *    pgid = getpgid(pid);
     *    kill(pgid > 0 ? -pgid : pid, signal);
     */

    FINALLY_IF(
        -1 == childPid);

    if ( ! childPid)
    {
        childPid = getpid();

        debug(0, "starting child process");

        /* The forked child has all its signal handlers reset, but
         * note that the parent will wait for the child to synchronise
         * before sending it signals, so that there is no race here.
         *
         * Close the StdFdFiller in case this will free up stdin, stdout or
         * stderr. The remaining operations will close the remaining
         * unwanted file descriptors. */

        if (closeStdFdFiller(aStdFdFiller))
            terminate(
                errno,
                "Unable to close stdin, stdout and stderr fillers");

        if (closePipe(self->mChildPipe))
            terminate(
                errno,
                "Unable to close child pipe");
        self->mChildPipe = 0;

        /* Wait until the parent has created the pidfile. This
         * invariant can be used to determine if the pidfile
         * is really associated with the process possessing
         * the specified pid. */

        debug(0, "synchronising child process");

        RACE
        ({
            while (true)
            {
                char buf[1];

                switch (read(aSyncPipe->mRdFile->mFd, buf, 1))
                {
                default:
                        break;

                case -1:
                    if (EINTR == errno)
                        continue;
                    terminate(
                        errno,
                        "Unable to synchronise child");
                    break;

                case 0:
                    _exit(EXIT_FAILURE);
                    break;
                }

                break;
            }
        });

        if (closePipe(aSyncPipe))
            terminate(
                errno,
                "Unable to close sync pipe");

        do
        {
            /* Close the reading end of the tether pipe separately
             * because it might turn out that the writing end
             * will not need to be duplicated. */

            if (closePipeReader(self->mTetherPipe))
                terminate(
                    errno,
                    "Unable to close tether pipe reader");

            if (closeSocketPair(aUmbilicalSocket))
                terminate(
                    errno,
                    "Unable to close umbilical socket");
            aUmbilicalSocket = 0;

            if (gOptions.mTether)
            {
                int tetherFd = *gOptions.mTether;

                if (0 > tetherFd)
                    tetherFd = self->mTetherPipe->mWrFile->mFd;

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
                        if (setenv(gOptions.mName, tetherArg, 1))
                            terminate(
                                errno,
                                "Unable to set environment variable '%s'",
                                gOptions.mName);
                    }
                    else
                    {
                        /* Start scanning from the first argument, leaving
                         * the command name intact. */

                        char *matchArg = 0;

                        for (unsigned ix = 1; aCmd[ix]; ++ix)
                        {
                            matchArg = strstr(aCmd[ix], gOptions.mName);

                            if (matchArg)
                            {
                                char replacedArg[
                                    strlen(aCmd[ix])       -
                                    strlen(gOptions.mName) +
                                    strlen(tetherArg)      + 1];

                                sprintf(replacedArg,
                                        "%.*s%s%s",
                                        matchArg - aCmd[ix],
                                        aCmd[ix],
                                        tetherArg,
                                        matchArg + strlen(gOptions.mName));

                                aCmd[ix] = strdup(replacedArg);

                                if ( ! aCmd[ix])
                                    terminate(
                                        errno,
                                        "Unable to duplicate '%s'",
                                        replacedArg);
                                break;
                            }
                        }

                        if ( ! matchArg)
                            terminate(
                                0,
                                "Unable to find matching argument '%s'",
                                gOptions.mName);
                    }
                }

                if (tetherFd == self->mTetherPipe->mWrFile->mFd)
                    break;

                if (dup2(self->mTetherPipe->mWrFile->mFd, tetherFd) != tetherFd)
                    terminate(
                        errno,
                        "Unable to dup tether pipe fd %d to fd %d",
                        self->mTetherPipe->mWrFile->mFd,
                        tetherFd);
            }

            if (closePipe(self->mTetherPipe))
                terminate(
                    errno,
                    "Unable to close tether pipe");

        } while (0);

        debug(0, "child process synchronised");

        /* The child process does not close the process lock because it
         * might need to emit a diagnostic if execvp() fails. Rely on
         * O_CLOEXEC to close the underlying file descriptors. */

        execvp(aCmd[0], aCmd);
        terminate(
            errno,
            "Unable to execute '%s'", aCmd[0]);
    }

    /* Even if the child has terminated, it remains a zombie until reaped,
     * so it is safe to query it to determine its process group. */

    self->mPid  = childPid;
    self->mPgid = gOptions.mSetPgid ? self->mPid : 0;

    debug(0,
          "running child pid %jd in pgid %jd",
          (intmax_t) self->mPid,
          (intmax_t) self->mPgid);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closeChildTether(struct ChildProcess *self)
{
    ensure(self->mTetherPipe);

    if (closePipe(self->mTetherPipe))
        terminate(
            errno,
            "Unable to close tether pipe");
    self->mTetherPipe = 0;
}

/* -------------------------------------------------------------------------- */
static void
closeChildFiles_(struct ChildProcess *self)
{
    if (closePipe(self->mChildPipe))
        terminate(
            errno,
            "Unable to close child pipe");
    self->mChildPipe = 0;

    if (closePipe(self->mTetherPipe))
        terminate(
            errno,
            "Unable to close tether pipe");
    self->mTetherPipe = 0;
}

/* -------------------------------------------------------------------------- */
int
closeChild(struct ChildProcess *self)
{
    closeChildFiles_(self);

    int status;

    if (reapProcess(self->mPid, &status))
        terminate(
            errno,
            "Unable to reap child pid '%jd'",
            (intmax_t) self->mPid);

    return status;
}

/* -------------------------------------------------------------------------- */
void
monitorChildUmbilical(struct ChildProcess *self, pid_t aParentPid)
{
    /* This function is called in the context of the umbilical process
     * to monitor the umbilical, and if the umbilical fails, to kill
     * the child.
     *
     * The caller has already configured stdin to be used to read data
     * from the umbilical pipe.
     */

    closeChildFiles_(self);

    /* The umbilical process is not the parent of the child process being
     * watched, so that there is no reliable way to send a signal to that
     * process alone because the pid might be recycled by the time the signal
     * is sent. Instead rely on the umbilical monitor being in the same
     * process group as the child process and use the process group as
     * a means of controlling the cild process. */

    struct UmbilicalMonitorPoll monitorpoll;
    if (createUmbilicalMonitor(&monitorpoll, STDIN_FILENO, aParentPid))
        terminate(errno, "Unable to create umbilical monitor");

    /* Synchronise with the watchdog to avoid timing races. The watchdog
     * writes to the umbilical when it is ready to start timing. */

    debug(0, "synchronising umbilical");

    if (synchroniseUmbilicalMonitor(&monitorpoll))
        terminate(errno, "Unable to synchronise umbilical monitor");

    debug(0, "synchronised umbilical");

    if (runUmbilicalMonitor(&monitorpoll))
        terminate(errno, "Unable to run umbilical monitor");

    /* The umbilical monitor returns when the connection to the watchdog
     * is either lost or no longer active. */

    pid_t pgid = getpgid(0);

    warn(0, "Killing child pgid %jd", (intmax_t) pgid);

    if (kill(0, SIGKILL))
        terminate(
            errno,
            "Unable to kill child pgid %jd", (intmax_t) pgid);
}

/* -------------------------------------------------------------------------- */
/* Child Process Monitoring
 *
 * The child process must be monitored for activity, and also for
 * termination.
 */

static const struct Type * const childMonitorType_ = TYPE("ChildMonitor");

struct ChildSignalPlan
{
    pid_t mPid;
    int   mSig;
};

struct ChildMonitor
{
    const struct Type *mType;

    pid_t mChildPid;

    struct Pipe         *mNullPipe;
    struct TetherThread *mTetherThread;

    struct
    {
        const struct ChildSignalPlan *mSignalPlan;
        struct Duration               mSignalPeriod;
    } mTermination;

    struct
    {
        struct File *mFile;
    } mUmbilical;

    struct
    {
        unsigned mCycleCount;       /* Current number of cycles */
        unsigned mCycleLimit;       /* Cycles before triggering */
    } mTether;

    struct pollfd            mPollFds[POLL_FD_CHILD_KINDS];
    struct PollFdAction      mPollFdActions[POLL_FD_CHILD_KINDS];
    struct PollFdTimerAction mPollFdTimerActions[POLL_FD_CHILD_TIMER_KINDS];
};

/* -------------------------------------------------------------------------- */
/* Child Termination State Machine
 *
 * When it is necessary to terminate the child process, run a state
 * machine to sequence through a signal plan that walks through
 * an escalating series of signals. */

static void
activateFdTimerTermination_(struct ChildMonitor         *self,
                            const struct EventClockTime *aPollTime)
{
    /* When it is necessary to terminate the child process, the child
     * process might already have terminated. No special action is
     * taken with the expectation that the termination code should
     * fully expect that child the terminate at any time */

    struct PollFdTimerAction *terminationTimer =
        &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_TERMINATION];

    if ( ! terminationTimer->mPeriod.duration.ns)
    {
        debug(1, "activating termination timer");

        terminationTimer->mPeriod =
            self->mTermination.mSignalPeriod;

        lapTimeTrigger(
            &terminationTimer->mSince, terminationTimer->mPeriod, aPollTime);
    }
}

static void
pollFdTimerTermination_(void                        *self_,
                        const struct EventClockTime *aPollTime)
{
    struct ChildMonitor *self = self_;

    ensure(childMonitorType_ == self->mType);

    /* Remember that this function races termination of the child process.
     * The child process might have terminated by the time this function
     * attempts to deliver the next signal. This should be handled
     * correctly because the child process will remain as a zombie
     * and signals will be delivered successfully, but without effect. */

    pid_t pidNum = self->mTermination.mSignalPlan->mPid;
    int   sigNum = self->mTermination.mSignalPlan->mSig;

    if (self->mTermination.mSignalPlan[1].mSig)
        ++self->mTermination.mSignalPlan;

    warn(0, "Killing child pid %jd with signal %d", (intmax_t) pidNum, sigNum);

    if (kill(pidNum, sigNum))
        terminate(
            errno,
            "Unable to kill child pid %jd with signal %d",
            (intmax_t) pidNum,
            sigNum);
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
pollFdUmbilical_(void                        *self_,
                 const struct EventClockTime *aPollTime)
{
    struct ChildMonitor *self = self_;

    debug(0, "umbilical connection closed");

    self->mPollFds[POLL_FD_CHILD_UMBILICAL].events = 0;
    self->mPollFds[POLL_FD_CHILD_UMBILICAL].fd     =
        self->mNullPipe->mRdFile->mFd;

    struct PollFdTimerAction *umbilicalTimer =
        &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_UMBILICAL];

    umbilicalTimer->mPeriod = Duration(NanoSeconds(0));

    struct PollFdTimerAction *tetherTimer =
        &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_TETHER];

    tetherTimer->mPeriod = Duration(NanoSeconds(0));

    activateFdTimerTermination_(self, aPollTime);
}

static int
pollFdWriteUmbilical_(struct ChildMonitor *self)
{
    int rc = -1;

    char buf[1] = { 0 };

    ssize_t wrlen = write(
        self->mUmbilical.mFile->mFd, buf, sizeof(buf));

    if (-1 != wrlen)
        debug(1, "wrote umbilical %zd", wrlen);
    else
    {
        switch (errno)
        {
        default:
            terminate(errno, "Unable to write to umbilical");

        case EPIPE:
            debug(1, "writing to umbilical closed");
            break;

        case EWOULDBLOCK:
            debug(1, "writing to umbilical blocked");
            break;

        case EINTR:
            debug(1, "umbilical write interrupted");
            break;
        }

        goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static void
pollFdContUmbilical_(void *self_)
{
    struct ChildMonitor *self = self_;

    ensure(childMonitorType_ == self->mType);

    /* This function is called when the process receives SIGCONT. The
     * function indicates to the umbilical monitor that the process
     * has just woken, so that the monitor can restart the timeout. */

    pollFdWriteUmbilical_(self);
}

static void
pollFdTimerUmbilical_(void                        *self_,
                      const struct EventClockTime *aPollTime)
{
    struct ChildMonitor *self = self_;

    ensure(childMonitorType_ == self->mType);

    /* Remember that the umbilical timer will race with child termination.
     * By the time this function runs, the child might already have
     * terminated so the umbilical socket might be closed. */

    if (pollFdWriteUmbilical_(self))
    {
        if (EINTR == errno)
        {
            struct PollFdTimerAction *umbilicalTimer =
                &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_UMBILICAL];

            /* Do not loop here on EINTR since it is important
             * to take care that the monitoring loop is
             * non-blocking. Instead, mark the timer as expired
             * for force the monitoring loop to retry immediately. */

            lapTimeTrigger(&umbilicalTimer->mSince,
                           umbilicalTimer->mPeriod, aPollTime);
        }
    }
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

    self->mPollFds[POLL_FD_CHILD_TETHER].fd     = self->mNullPipe->mRdFile->mFd;
    self->mPollFds[POLL_FD_CHILD_TETHER].events = 0;
}

static void
pollFdTether_(void                        *self_,
              const struct EventClockTime *aPollTime)
{
    struct ChildMonitor *self = self_;

    ensure(childMonitorType_ == self->mType);

    /* The tether thread control pipe will be closed when the tether
     * between the child process and watchdog is shut down. */

    disconnectPollFdTether_(self);
}

static void
restartFdTimerTether_(struct ChildMonitor         *self,
                      const struct EventClockTime *aPollTime)
{
    self->mTether.mCycleCount = 0;

    lapTimeRestart(
        &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_TETHER].mSince,
        aPollTime);
}

static void
pollFdTimerTether_(void                        *self_,
                   const struct EventClockTime *aPollTime)
{
    struct ChildMonitor *self = self_;

    ensure(childMonitorType_ == self->mType);

    /* The tether timer is only active if there is a tether and it was
     * configured with a timeout. The timeout expires if there was
     * no activity on the tether with the consequence that the monitored
     * child will be terminated. */

    do
    {
        struct PollFdTimerAction *tetherTimer =
            &self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_TETHER];

        enum ProcessStatus childstatus = monitorProcess(self->mChildPid);

        if (ProcessStatusError == childstatus)
        {
            if (ECHILD != errno)
                terminate(
                    errno,
                    "Unable to check for status of child pid %jd",
                    (intmax_t) self->mChildPid);

            /* The child process is no longer active, so it makes
             * sense to proceed as if the child process should
             * be terminated. */
        }
        else if (ProcessStatusTrapped == childstatus ||
                 ProcessStatusStopped == childstatus)
        {
            debug(0, "deferred timeout child status %c", childstatus);

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
                lockMutex(&self->mTetherThread->mActivity.mMutex);
                since = self->mTetherThread->mActivity.mSince;
                unlockMutex(&self->mTetherThread->mActivity.mMutex);
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

        /* Once the timeout has expired, the timer can be cancelled because
         * there is no further need to run this state machine. */

        debug(0, "timeout after %ds", gOptions.mTimeout.mTether_s);

        tetherTimer->mPeriod = Duration(NanoSeconds(0));

        activateFdTimerTermination_(self, aPollTime);

    } while (0);
}

/* -------------------------------------------------------------------------- */
static void
pollFdTimerOrphan_(void                        *self_,
                   const struct EventClockTime *aPollTime)
{
    struct ChildMonitor *self = self_;

    ensure(childMonitorType_ == self->mType);

    /* Using PR_SET_PDEATHSIG is very attractive however the detailed
     * discussion at the end of this thread is important:
     *
     * https://bugzilla.kernel.org/show_bug.cgi?id=43300
     *
     * In the most general case, PR_SET_PDEATHSIG is useless because
     * it tracks the termination of the parent thread, not the parent
     * process. */

    if (1 == getppid())
    {
        debug(0, "orphaned");

        self->mPollFdTimerActions[POLL_FD_CHILD_TIMER_ORPHAN].mPeriod =
            Duration(NanoSeconds(0));

        activateFdTimerTermination_(self, aPollTime);
    }
}

/* -------------------------------------------------------------------------- */
static bool
pollFdCompletion_(void *self_)
{
    struct ChildMonitor *self = self_;

    ensure(childMonitorType_ == self->mType);

    /* Wait until the child process has terminated, and the tether thread
     * has completed. */

    return
        ! (self->mPollFds[POLL_FD_CHILD_CHILD].events |
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

static void
pollFdChild_(void                        *self_,
             const struct EventClockTime *aPollTime)
{
    struct ChildMonitor *self = self_;

    ensure(childMonitorType_ == self->mType);

    /* There is a race here between receiving the indication that the
     * child process has terminated, and the other watchdog actions
     * that might be taking place to actively monitor or terminate
     * the child process. In other words, those actions might be
     * attempting to manage a child process that is already dead,
     * or declare the child process errant when it has already exited.
     *
     * Actively test the race by occasionally delaying this activity
     * when in test mode. */

    if ( ! testSleep())
    {
        struct PollEventText pollEventText;
        debug(
            1,
            "detected child %s",
            createPollEventText(
                &pollEventText,
                self->mPollFds[POLL_FD_CHILD_CHILD].revents));

        ensure(self->mPollFds[POLL_FD_CHILD_CHILD].events);

        char buf[1];

        switch (read(self->mPollFds[POLL_FD_CHILD_CHILD].fd, buf, sizeof(buf)))
        {
        default:
            /* The child process is running again after being stopped for
             * some time. Restart the tether timeout so that the stoppage
             * is not mistaken for a failure. */

            debug(0,
                  "child pid %jd is running",
                  (intmax_t) self->mChildPid);

            restartFdTimerTether_(self, aPollTime);
            break;

        case -1:
            if (EINTR != errno)
                terminate(
                    errno,
                    "Unable to read child pipe");
            break;

        case 0:
            /* The child process has terminated, so there is no longer
             * any need to monitor for SIGCHLD. */

            debug(0,
                  "child pid %jd has terminated",
                  (intmax_t) self->mChildPid);

            self->mPollFds[POLL_FD_CHILD_CHILD].events = 0;
            self->mPollFds[POLL_FD_CHILD_CHILD].fd     =
                self->mNullPipe->mRdFile->mFd;

            /* Record when the child has terminated, but do not exit
             * the event loop until all the IO has been flushed. With the
             * child terminated, no further input can be produced so indicate
             * to the tether thread that it should start flushing data now. */

            flushTetherThread(self->mTetherThread);

            /* Once the child process has terminated, start the disconnection
             * timer that sends a periodic signal to the tether thread
             * to ensure that it will not block. */

            self->mPollFdTimerActions[
                POLL_FD_CHILD_TIMER_DISCONNECTION].mPeriod = Duration(
                    NSECS(Seconds(1)));

            break;
        }
    }
}

static void
pollFdTimerChild_(void                        *self_,
                  const struct EventClockTime *aPollTime)
{
    struct ChildMonitor *self = self_;

    ensure(childMonitorType_ == self->mType);

    debug(0, "disconnecting tether thread");

    pingTetherThread(self->mTetherThread);
}

/* -------------------------------------------------------------------------- */
void
monitorChild(struct ChildProcess *self, struct File *aUmbilicalFile)
{
    debug(0, "start monitoring child");

    /* Remember that the child process might be in its own process group,
     * or might be in the same process group as the watchdog.
     *
     * When terminating the child process, first request that the child
     * terminate by sending it SIGTERM, and if the child does not terminate,
     * resort to sending SIGKILL. */

    struct ChildSignalPlan signalPlan[] =
    {
        {  self->mPid,  SIGTERM },
        { -self->mPgid, SIGKILL },
        { 0 }
    };

    struct Pipe nullPipe;
    if (createPipe(&nullPipe, O_CLOEXEC | O_NONBLOCK))
        terminate(
            errno,
            "Unable to create null pipe");

    /* Create a thread to use a blocking copy to transfer data from a
     * local pipe to stdout. This is primarily because SPLICE_F_NONBLOCK
     * cannot guarantee that the operation is non-blocking unless both
     * source and destination file descriptors are also themselves non-blocking.
     *
     * The child thread is used to perform a potentially blocking
     * transfer between an intermediate pipe and stdout, while
     * the main monitoring thread deals exclusively with non-blocking
     * file descriptors. */

    struct TetherThread tetherThread;
    createTetherThread(&tetherThread, &nullPipe);

    /* Divide the timeout into two cycles so that if the child process is
     * stopped, the first cycle will have a chance to detect it and
     * defer the timeout. */

    const unsigned timeoutCycles = 2;

    struct ChildMonitor childmonitor =
    {
        .mType = childMonitorType_,

        .mChildPid     = self->mPid,
        .mNullPipe     = &nullPipe,
        .mTetherThread = &tetherThread,

        .mTermination =
        {
            .mSignalPlan   = signalPlan,
            .mSignalPeriod = Duration(
                NSECS(Seconds(gOptions.mTimeout.mSignal_s))),
        },

        .mUmbilical =
        {
            .mFile = aUmbilicalFile,
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
            [POLL_FD_CHILD_CHILD] =
            {
                .fd     = self->mChildPipe->mRdFile->mFd,
                .events = POLL_INPUTEVENTS,
            },

            [POLL_FD_CHILD_UMBILICAL] =
            {
                .fd     = aUmbilicalFile->mFd,
                .events = POLL_DISCONNECTEVENT,
            },

            [POLL_FD_CHILD_TETHER] =
            {
                .fd     = tetherThread.mControlPipe.mWrFile->mFd,
                .events = POLL_DISCONNECTEVENT,
            },
        },

        .mPollFdActions =
        {
            [POLL_FD_CHILD_CHILD]     = { pollFdChild_ },
            [POLL_FD_CHILD_UMBILICAL] = { pollFdUmbilical_ },
            [POLL_FD_CHILD_TETHER]    = { pollFdTether_ },
        },

        .mPollFdTimerActions =
        {
            [POLL_FD_CHILD_TIMER_TETHER] =
            {
                /* Note that a zero value for gOptions.mTimeout.mTether_s will
                 * disable the tether timeout in which case the watchdog will
                 * supervise the child, but not impose any timing requirements
                 * on activity on the tether. */

                .mAction = pollFdTimerTether_,
                .mSince  = EVENTCLOCKTIME_INIT,
                .mPeriod = Duration(NanoSeconds(
                    NSECS(Seconds(gOptions.mTether
                                  ? gOptions.mTimeout.mTether_s
                                  : 0)).ns / timeoutCycles)),
            },

            [POLL_FD_CHILD_TIMER_UMBILICAL] =
            {
                .mAction = pollFdTimerUmbilical_,
                .mSince  = EVENTCLOCKTIME_INIT,
                .mPeriod = Duration(
                    NanoSeconds(
                        NSECS(Seconds(gOptions.mTimeout.mUmbilical_s)).ns / 2)),
            },

            [POLL_FD_CHILD_TIMER_ORPHAN] =
            {
                /* If requested to be aware when the watchdog becomes an orphan,
                 * check if init(8) is the parent of this process. If this is
                 * detected, start sending signals to the child to encourage it
                 * to exit. */

                .mAction = pollFdTimerOrphan_,
                .mSince  = EVENTCLOCKTIME_INIT,
                .mPeriod = Duration(NSECS(Seconds(gOptions.mOrphaned ? 3 : 0))),
            },

            [POLL_FD_CHILD_TIMER_TERMINATION] =
            {
                .mAction = pollFdTimerTermination_,
                .mSince  = EVENTCLOCKTIME_INIT,
                .mPeriod = Duration(NanoSeconds(0)),
            },

            [POLL_FD_CHILD_TIMER_DISCONNECTION] =
            {
                .mAction = pollFdTimerChild_,
                .mSince  = EVENTCLOCKTIME_INIT,
                .mPeriod = Duration(NanoSeconds(0)),
            },
        },
    };

    if ( ! gOptions.mTether)
        disconnectPollFdTether_(&childmonitor);

    /* Make the umbilical timer expire immediately so that the umbilical
     * process is activated to monitor the watchdog. */

    struct PollFdTimerAction *umbilicalTimer =
        &childmonitor.mPollFdTimerActions[POLL_FD_CHILD_TIMER_UMBILICAL];

    lapTimeTrigger(&umbilicalTimer->mSince, umbilicalTimer->mPeriod, 0);

    watchProcessSigCont(VoidMethod(pollFdContUmbilical_, &childmonitor));

    /* It is unfortunate that O_NONBLOCK is an attribute of the underlying
     * open file, rather than of each file descriptor. Since stdin and
     * stdout are typically inherited from the parent, setting O_NONBLOCK
     * would affect all file descriptors referring to the same open file,
     so this approach cannot be employed directly. */

    for (size_t ix = 0; NUMBEROF(childmonitor.mPollFds) > ix; ++ix)
    {
        if ( ! ownFdNonBlocking(childmonitor.mPollFds[ix].fd))
            terminate(
                0,
                "Expected %s fd %d to be non-blocking",
                sPollFdNames[ix],
                childmonitor.mPollFds[ix].fd);
    }

    struct PollFd pollfd;
    if (createPollFd(
            &pollfd,

            childmonitor.mPollFds,
            childmonitor.mPollFdActions,
            sPollFdNames, POLL_FD_CHILD_KINDS,

            childmonitor.mPollFdTimerActions,
            sPollFdTimerNames, POLL_FD_CHILD_TIMER_KINDS,

            pollFdCompletion_, &childmonitor))
        terminate(
            errno,
            "Unable to initialise polling loop");

    if (runPollFdLoop(&pollfd))
        terminate(
            errno,
            "Unable to run polling loop");

    if (closePollFd(&pollfd))
        terminate(
            errno,
            "Unable to close polling loop");

    unwatchProcessSigCont();

    closeTetherThread(&tetherThread);

    if (closePipe(&nullPipe))
        terminate(
            errno,
            "Unable to close null pipe");

    debug(0, "stop monitoring child");
}

/* -------------------------------------------------------------------------- */
