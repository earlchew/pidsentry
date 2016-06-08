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
    struct TetherThread *mThread;
    int                  mSrcFd;
    int                  mDstFd;
    char                *mBuf;
    size_t               mBufLen;
    char                *mBufPtr;
    char                *mBufEnd;

    struct pollfd            mPollFds[POLL_FD_TETHER_KINDS];
    struct PollFdAction      mPollFdActions[POLL_FD_TETHER_KINDS];
    struct PollFdTimerAction mPollFdTimerActions[POLL_FD_TETHER_TIMER_KINDS];
};

static CHECKED int
pollFdControl_(struct TetherPoll           *self,
               const struct EventClockTime *aPollTime)
{
    int rc = -1;

    char buf[1];

    ERROR_IF(
        -1 == readFd(
            self->mPollFds[POLL_FD_TETHER_CONTROL].fd, buf, sizeof(buf), 0));

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

static CHECKED int
pollFdDrainCopy_(struct TetherPoll           *self,
                 const struct EventClockTime *aPollTime)
{
    int rc = -1;

    int drained = 1;

    do
    {
        if (self->mBufPtr == self->mBufEnd)
        {
            int available;

            ERROR_IF(
                ioctl(self->mSrcFd, FIONREAD, &available));

            if ( ! available)
            {
                debug(0, "tether drain input empty");
                break;
            }

            /* This read(2) call should not block since the file
             * descriptor is created by the sentry and only read
             * in this thread. */

            ssize_t rdSize = -1;

            ERROR_IF(
                (rdSize = read(self->mSrcFd, self->mBuf, self->mBufLen),
                 -1 == rdSize && EINTR != errno && EWOULDBLOCK != errno));

            /* This is unlikely to happen since the ioctl() reported
             * data, and this is the only thread that should be reading
             * the data. Proceed defensively, rather than erroring out. */

            if ( ! rdSize)
            {
                debug(0, "tether drain input closed");
                break;
            }

            if (-1 != rdSize)
            {
                debug(1, "read %zd bytes from fd %d", rdSize, self->mSrcFd);

                ensure(rdSize <= self->mBufLen);

                self->mBufPtr = self->mBuf;
                self->mBufEnd = self->mBufPtr + rdSize;

                struct pollfd *pollFds = self->mPollFds;

                pollFds[POLL_FD_TETHER_INPUT].events  = POLL_DISCONNECTEVENT;
                pollFds[POLL_FD_TETHER_OUTPUT].events = POLL_OUTPUTEVENTS;
            }
        }
        else
        {
            /* This write(2) call will likely block if it is unable to
             * write all the data to the output file descriptor
             * immediately. */

            ssize_t wrSize = -1;

            ERROR_IF(
                (wrSize = write(self->mDstFd,
                                self->mBufPtr,
                                self->mBufEnd - self->mBufPtr),
                 -1 == wrSize && (EPIPE       != errno &&
                                  EWOULDBLOCK != errno &&
                                  EINTR       != errno)));
            if ( ! wrSize)
            {
                debug(0, "tether drain output closed");
                break;
            }

            if (-1 == wrSize)
            {
                if (EPIPE == errno)
                {
                    debug(0, "tether drain output broken");
                    break;
                }
            }
            else
            {
                debug(1, "wrote %zd bytes to fd %d", wrSize, self->mDstFd);

                ensure(wrSize <= self->mBufEnd - self->mBufPtr);

                self->mBufPtr += wrSize;

                if (self->mBufEnd == self->mBufPtr)
                {
                    struct pollfd *pollFds = self->mPollFds;

                    pollFds[POLL_FD_TETHER_INPUT].events = POLL_INPUTEVENTS;
                    pollFds[POLL_FD_TETHER_OUTPUT].events= POLL_DISCONNECTEVENT;
                }
            }
        }

        /* Reach here on input if the read file descriptor is not
         * yet closed (though the read might not have yielded any
         * data yet), or on output if there is still some more
         * data to be written. */

        drained = 0;

    } while (0);

    rc = drained;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
pollFdDrainSplice_(struct TetherPoll           *self,
                   const struct EventClockTime *aPollTime)
{
    int rc = -1;

#ifndef __linux__

    errno = ENOSYS;

#else
    int drained = 1;

    do
    {
        /* If there is no input available, the poll must have
         * returned because either an input disconnection event
         * or output disconnection event was detected. In either
         * case, the tether can be considered drained.
         *
         * If input is available, the input cannot have been
         * disconnected, though there is the possibility that
         * the output might have been in which case the splice()
         * call will fail. */

        int available;

        ERROR_IF(
            ioctl(self->mSrcFd, FIONREAD, &available));

        if ( ! available)
        {
            debug(0, "tether drain input empty");
            break;
        }

        /* Use the amount of data available in the input file descriptor
         * to specify the amount of data to splice.
         *
         * This splice(2) call will likely block if it is unable to
         * write all the data to the output file descriptor immediately.
         * Note that it cannot block on reading the input file descriptor
         * because that file descriptor is private to this process, the
         * amount of input available is known and is only read by this
         * thread. */

        ssize_t splicedBytes;

        ERROR_IF(
            (splicedBytes = spliceFd(
                self->mSrcFd, self->mDstFd, available, SPLICE_F_MOVE),
             -1 == splicedBytes &&
             EPIPE       != errno &&
             EWOULDBLOCK != errno &&
             EINTR       != errno));

        if ( ! splicedBytes)
        {
            debug(0, "tether drain output closed");
            break;
        }

        if (-1 == splicedBytes)
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
                  splicedBytes, self->mSrcFd, self->mDstFd);

            int srcFdReady = -1;
            ERROR_IF(
                (srcFdReady = waitFdReadReady(self->mSrcFd, &ZeroDuration),
                 -1 == srcFdReady));

            if (srcFdReady)
            {
                /* Some data was drained, but there is more input available.
                 * Perhaps the output file descriptor queues are full,
                 * so wait until more can be written. */

                struct pollfd *pollFds = self->mPollFds;

                pollFds[POLL_FD_TETHER_INPUT].events  = POLL_DISCONNECTEVENT;
                pollFds[POLL_FD_TETHER_OUTPUT].events = POLL_OUTPUTEVENTS;
            }
            else
            {
                /* Some output was drained, and now there is no more input
                 * available. This must mean that all the input was
                 * drained, so wait for some more. */

                struct pollfd *pollFds = self->mPollFds;

                pollFds[POLL_FD_TETHER_INPUT].events  = POLL_INPUTEVENTS;
                pollFds[POLL_FD_TETHER_OUTPUT].events = POLL_DISCONNECTEVENT;
            }
        }

        drained = 0;

    } while (0);

    rc = drained;

Finally:

    FINALLY({});
#endif

    return rc;
}

static CHECKED int
pollFdDrain_(struct TetherPoll           *self,
             const struct EventClockTime *aPollTime)
{
    int rc = -1;

    if (self->mPollFds[POLL_FD_TETHER_CONTROL].events)
    {
        {
            pthread_mutex_t *lock = lockMutex(self->mThread->mActivity.mMutex);
            self->mThread->mActivity.mSince = eventclockTime();
            lock = unlockMutex(lock);
        }

        int drained = -1;

        if (self->mBuf)
            ERROR_IF(
                (drained = pollFdDrainCopy_(self, aPollTime),
                 -1 == drained));
        else
            ERROR_IF(
                (drained = pollFdDrainSplice_(self, aPollTime),
                 -1 == drained));

        if (drained)
            self->mPollFds[POLL_FD_TETHER_CONTROL].events = 0;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
pollFdTimerDisconnected_(struct TetherPoll           *self,
                         const struct EventClockTime *aPollTime)
{
    int rc = -1;

    /* Once the tether drain timeout expires, disable the timer, and
     * force completion of the tether thread. */

    self->mPollFdTimerActions[POLL_FD_TETHER_TIMER_DISCONNECT].mPeriod =
        ZeroDuration;

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

static CHECKED int
tetherThreadMain_(struct TetherThread *self)
{
    int rc = -1;

    struct PollFd *pollfd = 0;

    struct ThreadSigMask *threadSigMask = 0;

    {
        pthread_mutex_t *lock = lockMutex(self->mState.mMutex);
        self->mState.mValue = TETHER_THREAD_RUNNING;
        lock = unlockMutexSignal(lock, self->mState.mCond);
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

    /* The splice() call is not supported on Linux if stdout is configured
     * for O_APPEND. In this case, fall back to using the slower
     * read-write approach to transfer data. For more information
     * see the following:
     *
     * https://bugzilla.kernel.org/show_bug.cgi?id=82841 */

    bool useReadWrite = true;

#ifdef __linux__
    {
        int dstFlags = -1;

        ERROR_IF(
            (dstFlags = ownFdFlags(dstFd),
             -1 == dstFlags));

        useReadWrite = !! (dstFlags & O_APPEND);
    }
#endif

    if (testAction(TestLevelRace))
        useReadWrite = ! useReadWrite;

    char readWriteBuffer[8 * 1024];

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
        .mBuf    = useReadWrite ? readWriteBuffer : 0,
        .mBufLen = sizeof(readWriteBuffer),
        .mBufPtr = 0,
        .mBufEnd = 0,

        .mPollFds =
        {
            [POLL_FD_TETHER_CONTROL]= {.fd     = controlFd,
                                       .events = POLL_INPUTEVENTS },
            [POLL_FD_TETHER_INPUT]  = {.fd     = srcFd,
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

    pollfd = closePollFd(pollfd);

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
        pthread_mutex_t *lock = lockMutex(self->mState.mMutex);

        while (TETHER_THREAD_RUNNING == self->mState.mValue)
            waitCond(self->mState.mCond, lock);

        lock = unlockMutex(lock);
    }

    rc = 0;

Finally:

    FINALLY
    ({
        pollfd = closePollFd(pollfd);

        threadSigMask = popThreadSigMask(threadSigMask);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
closeTetherThread_(struct TetherThread *self)
{
    self->mControlPipe = closePipe(self->mControlPipe);

    self->mState.mCond     = destroyCond(self->mState.mCond);
    self->mState.mMutex    = destroyMutex(self->mState.mMutex);
    self->mActivity.mMutex = destroyMutex(self->mActivity.mMutex);
}

/* -------------------------------------------------------------------------- */
int
createTetherThread(struct TetherThread *self, struct Pipe *aNullPipe)
{
    int rc = -1;

    self->mActivity.mMutex = createMutex(&self->mActivity.mMutex_);
    self->mState.mMutex    = createMutex(&self->mState.mMutex_);
    self->mState.mCond     = createCond(&self->mState.mCond_);

    self->mControlPipe     = 0;
    self->mNullPipe        = aNullPipe;
    self->mActivity.mSince = eventclockTime();
    self->mState.mValue    = TETHER_THREAD_STOPPED;
    self->mFlushed         = false;

    ERROR_IF(
        createPipe(&self->mControlPipe_, O_CLOEXEC | O_NONBLOCK));
    self->mControlPipe = &self->mControlPipe_;

    {
        struct ThreadSigMask  threadSigMask_;
        struct ThreadSigMask *threadSigMask =
            pushThreadSigMask(&threadSigMask_, ThreadSigMaskBlock, 0);

        createThread(&self->mThread, 0,
                     ThreadMethod(tetherThreadMain_, self));

        threadSigMask = popThreadSigMask(threadSigMask);
    }

    {
        pthread_mutex_t *lock = lockMutex(self->mState.mMutex);

        while (TETHER_THREAD_STOPPED == self->mState.mValue)
            waitCond(self->mState.mCond, lock);

        lock = unlockMutex(lock);
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
int
pingTetherThread(struct TetherThread *self)
{
    int rc = -1;

    debug(0, "ping tether thread");

    ERROR_IF(
        (errno = pthread_kill(self->mThread, SIGALRM)));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
flushTetherThread(struct TetherThread *self)
{
    int rc = -1;

    debug(0, "flushing tether thread");

    ERROR_IF(
        watchProcessClock(WatchProcessMethodNil(), ZeroDuration));

    /* This code will race the tether thread which might finished
     * because it already has detected that the child process has
     * terminated and closed its file descriptors. */

    char buf[1] = { 0 };

    ssize_t wrlen;
    ERROR_IF(
        (wrlen = writeFile(self->mControlPipe->mWrFile, buf, sizeof(buf), 0),
         -1 == wrlen
         ? EPIPE != errno
         : (errno = 0, sizeof(buf) != wrlen)));

    self->mFlushed = true;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
struct TetherThread *
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
            pthread_mutex_t *lock = lockMutex(self->mState.mMutex);

            ensure(TETHER_THREAD_RUNNING == self->mState.mValue);
            self->mState.mValue = TETHER_THREAD_STOPPING;

            lock = unlockMutexSignal(lock, self->mState.mCond);
        }

        ABORT_IF(
            joinThread(&self->mThread));

        ABORT_IF(
            unwatchProcessClock());

        closeTetherThread_(self);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
