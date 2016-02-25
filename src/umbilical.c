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

#include "umbilical.h"
#include "child.h"

#include "macros_.h"
#include "type_.h"
#include "error_.h"
#include "fd_.h"
#include "socketpair_.h"
#include "pidfile_.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

/* -------------------------------------------------------------------------- */
/* Umbilical Process
 *
 * The purpose of the umbilical process is to sense if the watchdog itself
 * is performing properly. The umbilical will break if either the watchdog
 * process terminates, or if the umbilical process terminates. Additionally
 * the umbilical process monitors the umbilical for periodic messages
 * sent by the watchdog, and echoes the messages back to the watchdog.
 */

static const struct Type * const umbilicalMonitorType_ =
    TYPE("UmbilicalMonitor");

static const char *pollFdNames_[POLL_FD_MONITOR_KINDS] =
{
    [POLL_FD_MONITOR_UMBILICAL] = "umbilical",
};

static const char *pollFdTimerNames_[POLL_FD_MONITOR_TIMER_KINDS] =
{
    [POLL_FD_MONITOR_TIMER_UMBILICAL] = "umbilical",
};

/* -------------------------------------------------------------------------- */
static void
pollFdUmbilical_(void                        *self_,
                 const struct EventClockTime *aPollTime)
{
    struct UmbilicalMonitor *self = self_;

    ensure(umbilicalMonitorType_ == self->mType);

    char buf[1];

    ssize_t rdlen = read(
        self->mPollFds[POLL_FD_MONITOR_UMBILICAL].fd, buf, sizeof(buf));

    /* If the far end did not the previous echo, and simply closed its
     * end of the connection (likely because it detected the child
     * process terminated), then the read will return ECONNRESET. This
     * is equivalent to encountering the end of file. */

    if ( ! rdlen)
    {
        errno = ECONNRESET;
        rdlen = -1;
    }

    if (-1 == rdlen)
    {
        switch (errno)
        {
        default:
            terminate(
                errno,
                "Unable to read umbilical connection");

        case EINTR:
            break;

        case ECONNRESET:
            if (self->mClosed)
                debug(0, "umbilical connection closed");
            else
                warn(0, "Umbilical connection broken");

            self->mPollFds[POLL_FD_MONITOR_UMBILICAL].events = 0;
            break;
        }
    }
    else
    {
        debug(1, "received umbilical connection ping %zd", rdlen);
        ensure(sizeof(buf) == rdlen);

        ensure( ! self->mClosed);

        if ( ! buf[0])
        {
            debug(1, "umbilical connection close request");

            self->mClosed = true;
        }
        else
        {
            debug(1, "umbilical connection echo request");

            if (writeFd(
                    self->mPollFds[POLL_FD_MONITOR_UMBILICAL].fd,
                    buf, rdlen) != rdlen)
            {
                /* Receiving EPIPE means that the umbilical connection
                 * has been closed. Rely on the umbilical connection
                 * reader to reactivate and detect the closed connection. */

                if (EPIPE != errno)
                    terminate(
                        errno,
                        "Unable to echo activity into umbilical connection");
            }
        }

        /* Once activity is detected on the umbilical, reset the
         * umbilical timer, but configure the timer so that it is
         * out-of-phase with the expected activity on the umbilical
         * to avoid having to deal with races when there is a tight
         * finish. */

        struct PollFdTimerAction *umbilicalTimer =
            &self->mPollFdTimerActions[POLL_FD_MONITOR_TIMER_UMBILICAL];

        lapTimeTrigger(&umbilicalTimer->mSince,
                       umbilicalTimer->mPeriod, aPollTime);

        lapTimeDelay(
            &umbilicalTimer->mSince,
            Duration(NanoSeconds(umbilicalTimer->mPeriod.duration.ns / 2)));

        self->mCycleCount = 0;
    }
}

static void
pollFdTimerUmbilical_(
    void                        *self_,
    const struct EventClockTime *aPollTime)
{
    struct UmbilicalMonitor *self = self_;

    ensure(umbilicalMonitorType_ == self->mType);

    /* If nothing is available from the umbilical connection after the
     * timeout period expires, then assume that the watchdog itself
     * is stuck. */

    enum ProcessStatus parentstatus = fetchProcessState(self->mParentPid);

    if (ProcessStateStopped == parentstatus)
    {
        debug(
            0,
            "umbilical timeout deferred due to parent status %c",
            parentstatus);
        self->mCycleCount = 0;
    }
    else if (++self->mCycleCount >= self->mCycleLimit)
    {
        warn(0, "Umbilical connection timed out");
        self->mPollFds[POLL_FD_MONITOR_UMBILICAL].events = 0;
    }
}

static bool
pollFdCompletion_(void *self_)
{
    struct UmbilicalMonitor *self = self_;

    ensure(umbilicalMonitorType_ == self->mType);

    return ! self->mPollFds[POLL_FD_MONITOR_UMBILICAL].events;
}

/* -------------------------------------------------------------------------- */
int
createUmbilicalMonitor(
    struct UmbilicalMonitor *self,
    int                      aStdinFd,
    pid_t                    aParentPid)
{
    int rc = -1;

    unsigned cycleLimit = 2;

    *self = (struct UmbilicalMonitor)
    {
        .mType       = umbilicalMonitorType_,
        .mCycleLimit = cycleLimit,
        .mParentPid  = aParentPid,
        .mClosed     = false,

        .mPollFds =
        {
            [POLL_FD_MONITOR_UMBILICAL] = { .fd     = aStdinFd,
                                            .events = POLL_INPUTEVENTS },
        },

        .mPollFdActions =
        {
            [POLL_FD_MONITOR_UMBILICAL] = { pollFdUmbilical_ },
        },

        .mPollFdTimerActions =
        {
            [POLL_FD_MONITOR_TIMER_UMBILICAL] =
            {
                pollFdTimerUmbilical_,
                Duration(
                    NanoSeconds(
                        NSECS(Seconds(gOptions.mTimeout.mUmbilical_s)).ns
                        / cycleLimit)),
            },
        },
    };

    rc = 0;

    goto Finally;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
synchroniseUmbilicalMonitor(struct UmbilicalMonitor *self)
{
    int rc = -1;

    /* Use a blocking read to wait for the watchdog to signal that the
     * umbilical monitor should proceed. */

    FINALLY_IF(
        -1 == waitFdReadReady(STDIN_FILENO, 0));

    pollFdUmbilical_(self, 0);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
runUmbilicalMonitor(struct UmbilicalMonitor *self)
{
    int rc = -1;

    struct PollFd pollfd;
    FINALLY_IF(
        createPollFd(
            &pollfd,
            self->mPollFds,
            self->mPollFdActions,
            pollFdNames_, POLL_FD_MONITOR_KINDS,
            self->mPollFdTimerActions,
            pollFdTimerNames_, POLL_FD_MONITOR_TIMER_KINDS,
            pollFdCompletion_, self));

    FINALLY_IF(
        runPollFdLoop(&pollfd));

    FINALLY_IF(
        closePollFd(&pollfd));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
bool
ownUmbilicalMonitorClosedOrderly(const struct UmbilicalMonitor *self)
{
    return self->mClosed;
}

/* -------------------------------------------------------------------------- */
static void
runUmbilicalProcess_(struct UmbilicalProcess *self,
                     pid_t                    aWatchdogPid,
                     struct ChildProcess     *aChildProcess,
                     struct SocketPair       *aUmbilicalSocket,
                     struct SocketPair       *aSyncSocket,
                     struct PidFile          *aPidFile)
{
    struct PidFile *pidFile = aPidFile;

    debug(0, "umbilical process pid %jd pgid %jd",
          (intmax_t) getpid(),
          (intmax_t) getpgid(0));

    ensure(aChildProcess->mPgid == getpgid(0));

    if (STDIN_FILENO !=
        dup2(aUmbilicalSocket->mChildFile->mFd, STDIN_FILENO))
        terminate(
            errno,
            "Unable to dup %d to stdin",
            aUmbilicalSocket->mChildFile->mFd);

    if (STDOUT_FILENO != dup2(
            aUmbilicalSocket->mChildFile->mFd, STDOUT_FILENO))
        terminate(
            errno,
            "Unable to dup %d to stdout",
            aUmbilicalSocket->mChildFile->mFd);

    if (pidFile)
    {
        if (acquireReadLockPidFile(pidFile))
            terminate(
                errno,
                "Unable to acquire read lock on pid file '%s'",
                pidFile->mPathName.mFileName);

        if (closePidFile(pidFile))
            terminate(
                errno,
                "Cannot close pid file '%s'",
                pidFile->mPathName.mFileName);
        pidFile = 0;
    }

    if (closeSocketPair(aSyncSocket))
        terminate(
            errno,
            "Unable to close sync socket");

    if (closeSocketPair(aUmbilicalSocket))
        terminate(
            errno,
            "Unable to close umbilical socket");

    monitorChildUmbilical(aChildProcess, aWatchdogPid);

    pid_t pgid = getpgid(0);

    warn(0, "Umbilical failed to clean process group %jd", (intmax_t) pgid);

    _exit(EXIT_FAILURE);
}

/* -------------------------------------------------------------------------- */
int
createUmbilicalProcess(struct UmbilicalProcess *self,
                       struct ChildProcess     *aChildProcess,
                       struct SocketPair       *aUmbilicalSocket,
                       struct SocketPair       *aSyncSocket,
                       struct PidFile          *aPidFile)
{
    int rc = -1;

    self->mPid    = 0;
    self->mPgid   = aChildProcess->mPgid;
    self->mSocket = aUmbilicalSocket;

    /* Ensure that SIGHUP is blocked so that the umbilical process
     * will not terminate should it be orphaned when the parent process
     * terminates. Verifying the signal is blocked here is important to
     * avoid a termination race.
     *
     * Note that forkProcess() will reset all handled signals in
     * the child process. */

    ensure( ! pthread_kill(pthread_self(), SIGHUP));

    pid_t watchdogPid  = getpid();

    pid_t umbilicalPid;
    FINALLY_IF(
        -1 == (umbilicalPid = forkProcess(ForkProcessSetProcessGroup,
                                          self->mPgid)));

    if (umbilicalPid)
    {
        self->mPid = umbilicalPid;
    }
    else
    {
        self->mPid = getpid();

        runUmbilicalProcess_(self,
                             watchdogPid,
                             aChildProcess,
                             aUmbilicalSocket,
                             aSyncSocket,
                             aPidFile);
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
stopUmbilicalProcess(struct UmbilicalProcess *self)
{
    int rc = -1;

    char buf[1] = { 0 };

    ssize_t wrlen = writeFile(self->mSocket->mParentFile, buf, sizeof(buf));

    if (-1 == wrlen)
    {
        /* The umbilical process might no longer running and thus
         * unable to clean up the child process group. If so, it is
         * necessary for the watchdog clean up the child process
         * group directly. */

        if (EPIPE != errno)
            goto Finally;
    }
    else
    {
        if (shutdownFileSocketWriter(self->mSocket->mParentFile))
            goto Finally;

        struct Duration umbilicalTimeout =
            Duration(NSECS(Seconds(gOptions.mTimeout.mUmbilical_s)));

        struct ProcessSigContTracker sigContTracker = ProcessSigContTracker();

        struct EventClockTime since = EVENTCLOCKTIME_INIT;

        while (1)
        {
            struct Duration remaining;

            if (deadlineTimeExpired(&since, umbilicalTimeout, &remaining, 0))
            {
                if (checkProcessSigContTracker(&sigContTracker))
                {
                    since = (struct EventClockTime) EVENTCLOCKTIME_INIT;
                    continue;
                }

                break;
            }

            switch (waitFileReadReady(self->mSocket->mParentFile, &remaining))
            {
            default:
                break;

            case -1:
                goto Finally;

            case 0:
                errno = ETIMEDOUT;
                goto Finally;
            }

            /* Although the connection to the umbilical process is closed,
             * there is no guarantee that waitpid() will not block. */

            switch (monitorProcess(self->mPid))
            {
            default:
                monotonicSleep(Duration(NSECS(Seconds(1))));
                continue;

            case ProcessStatusExited:
            case ProcessStatusKilled:
            case ProcessStatusDumped:
                break;
            }

            int status;
            if (reapProcess(self->mPid, &status))
                goto Finally;

            if (WIFEXITED(status))
            {
                errno = ECHILD;
                warn(0,
                     "Umbilical exited with status %d",
                     WEXITSTATUS(status));
                goto Finally;
            }
            else
            {
                if ( ! WIFSIGNALED(status))
                    terminate(
                        0,
                        "Umbilical pid %jd terminated for unknown reason",
                        (intmax_t) self->mPid);

                int termSig = WTERMSIG(status);

                struct ProcessSignalName sigName;

                if (SIGKILL != termSig)
                {
                    errno = ECHILD;
                    warn(0,
                         "Umbilical killed by signal %s",
                         formatProcessSignalName(&sigName, termSig));
                }
            }

            break;
        }
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
