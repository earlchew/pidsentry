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
#include "childprocess.h"
#include "pidserver.h"

#include "options_.h"

#include "ert/socketpair.h"
#include "ert/process.h"
#include "ert/fdset.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

/* -------------------------------------------------------------------------- */
/* Umbilical Process
 *
 * The purpose of the umbilical process is to sense if the sentry itself
 * is performing properly. The umbilical will break if either the sentry
 * process terminates, or if the umbilical process terminates. Additionally
 * the umbilical process monitors the umbilical for periodic messages
 * sent by the sentry, and echoes the messages back to the sentry.
 */

static const char *pollFdNames_[POLL_FD_MONITOR_KINDS] =
{
    [POLL_FD_MONITOR_UMBILICAL] = "umbilical",
    [POLL_FD_MONITOR_PIDSERVER] = "pidserver",
    [POLL_FD_MONITOR_PIDCLIENT] = "pidclient",
    [POLL_FD_MONITOR_EVENTPIPE] = "event pipe",
};

static const char *pollFdTimerNames_[POLL_FD_MONITOR_TIMER_KINDS] =
{
    [POLL_FD_MONITOR_TIMER_UMBILICAL] = "umbilical",
};

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
pollFdPidServer_(struct UmbilicalMonitor         *self,
                 const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    ERROR_IF(
        acceptPidServerConnection(self->mPidServer));

    struct pollfd *pollFd = &self->mPoll.mFds[POLL_FD_MONITOR_PIDCLIENT];

    if ( ! pollFd->events)
    {
        pollFd->fd     = self->mPidServer->mEventQueue->mFile->mFd;
        pollFd->events = ERT_POLL_INPUTEVENTS;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
pollFdPidClient_(struct UmbilicalMonitor         *self,
                 const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    if (cleanPidServer(self->mPidServer))
    {
        struct pollfd *pollFd = &self->mPoll.mFds[POLL_FD_MONITOR_PIDCLIENT];

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
    ABORT_IF(
        shutdown(
            self->mPoll.mFds[POLL_FD_MONITOR_UMBILICAL].fd,
            SHUT_WR));

    self->mPoll.mFds[POLL_FD_MONITOR_UMBILICAL].fd     = -1;
    self->mPoll.mFds[POLL_FD_MONITOR_UMBILICAL].events = 0;

    self->mPoll.mFds[POLL_FD_MONITOR_PIDSERVER].fd     = -1;
    self->mPoll.mFds[POLL_FD_MONITOR_PIDSERVER].events = 0;

    /* Since the umbilical connection is no longer being monitored, there
     * is no reason to run its associated timer. */

    self->mPoll.mFdTimerActions[POLL_FD_MONITOR_TIMER_UMBILICAL].mPeriod =
        Ert_ZeroDuration;
}

static ERT_CHECKED int
pollFdUmbilical_(struct UmbilicalMonitor         *self,
                 const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    char buf[1];

    ssize_t rdlen;
    ERROR_IF(
        (rdlen = read(
            self->mPoll.mFds[POLL_FD_MONITOR_UMBILICAL].fd, buf, sizeof(buf)),
         -1 == rdlen
         ? EINTR != errno && ECONNRESET != errno
         : (errno = 0, rdlen && sizeof(buf) != rdlen)));

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

            /* Requests for echoes are posted so that they can
             * be retried on EINTR. */

            ERROR_IF(
                Ert_EventLatchSettingError == ert_setEventLatch(
                    self->mLatch.mEchoRequest));
        }

        /* Once activity is detected on the umbilical, reset the
         * umbilical timer, but configure the timer so that it is
         * out-of-phase with the expected activity on the umbilical
         * to avoid having to deal with races when there is a tight
         * finish. */

        struct Ert_PollFdTimerAction *umbilicalTimer =
            &self->mPoll.mFdTimerActions[POLL_FD_MONITOR_TIMER_UMBILICAL];

        ert_lapTimeTrigger(&umbilicalTimer->mSince,
                           umbilicalTimer->mPeriod, aPollTime);

        ert_lapTimeDelay(
            &umbilicalTimer->mSince,
            Ert_Duration(
                Ert_NanoSeconds(umbilicalTimer->mPeriod.duration.ns / 2)));

        self->mUmbilical.mCycleCount = 0;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static ERT_CHECKED int
pollFdTimerUmbilical_(
    struct UmbilicalMonitor         *self,
    const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    /* If nothing is available from the umbilical connection after the
     * timeout period expires, then assume that the sentry itself
     * is stuck. */

    struct Ert_ProcessState parentState =
        ert_fetchProcessState(self->mUmbilical.mParentPid);

    if (Ert_ProcessStateStopped == parentState.mState)
    {
        debug(
            0,
            "umbilical timeout deferred due to "
            "parent status %" PRIs_Ert_ProcessState,
            FMTs_Ert_ProcessState(parentState));
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
static int
pollFdSendEcho_(struct UmbilicalMonitor         *self,
                bool                             aEnabled,
                const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    ensure(aEnabled);

    /* The umbilical connection might have been closed by the time
     * this code runs. */

    if (-1 == self->mPoll.mFds[POLL_FD_MONITOR_UMBILICAL].fd)
        debug(0, "skipping umbilical echo");
    else
    {
        /* Receiving EPIPE means that the umbilical connection
         * has been closed. Rely on the umbilical connection
         * reader to reactivate and detect the closed connection. */

        char buf[1] = { '.' };

        ssize_t wrlen;
        ERROR_IF(
            (wrlen = ert_writeFd(
                self->mPoll.mFds[POLL_FD_MONITOR_UMBILICAL].fd,
                buf, sizeof(buf), 0),
             -1 == wrlen
             ? EPIPE != errno
             : (errno = 0, sizeof(buf) != wrlen)));

        debug(0, "sent umbilical echo");
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
pollFdEventPipe_(struct UmbilicalMonitor         *self,
                 const struct Ert_EventClockTime *aPollTime)
{
    int rc = -1;

    /* Actively test races by occasionally delaying this activity
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

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static bool
pollFdCompletion_(struct UmbilicalMonitor *self)
{
    /* The umbilical event loop terminates when the connection to the
     * sentry is closed, and when there are no more outstanding
     * child process group references. */

    return
        ! self->mPoll.mFds[POLL_FD_MONITOR_UMBILICAL].events &&
        ! self->mPoll.mFds[POLL_FD_MONITOR_PIDSERVER].events &&
        ! self->mPoll.mFds[POLL_FD_MONITOR_PIDCLIENT].events;
}

/* -------------------------------------------------------------------------- */
int
createUmbilicalMonitor(
    struct UmbilicalMonitor *self,
    int                      aStdinFd,
    struct Ert_Pid           aParentPid,
    struct PidServer        *aPidServer)
{
    int rc = -1;

    unsigned cycleLimit = 2;

    self->mPidServer = aPidServer;
    self->mEventPipe = 0;

    ERROR_IF(
        ert_createEventLatch(&self->mLatch.mEchoRequest_, "echo request"));
    self->mLatch.mEchoRequest = &self->mLatch.mEchoRequest_;

    ERROR_IF(
        ert_createEventPipe(&self->mEventPipe_, O_CLOEXEC | O_NONBLOCK));
    self->mEventPipe = &self->mEventPipe_;

    ERROR_IF(
        Ert_EventLatchSettingError == ert_bindEventLatchPipe(
            self->mLatch.mEchoRequest, self->mEventPipe,
            Ert_EventLatchMethod(
                self, pollFdSendEcho_)));

    self->mUmbilical = (ERT_DECLTYPE(self->mUmbilical))
    {
        .mCycleLimit = cycleLimit,
        .mParentPid  = aParentPid,
        .mClosed     = false,
    };

    self->mPoll = (ERT_DECLTYPE(self->mPoll))
    {
        .mFds =
        {
            [POLL_FD_MONITOR_UMBILICAL] =
            {
                .fd     = aStdinFd,
                .events = ERT_POLL_INPUTEVENTS
            },

            [POLL_FD_MONITOR_PIDSERVER] =
            {
                .fd     = aPidServer
                          ? aPidServer->mUnixSocket->mSocket->mFile->mFd : -1,
                .events = aPidServer
                          ? ERT_POLL_INPUTEVENTS : 0,
            },

            [POLL_FD_MONITOR_PIDCLIENT] =
            {
                .fd     = -1,
                .events = 0,
            },

            [POLL_FD_MONITOR_EVENTPIPE] =
            {
                .fd     = self->mEventPipe->mPipe->mRdFile->mFd,
                .events = ERT_POLL_INPUTEVENTS,
            },
        },

        .mFdActions =
        {
            [POLL_FD_MONITOR_UMBILICAL] = {
                Ert_PollFdCallbackMethod(self, pollFdUmbilical_) },
            [POLL_FD_MONITOR_PIDSERVER] = {
                Ert_PollFdCallbackMethod(self, pollFdPidServer_) },
            [POLL_FD_MONITOR_PIDCLIENT] = {
                Ert_PollFdCallbackMethod(self, pollFdPidClient_) },
            [POLL_FD_MONITOR_EVENTPIPE] = {
                Ert_PollFdCallbackMethod(self, pollFdEventPipe_) },
        },

        .mFdTimerActions =
        {
            [POLL_FD_MONITOR_TIMER_UMBILICAL] =
            {
                .mAction = Ert_PollFdCallbackMethod(
                    self, pollFdTimerUmbilical_),
                .mSince  = ERT_EVENTCLOCKTIME_INIT,
                .mPeriod = Ert_Duration(
                    Ert_NanoSeconds(
                      ERT_NSECS(
                        Ert_Seconds(gOptions.mServer.mTimeout.mUmbilical_s)).ns
                      / cycleLimit)),
            },
        },
    };

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            self = closeUmbilicalMonitor(self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct UmbilicalMonitor *
closeUmbilicalMonitor(struct UmbilicalMonitor *self)
{
    if (self)
    {
        ABORT_IF(
            Ert_EventLatchSettingError == ert_unbindEventLatchPipe(
                self->mLatch.mEchoRequest));

        self->mEventPipe          = ert_closeEventPipe(self->mEventPipe);
        self->mLatch.mEchoRequest = ert_closeEventLatch(
            self->mLatch.mEchoRequest);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
int
synchroniseUmbilicalMonitor(struct UmbilicalMonitor *self)
{
    int rc = -1;

    /* Use a blocking read to wait for the sentry to signal that the
     * umbilical monitor should proceed. */

    ERROR_IF(
        -1 == ert_waitFdReadReady(
            self->mPoll.mFds[POLL_FD_MONITOR_UMBILICAL].fd, 0));

    ERROR_IF(
        pollFdUmbilical_(self, 0));

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

    struct Ert_PollFd  pollfd_;
    struct Ert_PollFd *pollfd = 0;

    ERROR_IF(
        ert_createPollFd(
            &pollfd_,
            self->mPoll.mFds,
            self->mPoll.mFdActions,
            pollFdNames_, POLL_FD_MONITOR_KINDS,
            self->mPoll.mFdTimerActions,
            pollFdTimerNames_, POLL_FD_MONITOR_TIMER_KINDS,
            Ert_PollFdCompletionMethod(self, pollFdCompletion_)));
    pollfd = &pollfd_;

    ERROR_IF(
        ert_runPollFdLoop(pollfd));

    rc = 0;

Finally:

    FINALLY
    ({
        pollfd = ert_closePollFd(pollfd);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
bool
ownUmbilicalMonitorClosedOrderly(const struct UmbilicalMonitor *self)
{
    return self->mUmbilical.mClosed;
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
prepareUmbilicalProcess_(struct UmbilicalProcess         *self,
                         const struct Ert_PreForkProcess *aPreFork)
{
    int rc = -1;

    ERROR_IF(
        ert_insertFdSetFile(
            aPreFork->mWhitelistFds,
            self->mSocket->mParentSocket->mSocket->mFile));

    ERROR_IF(
        ert_insertFdSetFile(
            aPreFork->mWhitelistFds,
            self->mSocket->mChildSocket->mSocket->mFile));

    if (self->mPidServer)
    {
        ERROR_IF(
            ert_insertFdSetFile(
                aPreFork->mWhitelistFds,
                self->mPidServer->mUnixSocket->mSocket->mFile));

        ERROR_IF(
            ert_insertFdSetFile(
                aPreFork->mWhitelistFds,
                self->mPidServer->mEventQueue->mFile));
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
postUmbilicalProcessChild_(struct UmbilicalProcess *self)
{
    int rc = -1;

    self->mPid = ert_ownProcessId();

    /* The umbilical process will create an anchor in the process
     * group of the child and the process group of the sentry so that
     * the pids will uniquely identify those process groups while
     * the umbilical exists. */

    ERROR_IF(
        (self->mChildAnchor = ert_forkProcessChild(
            Ert_ForkProcessSetProcessGroup,
            self->mChildProcess->mPgid,
            Ert_PreForkProcessMethodNil(),
            Ert_PostForkChildProcessMethodNil(),
            Ert_PostForkParentProcessMethodNil(),
            Ert_ForkProcessMethod(
                "",
                ERT_LAMBDA(
                    int, (char *this),
                    {
                        return EXIT_SUCCESS;
                    }))),
         -1 == self->mChildAnchor.mPid));

    ERROR_IF(
        (self->mSentryAnchor = ert_forkProcessChild(
            Ert_ForkProcessSetProcessGroup,
            self->mSentryPgid,
            Ert_PreForkProcessMethodNil(),
            Ert_PostForkChildProcessMethodNil(),
            Ert_PostForkParentProcessMethodNil(),
            Ert_ForkProcessMethod(
                "",
                ERT_LAMBDA(
                    int, (char *this),
                    {
                        return EXIT_SUCCESS;
                    }))),
         -1 == self->mSentryAnchor.mPid));

    debug(
        0,
        "umbilical process pid %" PRId_Ert_Pid " pgid %" PRId_Ert_Pgid,
        FMTd_Ert_Pid(ert_ownProcessId()),
        FMTd_Ert_Pgid(ert_ownProcessGroupId()));

    /* Indicate to the sentry that the umbilical monitor has
     * started successfully and bound itself to process group
     * of the sentry and child. */

    ert_closeSocketPairParent(self->mSocket);

    ERROR_IF(
        STDIN_FILENO !=
        dup2(self->mSocket->mChildSocket->mSocket->mFile->mFd, STDIN_FILENO));

    ERROR_IF(
        STDOUT_FILENO != dup2(
            self->mSocket->mChildSocket->mSocket->mFile->mFd, STDOUT_FILENO));

    self->mSocket = ert_closeSocketPair(self->mSocket);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
postUmbilicalProcessParent_(struct UmbilicalProcess *self,
                            struct Ert_Pid           aChildPid)
{
    int rc = -1;

    ert_closeSocketPairChild(self->mSocket);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
runUmbilicalProcessChild_(struct UmbilicalProcess *self)
{
    int rc = -1;

    struct UmbilicalMonitor  umbilicalMonitor_;
    struct UmbilicalMonitor *umbilicalMonitor = 0;

    if (ert_testMode(Ert_TestLevelSync))
    {
        ERROR_IF(
            raise(SIGSTOP));
    }

    /* The umbilical process is not the parent of the child process being
     * watched, so that there is no reliable way to send a signal to that
     * process alone because the pid might be recycled by the time the signal
     * is sent. Instead rely on the umbilical monitor being in the same
     * process group as the child process and use the process group as
     * a means of controlling the cild process. */

    ERROR_IF(
        createUmbilicalMonitor(
            &umbilicalMonitor_,
            STDIN_FILENO, self->mSentryPid, self->mPidServer));
    umbilicalMonitor = &umbilicalMonitor_;

    /* Synchronise with the sentry to avoid timing races. The sentry
     * writes to the umbilical when it is ready to start timing. */

    debug(0, "synchronising umbilical");

    ERROR_IF(
        synchroniseUmbilicalMonitor(umbilicalMonitor));

    debug(0, "synchronised umbilical");

    ERROR_IF(
        runUmbilicalMonitor(umbilicalMonitor));

    /* The umbilical monitor returns when the connection to the sentry
     * is either lost or no longer active. Only issue a diagnostic if
     * the shutdown was not orderly. */

    if ( ! ownUmbilicalMonitorClosedOrderly(umbilicalMonitor))
        warn(
            0,
            "Killing child pgid %" PRId_Ert_Pgid " from umbilical",
            FMTd_Ert_Pgid(self->mChildProcess->mPgid));

    ERROR_IF(
        killChildProcessGroup(self->mChildProcess));

    /* If the shutdown was not orderly, assume the worst and attempt to
     * clean up the sentry process group. */

    if ( ! ownUmbilicalMonitorClosedOrderly(umbilicalMonitor))
        ERROR_IF(
            ert_signalProcessGroup(self->mSentryPgid, SIGKILL));

    debug(0, "exit umbilical");

    rc = EXIT_SUCCESS;

Finally:

    FINALLY
    ({
        umbilicalMonitor = closeUmbilicalMonitor(umbilicalMonitor);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
createUmbilicalProcess(struct UmbilicalProcess *self,
                       struct ChildProcess     *aChildProcess,
                       struct Ert_SocketPair   *aUmbilicalSocket,
                       struct PidServer        *aPidServer)
{
    int rc = -1;

    struct Ert_ThreadSigMask  sigMask_;
    struct Ert_ThreadSigMask *sigMask = 0;

    self->mPid          = Ert_Pid(0);
    self->mChildAnchor  = Ert_Pid(0);
    self->mSentryAnchor = Ert_Pid(0);
    self->mSentryPid    = ert_ownProcessId();
    self->mSentryPgid   = ert_ownProcessGroupId();
    self->mChildProcess = aChildProcess;
    self->mSocket       = aUmbilicalSocket;
    self->mPidServer    = aPidServer;

    /* Ensure that SIGHUP is blocked so that the umbilical process
     * will not terminate should it be orphaned when the parent process
     * terminates. Doing this first in the parent is important to avoid
     * a termination race.
     *
     * Note that forkProcess() will reset all handled signals in
     * the child process. */

    sigMask = ert_pushThreadSigMask(
        &sigMask_, Ert_ThreadSigMaskBlock, (const int []) { SIGHUP, 0 });

    struct Ert_Pid umbilicalPid;
    ERROR_IF(
        (umbilicalPid = ert_forkProcessChild(
            Ert_ForkProcessSetProcessGroup,
            Ert_Pgid(0),
            Ert_PreForkProcessMethod(
                self, prepareUmbilicalProcess_),
            Ert_PostForkChildProcessMethod(
                self, postUmbilicalProcessChild_),
            Ert_PostForkParentProcessMethod(
                self, postUmbilicalProcessParent_),
            Ert_ForkProcessMethod(
                self, runUmbilicalProcessChild_)),
         -1 == umbilicalPid.mPid));
    self->mPid = umbilicalPid;

    rc = 0;

Finally:

    FINALLY
    ({
        sigMask = ert_popThreadSigMask(sigMask);

        if (rc)
        {
            if (self->mPid.mPid)
            {
                ABORT_IF(
                    ert_signalProcessGroup(Ert_Pgid(self->mPid.mPid), SIGKILL));

                int status;
                ABORT_IF(
                    ert_reapProcessChild(self->mPid, &status));
            }
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
stopUmbilicalProcess(struct UmbilicalProcess *self)
{
    int rc = -1;

    /* Try to shut down the umbilical process, but take care that it
     * might already have terminated. */

    char buf[1] = { 0 };

    ssize_t wrlen;
    ERROR_IF(
        (wrlen = ert_sendUnixSocket(self->mSocket->mParentSocket,
                                    buf, sizeof(buf)),
         -1 == wrlen && EPIPE != errno));

    if (-1 != wrlen)
    {
        /* If the umbilical process has not yet already shut down, be
         * prepared to wait a short time to obtain an orderly shut down,
         * but do not stall here indefinitely. */

        ERROR_IF(
            ert_shutdownUnixSocketWriter(self->mSocket->mParentSocket));

        struct Ert_Duration umbilicalTimeout =
            Ert_Duration(
                ERT_NSECS(Ert_Seconds(gOptions.mServer.mTimeout.mUmbilical_s)));

        struct Ert_ProcessSigContTracker sigContTracker =
            Ert_ProcessSigContTracker();

        struct Ert_EventClockTime since = ERT_EVENTCLOCKTIME_INIT;

        enum
        {
            Running,
            Stopping,
            Stopped
        } umbilicalState = Running;

        while (Stopped != umbilicalState)
        {
            struct Ert_Duration remaining;

            int expired;
            ERROR_IF(
                (expired = ert_deadlineTimeExpired(
                    &since, umbilicalTimeout, &remaining, 0),
                 expired && ! ert_checkProcessSigContTracker(&sigContTracker)),
                {
                    errno = ETIMEDOUT;
                });

            if (expired)
            {
                since = (struct Ert_EventClockTime) ERT_EVENTCLOCKTIME_INIT;
                continue;
            }

            if (Running == umbilicalState)
            {
                /* The umbilical process might have been in the midst of
                 * responding to a ping, so take the trouble to drain the
                 * connection to get a clean shutdown. */

                int ready = 0;
                ERROR_IF(
                    (ready = ert_waitUnixSocketReadReady(
                        self->mSocket->mParentSocket, &remaining),
                     -1 == ready));

                if (ready)
                {
                    ssize_t rdlen = 0;
                    ERROR_IF(
                        (rdlen = ert_recvUnixSocket(
                            self->mSocket->mParentSocket, buf, 1),
                         -1 == rdlen && ECONNRESET != errno));

                    if (rdlen)
                        continue;
                }

                umbilicalState = Stopping;
            }
            else if (Stopping == umbilicalState)
            {
                struct Ert_ChildProcessState processState;

                ERROR_IF(
                    (processState = ert_waitProcessChild(self->mPid),
                     Ert_ChildProcessStateError == processState.mChildState));

                switch (processState.mChildState)
                {
                default:
                    break;

                case Ert_ChildProcessStateExited:
                case Ert_ChildProcessStateKilled:
                case Ert_ChildProcessStateDumped:
                    umbilicalState = Stopped;
                    break;
                }
            }
        }
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
