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
#include "pidserver.h"

#include "type_.h"
#include "error_.h"
#include "fd_.h"
#include "socketpair_.h"
#include "pidfile_.h"
#include "test_.h"
#include "error_.h"
#include "macros_.h"

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
    [POLL_FD_MONITOR_PIDSERVER] = "pidserver",
    [POLL_FD_MONITOR_PIDCLIENT] = "pidclient",
};

static const char *pollFdTimerNames_[POLL_FD_MONITOR_TIMER_KINDS] =
{
    [POLL_FD_MONITOR_TIMER_UMBILICAL] = "umbilical",
};

/* -------------------------------------------------------------------------- */
static int
pollFdPidServer_(void                        *self_,
                 const struct EventClockTime *aPollTime)
{
    int rc = -1;

    struct UmbilicalMonitor *self = self_;

    ensure(umbilicalMonitorType_ == self->mType);

    ERROR_IF(
        acceptPidServerConnection(self->mPidServer));

    struct pollfd *pollFd = &self->mPollFds[POLL_FD_MONITOR_PIDCLIENT];

    if ( ! pollFd->events)
    {
        pollFd->fd     = self->mPidServer->mEventQueue->mFile->mFd;
        pollFd->events = POLL_INPUTEVENTS;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
pollFdPidClient_(void                        *self_,
                 const struct EventClockTime *aPollTime)
{
    int rc = -1;

    struct UmbilicalMonitor *self = self_;

    ensure(umbilicalMonitorType_ == self->mType);

    if (cleanPidServer(self->mPidServer))
    {
        struct pollfd *pollFd = &self->mPollFds[POLL_FD_MONITOR_PIDCLIENT];

        pollFd->fd     = -1;
        pollFd->events = 0;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
closeFdUmbilical_(struct UmbilicalMonitor *self)
{
    self->mPollFds[POLL_FD_MONITOR_UMBILICAL].fd     = -1;
    self->mPollFds[POLL_FD_MONITOR_UMBILICAL].events = 0;

    self->mPollFds[POLL_FD_MONITOR_PIDSERVER].fd     = -1;
    self->mPollFds[POLL_FD_MONITOR_PIDSERVER].events = 0;
}

static int
pollFdUmbilical_(void                        *self_,
                 const struct EventClockTime *aPollTime)
{
    int rc = -1;

    struct UmbilicalMonitor *self = self_;

    ensure(umbilicalMonitorType_ == self->mType);

    char buf[1];

    ssize_t rdlen;
    ABORT_IF(
        (rdlen = read(
            self->mPollFds[POLL_FD_MONITOR_UMBILICAL].fd, buf, sizeof(buf)),
         -1 == rdlen
         ? EINTR != errno && ECONNRESET != errno
         : (errno = 0, rdlen && sizeof(buf) != rdlen)),
        {
            terminate(
                errno,
                "Unable to read umbilical connection");
        });

    /* If the far end did not read the previous echo, and simply closed its
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
        if (ECONNRESET == errno)
        {
            if (self->mUmbilical.mClosed)
                debug(0, "umbilical connection closed");
            else
                warn(0, "Umbilical connection broken");

            closeFdUmbilical_(self);
        }
    }
    else
    {
        debug(1, "received umbilical connection ping %zd", rdlen);

        ensure( ! self->mUmbilical.mClosed);

        if ( ! buf[0])
        {
            debug(1, "umbilical connection close request");

            self->mUmbilical.mClosed = true;
        }
        else
        {
            debug(1, "umbilical connection echo request");

            ssize_t wrlen;
            ABORT_IF(
                (wrlen = writeFd(
                    self->mPollFds[POLL_FD_MONITOR_UMBILICAL].fd,
                    buf, rdlen),
                 -1 == wrlen
                 ? EPIPE != errno
                 : (errno = 0, wrlen != rdlen)),
                {
                    /* Receiving EPIPE means that the umbilical connection
                     * has been closed. Rely on the umbilical connection
                     * reader to reactivate and detect the closed connection. */

                    terminate(
                        errno,
                        "Unable to echo activity into umbilical connection");
                });
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

        self->mUmbilical.mCycleCount = 0;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static int
pollFdTimerUmbilical_(
    void                        *self_,
    const struct EventClockTime *aPollTime)
{
    int rc = -1;

    struct UmbilicalMonitor *self = self_;

    ensure(umbilicalMonitorType_ == self->mType);

    /* If nothing is available from the umbilical connection after the
     * timeout period expires, then assume that the watchdog itself
     * is stuck. */

    struct ProcessState parentState =
        fetchProcessState(self->mUmbilical.mParentPid);

    if (ProcessStateStopped == parentState.mState)
    {
        debug(
            0,
            "umbilical timeout deferred due to "
            "parent status %" PRIs_ProcessState,
            FMTs_ProcessState(parentState));
        self->mUmbilical.mCycleCount = 0;
    }
    else if (++self->mUmbilical.mCycleCount >= self->mUmbilical.mCycleLimit)
    {
        warn(0, "Umbilical connection timed out");

        closeFdUmbilical_(self);
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static bool
pollFdCompletion_(void *self_)
{
    struct UmbilicalMonitor *self = self_;

    ensure(umbilicalMonitorType_ == self->mType);

    /* The umbilical event loop terminates when the connection to the
     * watchdog is closed, and when there are no more outstanding
     * child process group references. */

    return
        ! self->mPollFds[POLL_FD_MONITOR_UMBILICAL].events &&
        ! self->mPollFds[POLL_FD_MONITOR_PIDSERVER].events &&
        ! self->mPollFds[POLL_FD_MONITOR_PIDCLIENT].events;
}

/* -------------------------------------------------------------------------- */
int
createUmbilicalMonitor(
    struct UmbilicalMonitor *self,
    int                      aStdinFd,
    struct Pid               aParentPid,
    struct PidServer        *aPidServer)
{
    int rc = -1;

    unsigned cycleLimit = 2;

    *self = (struct UmbilicalMonitor)
    {
        .mType = umbilicalMonitorType_,

        .mUmbilical =
        {
            .mCycleLimit = cycleLimit,
            .mParentPid  = aParentPid,
            .mClosed     = false,
        },

        .mPidServer = aPidServer,

        .mPollFds =
        {
            [POLL_FD_MONITOR_UMBILICAL] =
            {
                .fd     = aStdinFd,
                .events = POLL_INPUTEVENTS
            },

            [POLL_FD_MONITOR_PIDSERVER] =
            {
                .fd     = aPidServer ? aPidServer->mSocket->mFile->mFd : -1,
                .events = aPidServer ? POLL_INPUTEVENTS : 0,
            },

            [POLL_FD_MONITOR_PIDCLIENT] =
            {
                .fd     = -1,
                .events = 0,
            },
        },

        .mPollFdActions =
        {
            [POLL_FD_MONITOR_UMBILICAL] = { pollFdUmbilical_ },
            [POLL_FD_MONITOR_PIDSERVER] = { pollFdPidServer_ },
            [POLL_FD_MONITOR_PIDCLIENT] = { pollFdPidClient_ },
        },

        .mPollFdTimerActions =
        {
            [POLL_FD_MONITOR_TIMER_UMBILICAL] =
            {
                .mAction = pollFdTimerUmbilical_,
                .mSince  = EVENTCLOCKTIME_INIT,
                .mPeriod = Duration(
                    NanoSeconds(
                        NSECS(Seconds(gOptions.mTimeout.mUmbilical_s)).ns
                        / cycleLimit)),
            },
        },
    };

    rc = 0;

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

    ERROR_IF(
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
    ERROR_IF(
        createPollFd(
            &pollfd,
            self->mPollFds,
            self->mPollFdActions,
            pollFdNames_, POLL_FD_MONITOR_KINDS,
            self->mPollFdTimerActions,
            pollFdTimerNames_, POLL_FD_MONITOR_TIMER_KINDS,
            pollFdCompletion_, self));

    ERROR_IF(
        runPollFdLoop(&pollfd));

    closePollFd(&pollfd);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
bool
ownUmbilicalMonitorClosedOrderly(const struct UmbilicalMonitor *self)
{
    return self->mUmbilical.mClosed;
}

/* -------------------------------------------------------------------------- */
static void
runUmbilicalProcess_(struct UmbilicalProcess *self,
                     struct Pid               aWatchdogPid,
                     struct ChildProcess     *aChildProcess,
                     struct SocketPair       *aUmbilicalSocket,
                     struct PidServer        *aPidServer)
{

    debug(0,
          "umbilical process pid %" PRId_Pid " pgid %" PRId_Pgid,
          FMTd_Pid(ownProcessId()),
          FMTd_Pgid(ownProcessGroupId()));

    ABORT_IF(
        STDIN_FILENO !=
        dup2(aUmbilicalSocket->mChildFile->mFd, STDIN_FILENO),
        {
            terminate(
                errno,
                "Unable to dup %d to stdin",
                aUmbilicalSocket->mChildFile->mFd);
        });

    ABORT_IF(
        STDOUT_FILENO != dup2(
            aUmbilicalSocket->mChildFile->mFd, STDOUT_FILENO),
        {
            terminate(
                errno,
                "Unable to dup %d to stdout",
                aUmbilicalSocket->mChildFile->mFd);
        });

    closeSocketPair(aUmbilicalSocket);

    int whiteList[] =
    {
        STDIN_FILENO,
        STDOUT_FILENO,
        STDERR_FILENO,
        ownProcessLockFile()->mFd,
        aPidServer ? aPidServer->mSocket->mFile->mFd : -1,
        aPidServer ? aPidServer->mEventQueue->mFile->mFd : -1,
    };

    ABORT_IF(
        closeFdDescriptors(whiteList, NUMBEROF(whiteList)),
        {
            terminate(
                errno,
                "Unable to close extraneous file descriptors");
        });

    if (testMode(TestLevelSync))
    {
        ABORT_IF(
            raise(SIGSTOP),
            {
                terminate(
                    errno,
                    "Unable to stop process");
            });
    }

    /* The umbilical process is not the parent of the child process being
     * watched, so that there is no reliable way to send a signal to that
     * process alone because the pid might be recycled by the time the signal
     * is sent. Instead rely on the umbilical monitor being in the same
     * process group as the child process and use the process group as
     * a means of controlling the cild process. */

    struct UmbilicalMonitor monitorpoll;
    ABORT_IF(
        createUmbilicalMonitor(
            &monitorpoll, STDIN_FILENO, aWatchdogPid, aPidServer),
        {
            terminate(errno, "Unable to create umbilical monitor");
        });

    /* Synchronise with the watchdog to avoid timing races. The watchdog
     * writes to the umbilical when it is ready to start timing. */

    debug(0, "synchronising umbilical");

    ABORT_IF(
        synchroniseUmbilicalMonitor(&monitorpoll),
        {
            terminate(errno, "Unable to synchronise umbilical monitor");
        });

    debug(0, "synchronised umbilical");

    ABORT_IF(
        runUmbilicalMonitor(&monitorpoll),
        {
            terminate(errno, "Unable to run umbilical monitor");
        });

    /* The umbilical monitor returns when the connection to the watchdog
     * is either lost or no longer active. Only issue a diagnostic if
     * the shutdown was not orderly. */

    if ( ! ownUmbilicalMonitorClosedOrderly(&monitorpoll))
        warn(0,
             "Killing child pgid %" PRId_Pgid " from umbilical",
             FMTd_Pgid(aChildProcess->mPgid));

    killChildProcessGroup(aChildProcess);
}

/* -------------------------------------------------------------------------- */
int
createUmbilicalProcess(struct UmbilicalProcess *self,
                       struct ChildProcess     *aChildProcess,
                       struct SocketPair       *aUmbilicalSocket,
                       struct PidServer        *aPidServer)
{
    int rc = -1;

    self->mPid         = Pid(0);
    self->mChildAnchor = Pid(0);
    self->mSocket      = aUmbilicalSocket;

    /* Ensure that SIGHUP is blocked so that the umbilical process
     * will not terminate should it be orphaned when the parent process
     * terminates. Verifying the signal is blocked here is important to
     * avoid a termination race.
     *
     * Note that forkProcess() will reset all handled signals in
     * the child process. */

    ensure( ! pthread_kill(pthread_self(), SIGHUP));

    struct Pid watchdogPid = ownProcessId();

    struct Pid umbilicalPid;
    ERROR_IF(
        (umbilicalPid = forkProcess(ForkProcessSetProcessGroup, Pgid(0)),
         -1 == umbilicalPid.mPid));

    if (umbilicalPid.mPid)
    {
        self->mPid = umbilicalPid;
    }
    else
    {
        self->mPid = ownProcessId();

        /* The umbilical process will create an anchor in the process
         * group of the child so that the child pid will uniquely
         * identify the child while the umbilical exists. */

        ABORT_IF(
            (self->mChildAnchor = forkProcess(
                ForkProcessSetProcessGroup, aChildProcess->mPgid),
             -1 == self->mChildAnchor.mPid));

        if ( ! self->mChildAnchor.mPid)
            quitProcess(EXIT_SUCCESS);

        runUmbilicalProcess_(self,
                             watchdogPid,
                             aChildProcess,
                             aUmbilicalSocket,
                             aPidServer);

        debug(0, "exit umbilical");

        quitProcess(EXIT_SUCCESS);
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

    ssize_t wrlen;
    ERROR_IF(
        (wrlen = writeFile(self->mSocket->mParentFile, buf, sizeof(buf)),
         -1 == wrlen && EPIPE != errno));

    /* The umbilical process might no longer be running and thus
     * unable to clean up the child process group. If so, it is
     * necessary for the watchdog clean up the child process
     * group directly. */

    if (-1 != wrlen)
    {
        ERROR_IF(
            shutdownFileSocketWriter(self->mSocket->mParentFile));

        struct Duration umbilicalTimeout =
            Duration(NSECS(Seconds(gOptions.mTimeout.mUmbilical_s)));

        struct ProcessSigContTracker sigContTracker = ProcessSigContTracker();

        struct EventClockTime since = EVENTCLOCKTIME_INIT;

        while (1)
        {
            struct Duration remaining;

            int expired;
            ERROR_IF(
                (expired = deadlineTimeExpired(
                    &since, umbilicalTimeout, &remaining, 0),
                 expired && ! checkProcessSigContTracker(&sigContTracker)),
                {
                    errno = ETIMEDOUT;
                });

            if (expired)
            {
                since = (struct EventClockTime) EVENTCLOCKTIME_INIT;
                continue;
            }

            /* The umbilical process might have been in the midst of
             * responding to a ping, so take the trouble to drain the
             * connection to get a clean shutdown. */

            int ready = 0;
            ERROR_IF(
                (ready = waitFileReadReady(
                    self->mSocket->mParentFile, &remaining),
                 -1 == ready));

            if (ready)
            {
                ssize_t rdlen = 0;
                ERROR_IF(
                    (rdlen = readFile(
                        self->mSocket->mParentFile, buf, 1),
                     -1 == rdlen && ECONNRESET != errno));

                if (rdlen)
                    continue;
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
