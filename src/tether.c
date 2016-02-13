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

#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>

#include <sys/ioctl.h>

/* -------------------------------------------------------------------------- */
enum TetherFdKind
{
    TETHER_FD_CONTROL,
    TETHER_FD_INPUT,
    TETHER_FD_OUTPUT,
    TETHER_FD_KINDS
};

enum TetherFdTimerKind
{
    TETHER_FD_TIMER_DISCONNECT,
    TETHER_FD_TIMER_KINDS
};

static const char *sTetherFdNames[] =
{
    [TETHER_FD_CONTROL] = "control",
    [TETHER_FD_INPUT]   = "input",
    [TETHER_FD_OUTPUT]  = "output",
};

static const char *sTetherFdTimerNames[] =
{
    [TETHER_FD_TIMER_DISCONNECT] = "disconnection",
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

    struct pollfd            mPollFds[TETHER_FD_KINDS];
    struct PollFdAction      mPollFdActions[TETHER_FD_KINDS];
    struct PollFdTimerAction mPollFdTimerActions[TETHER_FD_TIMER_KINDS];
};

static void
polltethercontrol(void                        *self_,
                  const struct EventClockTime *aPollTime)
{
    struct TetherPoll *self = self_;

    char buf[1];

    if (0 > readFd(self->mPollFds[TETHER_FD_CONTROL].fd, buf, sizeof(buf)))
        terminate(
            errno,
            "Unable to read tether control");

    debug(0, "tether disconnection request received");

    /* Note that gOptions.mTimeout.mDrain_s might be zero to indicate
     * that the no drain timeout is to be enforced. */

    self->mPollFdTimerActions[TETHER_FD_TIMER_DISCONNECT].mPeriod =
        Duration(NSECS(Seconds(gOptions.mTimeout.mDrain_s)));
}

static void
polltetherdrain(void                        *self_,
                const struct EventClockTime *aPollTime)
{
    struct TetherPoll *self = self_;

    if (self->mPollFds[TETHER_FD_CONTROL].events)
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

            if (ioctl(self->mSrcFd, FIONREAD, &available))
                terminate(
                    errno,
                    "Unable to find amount of readable data in fd %d",
                    self->mSrcFd);

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

            ssize_t bytes = spliceFd(
                self->mSrcFd, self->mDstFd, available, SPLICE_F_MOVE);

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

                if (EWOULDBLOCK != errno && EINTR != errno)
                    terminate(
                        errno,
                        "Unable to splice %d bytes from fd %d to fd %d",
                        available,
                        self->mSrcFd,
                        self->mDstFd);
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
            self->mPollFds[TETHER_FD_CONTROL].events = 0;
    }
}

static void
polltetherdisconnected(void                        *self_,
                       const struct EventClockTime *aPollTime)
{
    struct TetherPoll *self = self_;

    /* Once the tether drain timeout expires, disable the timer, and
     * force completion of the tether thread. */

    self->mPollFdTimerActions[TETHER_FD_TIMER_DISCONNECT].mPeriod =
        Duration(NanoSeconds(0));

    self->mPollFds[TETHER_FD_CONTROL].events = 0;
}

static bool
polltethercompletion(void *self_)
{
    struct TetherPoll *self = self_;

    return ! self->mPollFds[TETHER_FD_CONTROL].events;
}

static void *
tetherThreadMain_(void *self_)
{
    struct TetherThread *self = self_;

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
    int controlFd = self->mControlPipe.mRdFile->mFd;

    /* The file descriptor for stdin is a pipe created by the watchdog
     * so it is known to be nonblocking. The file descriptor for stdout
     * is inherited, so it is likely blocking. */

    ensure(nonblockingFd(srcFd));

    /* The tether thread is configured to receive SIGALRM, but
     * these signals are not delivered until the thread is
     * flushed after the child process has terminated. */

    struct ThreadSigMask threadSigMask;

    const int sigList[] = { SIGALRM, 0 };

    if (pushThreadSigMask(&threadSigMask, ThreadSigMaskUnblock, sigList))
        terminate(errno, "Unable to push thread signal mask");

    struct TetherPoll tetherpoll =
    {
        .mThread = self,
        .mSrcFd  = srcFd,
        .mDstFd  = dstFd,

        .mPollFds =
        {
            [TETHER_FD_CONTROL]= {.fd= controlFd,.events= POLL_INPUTEVENTS },
            [TETHER_FD_INPUT]  = {.fd= srcFd,    .events= POLL_INPUTEVENTS },
            [TETHER_FD_OUTPUT] = {.fd= dstFd,    .events= POLL_DISCONNECTEVENT},
        },

        .mPollFdActions =
        {
            [TETHER_FD_CONTROL] = { polltethercontrol },
            [TETHER_FD_INPUT]   = { polltetherdrain },
            [TETHER_FD_OUTPUT]  = { polltetherdrain },
        },

        .mPollFdTimerActions =
        {
            [TETHER_FD_TIMER_DISCONNECT] = { polltetherdisconnected },
        },
    };

    struct PollFd pollfd;
    if (createPollFd(
            &pollfd,
            tetherpoll.mPollFds,
            tetherpoll.mPollFdActions,
            sTetherFdNames, TETHER_FD_KINDS,
            tetherpoll.mPollFdTimerActions,
            sTetherFdTimerNames, TETHER_FD_TIMER_KINDS,
            polltethercompletion, &tetherpoll))
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

    if (popThreadSigMask(&threadSigMask))
        terminate(errno, "Unable to push process signal mask");

    /* Close the input file descriptor so that there is a chance
     * to propagte SIGPIPE to the child process. */

    if (dup2(self->mNullPipe->mRdFile->mFd, srcFd) != srcFd)
        terminate(
            errno,
            "Unable to dup fd %d to fd %d",
            self->mNullPipe->mRdFile->mFd,
            srcFd);

    /* Shut down the end of the control pipe controlled by this thread,
     * without closing the control pipe file descriptor itself. The
     * monitoring loop is waiting for the control pipe to close before
     * exiting the event loop. */

    if (dup2(self->mNullPipe->mRdFile->mFd, controlFd) != controlFd)
        terminate(errno, "Unable to shut down tether thread control");

    debug(0, "tether emptied");

    {
        lockMutex(&self->mState.mMutex);

        while (TETHER_THREAD_RUNNING == self->mState.mValue)
            waitCond(&self->mState.mCond, &self->mState.mMutex);

        unlockMutex(&self->mState.mMutex);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
void
createTetherThread(struct TetherThread *self, struct Pipe *aNullPipe)
{
    if (createPipe(&self->mControlPipe, O_CLOEXEC | O_NONBLOCK))
        terminate(errno, "Unable to create tether control pipe");

    if (errno = pthread_mutex_init(&self->mActivity.mMutex, 0))
        terminate(errno, "Unable to create activity mutex");

    if (errno = pthread_mutex_init(&self->mState.mMutex, 0))
        terminate(errno, "Unable to create state mutex");

    if (errno = pthread_cond_init(&self->mState.mCond, 0))
        terminate(errno, "Unable to create state condition");

    self->mNullPipe        = aNullPipe;
    self->mActivity.mSince = eventclockTime();
    self->mState.mValue    = TETHER_THREAD_STOPPED;
    self->mFlushed         = false;

    {
        struct ThreadSigMask threadSigMask;

        if (pushThreadSigMask(&threadSigMask, ThreadSigMaskBlock, 0))
            terminate(errno, "Unable to push thread signal mask");

        createThread(&self->mThread, 0, tetherThreadMain_, self);

        if (popThreadSigMask(&threadSigMask))
            terminate(errno, "Unable to pop thread signal mask");
    }

    {
        lockMutex(&self->mState.mMutex);

        while (TETHER_THREAD_STOPPED == self->mState.mValue)
            waitCond(&self->mState.mCond, &self->mState.mMutex);

        unlockMutex(&self->mState.mMutex);
    }
}

/* -------------------------------------------------------------------------- */
void
pingTetherThread(struct TetherThread *self)
{
    debug(0, "ping tether thread");

    if (errno = pthread_kill(self->mThread, SIGALRM))
        terminate(
            errno,
            "Unable to signal tether thread");
}

/* -------------------------------------------------------------------------- */
void
flushTetherThread(struct TetherThread *self)
{
    debug(0, "flushing tether thread");

    if (watchProcessClock(VoidMethod(0, 0), Duration(NanoSeconds(0))))
        terminate(
            errno,
            "Unable to configure synchronisation clock");

    char buf[1] = { 0 };

    if (sizeof(buf) != writeFile(self->mControlPipe.mWrFile, buf, sizeof(buf)))
    {
        /* This code will race the tether thread which might finished
         * because it already has detected that the child process has
         * terminated and closed its file descriptors. */

        if (EPIPE != errno)
            terminate(
                errno,
                "Unable to flush tether thread");
    }

    self->mFlushed = true;
}

/* -------------------------------------------------------------------------- */
void
closeTetherThread(struct TetherThread *self)
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

    (void) joinThread(&self->mThread);

    if (unwatchProcessClock())
        terminate(
            errno,
            "Unable to reset synchronisation clock");

    if (errno = pthread_cond_destroy(&self->mState.mCond))
        terminate(errno, "Unable to destroy state condition");

    if (errno = pthread_mutex_destroy(&self->mState.mMutex))
        terminate(errno, "Unable to destroy state mutex");

    if (errno = pthread_mutex_destroy(&self->mActivity.mMutex))
        terminate(errno, "Unable to destroy activity mutex");

    if (closePipe(&self->mControlPipe))
        terminate(errno, "Unable to close tether control pipe");
}

/* -------------------------------------------------------------------------- */