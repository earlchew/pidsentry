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

#include "tether.h"

#include "fd_.h"
#include "pollfd_.h"
#include "error_.h"
#include "thread_.h"
#include "process_.h"

#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>

#include <sys/ioctl.h>

/* -------------------------------------------------------------------------- */
enum PollFdTetherKind
{
    POLL_FD_TETHER_CONTROL,
    POLL_FD_TETHER_INPUT,
    POLL_FD_TETHER_OUTPUT,
    POLL_FD_TETHER_KINDS
};

enum PollFdTetherTimerKind
{
    POLL_FD_TETHER_TIMER_DISCONNECT,
    POLL_FD_TETHER_TIMER_KINDS
};

static const char *pollFdNames_[] =
{
    [POLL_FD_TETHER_CONTROL] = "control",
    [POLL_FD_TETHER_INPUT]   = "input",
    [POLL_FD_TETHER_OUTPUT]  = "output",
};

static const char *pollFdTimerNames_[] =
{
    [POLL_FD_TETHER_TIMER_DISCONNECT] = "disconnection",
};

/* -------------------------------------------------------------------------- */
/* Tether Thread
 *
 * The purpose of the tether thread is to isolate the event loop
 * in the main thread from blocking that might arise when writing to
 * the destination file descriptor. The destination file descriptor
 * cannot be guaranteed to be non-blocking because it is inherited
 * when the watchdog process is started. */

struct TetherPoll
{
    struct TetherThread     *mThread;
    int                      mSrcFd;
    int                      mDstFd;

    struct pollfd            mPollFds[POLL_FD_TETHER_KINDS];
    struct PollFdAction      mPollFdActions[POLL_FD_TETHER_KINDS];
    struct PollFdTimerAction mPollFdTimerActions[POLL_FD_TETHER_TIMER_KINDS];
};

static int
pollFdControl_(struct TetherPoll           *self,
               const struct EventClockTime *aPollTime)
{
    int rc = -1;

    char buf[1];

    ABORT_IF(
        -1 == readFd(
            self->mPollFds[POLL_FD_TETHER_CONTROL].fd, buf, sizeof(buf)),
        {
            terminate(
                errno,
                "Unable to read tether control");
        });

    debug(0, "tether disconnection request received");

    /* Note that gOptions.mTimeout.mDrain_s might be zero to indicate
     * that the no drain timeout is to be enforced. */

    self->mPollFdTimerActions[POLL_FD_TETHER_TIMER_DISCONNECT].mPeriod =
        Duration(NSECS(Seconds(gOptions.mTimeout.mDrain_s)));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static int
pollFdDrain_(struct TetherPoll           *self,
             const struct EventClockTime *aPollTime)
{
    int rc = -1;

    if (self->mPollFds[POLL_FD_TETHER_CONTROL].events)
    {
        {
            lockMutex(&self->mThread->mActivity.mMutex);
            self->mThread->mActivity.mSince = eventclockTime();
            unlockMutex(&self->mThread->mActivity.mMutex);
        }

        bool drained = true;

        do
        {
            /* The output file descriptor must have been closed if:
             *
             *  o There is no input available, so the poll must have
             *    returned because an output disconnection event was detected
             *  o Input was available, but none could be written to the output
             */

            int available;

            ABORT_IF(
                ioctl(self->mSrcFd, FIONREAD, &available),
                {
                    terminate(
                        errno,
                        "Unable to find amount of readable data in fd %d",
                        self->mSrcFd);
                });

            if ( ! available)
            {
                debug(0, "tether drain input empty");
                break;
            }

            /* This splice(2) call will likely block if it is unable to
             * write all the data to the output file descriptor immediately.
             * Note that it cannot block on reading the input file descriptor
             * because that file descriptor is private to this process, the
             * amount of input available is known and is only read by this
             * thread. */

            ssize_t bytes;

            ABORT_IF(
                (bytes = spliceFd(
                    self->mSrcFd, self->mDstFd, available, SPLICE_F_MOVE),
                 -1 == bytes &&
                 EPIPE       != errno &&
                 EWOULDBLOCK != errno &&
                 EINTR       != errno),
                {
                    terminate(
                        errno,
                        "Unable to splice %d bytes from fd %d to fd %d",
                        available,
                        self->mSrcFd,
                        self->mDstFd);
                });

            if ( ! bytes)
            {
                debug(0, "tether drain output closed");
                break;
            }

            if (-1 == bytes)
            {
                if (EPIPE == errno)
                {
                    debug(0, "tether drain output broken");
                    break;
                }
            }
            else
            {
                debug(1,
                      "drained %zd bytes from fd %d to fd %d",
                      bytes, self->mSrcFd, self->mDstFd);
            }

            drained = false;

        } while (0);

        if (drained)
            self->mPollFds[POLL_FD_TETHER_CONTROL].events = 0;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static int
pollFdTimerDisconnected_(struct TetherPoll           *self,
                         const struct EventClockTime *aPollTime)
{
    int rc = -1;

    /* Once the tether drain timeout expires, disable the timer, and
     * force completion of the tether thread. */

    self->mPollFdTimerActions[POLL_FD_TETHER_TIMER_DISCONNECT].mPeriod =
        Duration(NanoSeconds(0));

    self->mPollFds[POLL_FD_TETHER_CONTROL].events = 0;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static bool
pollFdCompletion_(struct TetherPoll *self)
{
    return ! self->mPollFds[POLL_FD_TETHER_CONTROL].events;
}

static int
tetherThreadMain_(struct TetherThread *self)
{
    int rc = -1;

    struct PollFd *pollfd = 0;

    struct ThreadSigMask *threadSigMask = 0;

    {
        lockMutex(&self->mState.mMutex);
        self->mState.mValue = TETHER_THREAD_RUNNING;
        unlockMutexSignal(&self->mState.mMutex, &self->mState.mCond);
    }

    /* Do not open, or close files in this thread because it will race
     * the main thread forking the child process. When forking the
     * child process, it is important to control the file descriptors
     * inherited by the chlid. */

    int srcFd     = STDIN_FILENO;
    int dstFd     = STDOUT_FILENO;
    int controlFd = self->mControlPipe->mRdFile->mFd;

    /* The file descriptor for stdin is a pipe created by the watchdog
     * so it is known to be nonblocking. The file descriptor for stdout
     * is inherited, so it is likely blocking. */

    ensure(nonBlockingFd(srcFd));

    /* The tether thread is configured to receive SIGALRM, but
     * these signals are not delivered until the thread is
     * flushed after the child process has terminated. */

    struct ThreadSigMask threadSigMask_;
    threadSigMask = pushThreadSigMask(
        &threadSigMask_, ThreadSigMaskUnblock, (const int []) { SIGALRM, 0 });

    struct TetherPoll tetherpoll =
    {
        .mThread = self,
        .mSrcFd  = srcFd,
        .mDstFd  = dstFd,

        .mPollFds =
        {
            [POLL_FD_TETHER_CONTROL]= {.fd     = controlFd,
                                       .events = POLL_INPUTEVENTS },
            [POLL_FD_TETHER_INPUT]  = {.fd     =  srcFd,
                                       .events = POLL_INPUTEVENTS },
            [POLL_FD_TETHER_OUTPUT] = {.fd     = dstFd,
                                       .events = POLL_DISCONNECTEVENT},
        },

        .mPollFdActions =
        {
            [POLL_FD_TETHER_CONTROL] = {
                PollFdCallbackMethod(pollFdControl_, &tetherpoll) },
            [POLL_FD_TETHER_INPUT]   = {
                PollFdCallbackMethod(pollFdDrain_, &tetherpoll) },
            [POLL_FD_TETHER_OUTPUT]  = {
                PollFdCallbackMethod(pollFdDrain_, &tetherpoll) },
        },

        .mPollFdTimerActions =
        {
            [POLL_FD_TETHER_TIMER_DISCONNECT] = {
                PollFdCallbackMethod(pollFdTimerDisconnected_, &tetherpoll) },
        },
    };

    struct PollFd pollfd_;
    ERROR_IF(
        createPollFd(
            &pollfd_,
            tetherpoll.mPollFds,
            tetherpoll.mPollFdActions,
            pollFdNames_, POLL_FD_TETHER_KINDS,
            tetherpoll.mPollFdTimerActions,
            pollFdTimerNames_, POLL_FD_TETHER_TIMER_KINDS,
            PollFdCompletionMethod(pollFdCompletion_, &tetherpoll)));
    pollfd = &pollfd_;

    ERROR_IF(
        runPollFdLoop(pollfd));

    closePollFd(pollfd);
    pollfd = 0;

    threadSigMask = popThreadSigMask(threadSigMask);

    /* Close the input file descriptor so that there is a chance
     * to propagte SIGPIPE to the child process. */

    ERROR_IF(
        dup2(self->mNullPipe->mRdFile->mFd, srcFd) != srcFd);

    /* Shut down the end of the control pipe controlled by this thread,
     * without closing the control pipe file descriptor itself. The
     * monitoring loop is waiting for the control pipe to close before
     * exiting the event loop. */

    ERROR_IF(
        dup2(self->mNullPipe->mRdFile->mFd, controlFd) != controlFd);

    debug(0, "tether emptied");

    {
        lockMutex(&self->mState.mMutex);

        while (TETHER_THREAD_RUNNING == self->mState.mValue)
            waitCond(&self->mState.mCond, &self->mState.mMutex);

        unlockMutex(&self->mState.mMutex);
    }

    rc = 0;

Finally:

    FINALLY
    ({
        closePollFd(pollfd);

        threadSigMask = popThreadSigMask(threadSigMask);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
closeTetherThread_(struct TetherThread *self)
{
    closePipe(self->mControlPipe);

    destroyCond(&self->mState.mCond);
    destroyMutex(&self->mState.mMutex);
    destroyMutex(&self->mActivity.mMutex);
}

/* -------------------------------------------------------------------------- */
int
createTetherThread(struct TetherThread *self, struct Pipe *aNullPipe)
{
    int rc = -1;

    createMutex(&self->mActivity.mMutex);
    createMutex(&self->mState.mMutex);
    createCond(&self->mState.mCond);

    self->mControlPipe     = 0;
    self->mNullPipe        = aNullPipe;
    self->mActivity.mSince = eventclockTime();
    self->mState.mValue    = TETHER_THREAD_STOPPED;
    self->mFlushed         = false;

    ERROR_IF(
        createPipe(&self->mControlPipe_, O_CLOEXEC | O_NONBLOCK));
    self->mControlPipe = &self->mControlPipe_;

    {
        struct ThreadSigMask threadSigMask;

        pushThreadSigMask(&threadSigMask, ThreadSigMaskBlock, 0);

        createThread(&self->mThread, 0,
                     ThreadMethod(tetherThreadMain_, self));

        popThreadSigMask(&threadSigMask);
    }

    {
        lockMutex(&self->mState.mMutex);

        while (TETHER_THREAD_STOPPED == self->mState.mValue)
            waitCond(&self->mState.mCond, &self->mState.mMutex);

        unlockMutex(&self->mState.mMutex);
    }

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            closeTetherThread_(self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
void
pingTetherThread(struct TetherThread *self)
{
    debug(0, "ping tether thread");

    ABORT_IF(
        (errno = pthread_kill(self->mThread, SIGALRM)),
        {
            terminate(
                errno,
                "Unable to signal tether thread");
        });
}

/* -------------------------------------------------------------------------- */
void
flushTetherThread(struct TetherThread *self)
{
    debug(0, "flushing tether thread");

    ABORT_IF(
        watchProcessClock(IntMethodNil(), Duration(NanoSeconds(0))),
        {
            terminate(
                errno,
                "Unable to configure synchronisation clock");
        });

    char buf[1] = { 0 };

    ssize_t wrlen;
    ABORT_IF(
        (wrlen = writeFile(self->mControlPipe->mWrFile, buf, sizeof(buf)),
         -1 == wrlen
         ? EPIPE != errno
         : (errno = 0, sizeof(buf) != wrlen)),
        {
            /* This code will race the tether thread which might finished
             * because it already has detected that the child process has
             * terminated and closed its file descriptors. */

            terminate(
                errno,
                "Unable to flush tether thread");
        });

    self->mFlushed = true;
}

/* -------------------------------------------------------------------------- */
void
closeTetherThread(struct TetherThread *self)
{
    if (self)
    {
        ensure(self->mFlushed);

        /* This method is not called until the tether thread has closed
         * its end of the control pipe to indicate that it has completed.
         * At that point the thread is waiting for the thread state
         * to change so that it can exit. */

        debug(0, "synchronising tether thread");

        {
            lockMutex(&self->mState.mMutex);

            ensure(TETHER_THREAD_RUNNING == self->mState.mValue);
            self->mState.mValue = TETHER_THREAD_STOPPING;

            unlockMutexSignal(&self->mState.mMutex, &self->mState.mCond);
        }

        ABORT_IF(
            joinThread(&self->mThread));

        ABORT_IF(
            unwatchProcessClock());

        closeTetherThread_(self);
    }
}

/* -------------------------------------------------------------------------- */
