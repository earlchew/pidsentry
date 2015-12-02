/* -*- c-basic-offset:4; indent-tabs-mode:nil -*- vi: set sw=4 et: */
/*
// Copyright (c) 2015, Earl Chew
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

#include "fd.h"
#include "macros.h"
#include "test.h"
#include "error.h"
#include "timekeeping.h"

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <inttypes.h>
#include <process.h>

#include <sys/time.h>
#include <sys/file.h>

#include <valgrind/valgrind.h>

/* -------------------------------------------------------------------------- */
int
closeFd(int *aFd)
{
    int rc = -1;

    if (-1 != *aFd)
    {
        if (close(*aFd))
            goto Finally;
        *aFd = -1;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
bool
stdFd(int aFd)
{
    return STDIN_FILENO == aFd || STDOUT_FILENO == aFd || STDERR_FILENO == aFd;
}

/* -------------------------------------------------------------------------- */
int
closeFdOnExec(int aFd, unsigned aCloseOnExec)
{
    int rc = -1;

    unsigned closeOnExec = 0;

    if (aCloseOnExec)
    {
        if (aCloseOnExec != O_CLOEXEC)
        {
            errno = EINVAL;
            goto Finally;
        }

        /* Take care. O_CLOEXEC is the flag for obtaining close-on-exec
         * semantics when using open(), but fcntl() requires FD_CLOEXEC. */

        closeOnExec = FD_CLOEXEC;
    }

    long flags = fcntl(aFd, F_GETFD);

    if (-1 == flags)
        goto Finally;

    rc = fcntl(aFd, F_SETFD, (flags & ~FD_CLOEXEC) | closeOnExec);

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
nonblockingFd(int aFd)
{
    int rc = -1;

    long statusFlags = fcntl(aFd, F_GETFL);
    int  descriptorFlags = fcntl(aFd, F_GETFD);

    if (-1 == statusFlags || -1 == descriptorFlags)
        goto Finally;

    /* Because O_NONBLOCK affects the underlying open file, to get some
     * peace of mind, only allow non-blocking mode on file descriptors
     * that are not going to be shared. This is not a water-tight
     * defense, but seeks to prevent some careless mistakes. */

    if ( ! (descriptorFlags & FD_CLOEXEC))
    {
        errno = EBADF;
        goto Finally;
    }

    rc = (statusFlags & O_NONBLOCK) ? 0 : fcntl(aFd, F_SETFL,
                                                statusFlags | O_NONBLOCK);

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
ownFdNonBlocking(int aFd)
{
    int rc = -1;

    int flags = fcntl(aFd, F_GETFL);

    if (-1 == flags)
        goto Finally;

    rc = flags & O_NONBLOCK ? 1 : 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
ownFdValid(int aFd)
{
    int rc = -1;

    int valid = 1;

    if (-1 == fcntl(aFd, F_GETFL))
    {
        if (EBADF != errno)
            goto Finally;
        valid = 0;
    }

    rc = valid;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
ssize_t
spliceFd(int aSrcFd, int aDstFd, size_t aLen, unsigned aFlags)
{
    int rc = -1;

    if ( ! RUNNING_ON_VALGRIND)
        rc = splice(aSrcFd, 0, aDstFd, 0, aLen, aFlags);
    else
    {
        /* Early versions of valgrind do not support splice(), so
         * provide a workaround here. This implementation of splice()
         * does not mimic the kernel implementation exactly, but is
         * enough for testing. */

        char buffer[8192];

        size_t len = sizeof(buffer);

        if (len > aLen)
            len = aLen;

        ssize_t bytes;

        do
            bytes = read(aSrcFd, buffer, len);
        while (-1 == bytes && EINTR == errno);

        if (-1 == bytes)
            goto Finally;

        if (bytes)
        {
            char *bufptr = buffer;
            char *bufend = bufptr + bytes;

            while (bufptr != bufend)
            {
                ssize_t wrote;

                do
                    wrote = write(aDstFd, bufptr, bufend - bufptr);
                while (-1 == wrote  && EINTR == errno);

                if (-1 == wrote)
                    goto Finally;

                bufptr += wrote;
            }
        }

        rc = bytes;
    }

Finally:

    return rc;
}

/* -------------------------------------------------------------------------- */
ssize_t
writeFd(int aFd, const char *aBuf, size_t aLen)
{
    const char *bufPtr = aBuf;
    const char *bufEnd = bufPtr + aLen;

    while (bufPtr != bufEnd)
    {
        ssize_t len;

        len = write(aFd, bufPtr, bufEnd - bufPtr);

        if (-1 == len)
        {
            if (EINTR == errno)
                continue;

            if (bufPtr != aBuf)
                break;

            return -1;
        }

        if ( ! len)
        {
            errno = EWOULDBLOCK;
            return -1;
        }

        bufPtr += len;
    }

    return bufPtr - aBuf;
}

/* -------------------------------------------------------------------------- */
static void
lockFdTimer_(int aSigNum)
{ }

int
lockFd(int aFd, int aType, unsigned aMilliSeconds)
{
    int rc = -1;

    if (LOCK_EX != aType && LOCK_SH != aType)
    {
        errno = EINVAL;
        goto Finally;
    }

    /* Disable the timer and SIGALRM action so that a new
     * timer and action can be installed to provide some
     * protection against deadlocks.
     *
     * Take care to disable the timer, before resetting the
     * signal handler, then re-configuring the timer. */

    struct itimerval prevTimer;

    static const struct itimerval disableTimer =
    {
        .it_value    = { .tv_sec = 0 },
        .it_interval = { .tv_sec = 0 },
    };

    if (setitimer(ITIMER_REAL, &disableTimer, &prevTimer))
        goto Finally;

    struct sigaction prevTimerAction;

    struct sigaction timerAction =
    {
        .sa_handler = lockFdTimer_,
    };

    if (sigaction(SIGALRM, &timerAction, &prevTimerAction))
        goto Finally;

    static const struct itimerval flockTimer =
    {
        .it_value    = { .tv_sec = 1 },
        .it_interval = { .tv_sec = 1 },
    };

    if (setitimer(ITIMER_REAL, &flockTimer, 0))
        goto Finally;

    /* The installed timer will inject periodic SIGALRM signals
     * and cause flock() to return with EINTR. This allows
     * the deadline to be checked periodically. */

    for (uint64_t deadlineTime =
             ownProcessElapsedTime() + milliSeconds(aMilliSeconds); ; )
    {
        if (deadlineTime < ownProcessElapsedTime())
        {
            errno = EDEADLOCK;
            goto Finally;
        }

        /* Very infrequently block here to exercise the EINTR
         * functionality of the delivered SIGALRM signal. */

        if (testAction() && 1 > random() % 10)
        {
            struct timeval timeout =
            {
                .tv_sec = 24 * 60 * 60,
            };

            ensure(-1 == select(0, 0, 0, 0, &timeout) && EINTR == errno);
        }

        if ( ! flock(aFd, aType))
            break;

        if (EINTR != errno)
            goto Finally;
    }

    /* Restore the previous setting of the timer and SIGALRM handler.
     * Take care to disable the timer, before restoring the
     * signal handler, then restoring the setting of the timer. */

    if (setitimer(ITIMER_REAL, &disableTimer, 0))
        goto Finally;

    if (sigaction(SIGALRM, &prevTimerAction, 0))
        goto Finally;

    if (setitimer(ITIMER_REAL, &prevTimer, 0))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
unlockFd(int aFd)
{
    int rc = -1;

    if (flock(aFd, LOCK_UN))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
