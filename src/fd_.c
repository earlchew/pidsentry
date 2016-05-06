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

#include "fd_.h"
#include "macros_.h"
#include "error_.h"
#include "timekeeping_.h"
#include "test_.h"
#include "process_.h"

#include <errno.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/file.h>
#include <sys/resource.h>
#include <sys/poll.h>

#include <valgrind/valgrind.h>

/* -------------------------------------------------------------------------- */
#define DEVNULLPATH "/dev/null"

static const char devNullPath_[] = DEVNULLPATH;

/* -------------------------------------------------------------------------- */
void
closeFd(int *aFd)
{
    int fd = *aFd;

    if (-1 != fd)
    {
        *aFd = -1;
        ABORT_IF(
            close(fd));
    }
}

/* -------------------------------------------------------------------------- */
static int
rankFd_(const void *aLhs, const void *aRhs)
{
    int lhs = * (const int *) aLhs;
    int rhs = * (const int *) aRhs;

    if (lhs < rhs) return -1;
    if (lhs > rhs) return +1;
    return 0;
}

int
closeFdDescriptors(const int *aWhiteList, size_t aWhiteListLen)
{
    int rc = -1;

    if (aWhiteListLen)
    {
        struct rlimit noFile;

        ERROR_IF(
            getrlimit(RLIMIT_NOFILE, &noFile));

        int whiteList[aWhiteListLen + 1];

        whiteList[aWhiteListLen] = noFile.rlim_cur;

        for (size_t ix = 0; aWhiteListLen > ix; ++ix)
        {
            whiteList[ix] = aWhiteList[ix];
            ensure(whiteList[aWhiteListLen] > whiteList[ix]);
        }

        qsort(
            whiteList,
            NUMBEROF(whiteList), sizeof(whiteList[0]), rankFd_);

        unsigned purgedFds = 0;

        for (int fd = 0, wx = 0; NUMBEROF(whiteList) > wx; )
        {
            if (0 > whiteList[wx])
            {
                ++wx;
                continue;
            }

            do
            {
                if (fd != whiteList[wx])
                {
                    ++purgedFds;

                    int valid;
                    ERROR_IF(
                        (valid = ownFdValid(fd),
                         -1 == valid));

                    if (valid)
                    {
                        int closedFd = fd;

                        closeFd(&closedFd);
                    }
                }
                else
                {
                    debug(0, "not closing fd %d", fd);

                    while (NUMBEROF(whiteList) > ++wx)
                    {
                        if (whiteList[wx] != fd)
                            break;
                    }
                }

            } while (0);

            ++fd;
        }

        debug(0, "purged %u fds", purgedFds);
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
        ERROR_IF(
            aCloseOnExec != O_CLOEXEC,
            {
                errno = EINVAL;
            });

        /* Take care. O_CLOEXEC is the flag for obtaining close-on-exec
         * semantics when using open(), but fcntl() requires FD_CLOEXEC. */

        closeOnExec = FD_CLOEXEC;
    }

    int flags;
    ERROR_IF(
        (flags = fcntl(aFd, F_GETFD),
         -1 == flags));

    ERROR_IF(
        fcntl(aFd, F_SETFD, (flags & ~FD_CLOEXEC) | closeOnExec));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
nullifyFd(int aFd)
{
    int rc = -1;
    int fd;

    /* Take a process lock to avoid the possibility of a concurrent
     * fork() ending up with more file descriptors than it anticipated. */

    struct ProcessAppLock *appLock;

    appLock = createProcessAppLock();

    int closeExec;
    ERROR_IF(
        (closeExec = ownFdCloseOnExec(aFd),
         -1 == closeExec));

    if (closeExec)
        closeExec = O_CLOEXEC;

    ERROR_IF(
        (fd = open(devNullPath_, O_WRONLY | closeExec),
         -1 == fd));

    if (fd == aFd)
        fd = -1;
    else
        ERROR_IF(
            aFd != dup2(fd, aFd));

    rc = 0;

Finally:

    FINALLY
    ({
        closeFd(&fd);

        destroyProcessAppLock(appLock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
nonBlockingFd(int aFd)
{
    int rc = -1;

    int statusFlags;
    ERROR_IF(
        (statusFlags = fcntl(aFd, F_GETFL),
         -1 == statusFlags));

    int descriptorFlags;
    ERROR_IF(
        (descriptorFlags = fcntl(aFd, F_GETFD),
         -1 == descriptorFlags));

    /* Because O_NONBLOCK affects the underlying open file, to get some
     * peace of mind, only allow non-blocking mode on file descriptors
     * that are not going to be shared. This is not a water-tight
     * defense, but seeks to prevent some careless mistakes. */

    ERROR_UNLESS(
        descriptorFlags & FD_CLOEXEC,
        {
            errno = EBADF;
        });

    if ( ! (statusFlags & O_NONBLOCK))
        ERROR_IF(
            fcntl(aFd, F_SETFL, statusFlags | O_NONBLOCK));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
ownFdNonBlocking(int aFd)
{
    int rc = -1;

    int flags;
    ERROR_IF(
        (flags = fcntl(aFd, F_GETFL),
         -1 == flags));

    rc = flags & O_NONBLOCK ? 1 : 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
ownFdCloseOnExec(int aFd)
{
    int rc = -1;

    int flags;
    ERROR_IF(
        (flags = fcntl(aFd, F_GETFD),
         -1 == flags));

    rc = flags & FD_CLOEXEC ? 1 : 0;

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

    ERROR_IF(
        (-1 == fcntl(aFd, F_GETFL) && (valid = 0, EBADF != errno)));

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
            ERROR_IF(
                (bytes = read(aSrcFd, buffer, len),
                 -1 == bytes && EINTR != errno));
        while (-1 == bytes);

        if (bytes)
        {
            char *bufptr = buffer;
            char *bufend = bufptr + bytes;

            while (bufptr != bufend)
            {
                ssize_t wrote;

                do
                    ERROR_IF(
                        (wrote = write(aDstFd, bufptr, bufend - bufptr),
                         -1 == wrote && EINTR != errno));
                while (-1 == wrote);

                bufptr += wrote;
            }
        }

        rc = bytes;
    }

Finally:

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
waitFdReady_(int aFd, unsigned aPollMask, const struct Duration *aTimeout)
{
    int rc = -1;

    struct pollfd pollfd[1] =
    {
        {
            .fd      = aFd,
            .events  = aPollMask,
            .revents = 0,
        },
    };

    struct EventClockTime since = EVENTCLOCKTIME_INIT;
    struct Duration       remaining;

    const struct Duration timeout =
        aTimeout ? *aTimeout : Duration(NanoSeconds(0));

    struct ProcessSigContTracker sigContTracker = ProcessSigContTracker();

    while (1)
    {
        struct EventClockTime tm = eventclockTime();

        TEST_RACE
        ({
            /* In case the process is stopped after the time is
             * latched, check once more if the fds are ready
             * before checking the deadline. */

            int events;
            ERROR_IF(
                (events = poll(pollfd, NUMBEROF(pollfd), 0),
                 -1 == events));

            if (events)
                break;
        });

        int timeout_ms;

        if ( ! aTimeout)
            timeout_ms = -1;
        else
        {
            if (deadlineTimeExpired(&since, timeout, &remaining, &tm))
            {
                if (checkProcessSigContTracker(&sigContTracker))
                {
                    since = (struct EventClockTime) EVENTCLOCKTIME_INIT;
                    continue;
                }

                break;
            }

            uint64_t remaining_ms = MSECS(remaining.duration).ms;

            timeout_ms = remaining_ms;

            if (0 > timeout_ms || timeout_ms != remaining_ms)
                timeout_ms = INT_MAX;
        }

        int events;
        ERROR_IF(
            (events = poll(pollfd, NUMBEROF(pollfd), timeout_ms),
             -1 == events && EINTR != errno));

        switch (events)
        {
        default:
            break;

        case -1:
            continue;

        case 0:
            pollfd[0].revents = 0;
            continue;
        }

        break;
    }

    rc = !! (pollfd[0].revents & aPollMask);

Finally:

    FINALLY({});

    return rc;
}

int
waitFdWriteReady(int aFd, const struct Duration *aTimeout)
{
    return waitFdReady_(aFd, POLLOUT, aTimeout);
}

int
waitFdReadReady(int aFd, const struct Duration *aTimeout)
{
    return waitFdReady_(aFd, POLLPRI | POLLIN, aTimeout);
}

/* -------------------------------------------------------------------------- */
ssize_t
readFd(int aFd, char *aBuf, size_t aLen)
{
    ssize_t rc = -1;

    char *bufPtr = aBuf;
    char *bufEnd = bufPtr + aLen;

    while (bufPtr != bufEnd)
    {
        ssize_t len;

        ERROR_IF(
            (len = read(aFd, bufPtr, bufEnd - bufPtr),
             -1 == len && EINTR != errno && bufPtr == aBuf));

        if ( ! len)
            break;

        if (-1 == len)
        {
            if (EINTR == errno)
                continue;

            break;
        }

        bufPtr += len;
    }

    rc = bufPtr - aBuf;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
ssize_t
writeFd(int aFd, const char *aBuf, size_t aLen)
{
    ssize_t rc = -1;

    const char *bufPtr = aBuf;
    const char *bufEnd = bufPtr + aLen;

    while (bufPtr != bufEnd)
    {
        ssize_t len;

        ERROR_IF(
            (len = write(aFd, bufPtr, bufEnd - bufPtr),
             -1 == len && EINTR != errno && bufPtr == aBuf));

        if ( ! len)
            break;

        if (-1 == len)
        {
            if (EINTR == errno)
                continue;

            break;
        }

        bufPtr += len;
    }

    rc = bufPtr - aBuf;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
ssize_t
readFdFully(int aFd, char **aBuf, size_t aBufSize)
{
    ssize_t rc = -1;

    char   *buf = 0;
    char   *end = buf;
    size_t  len = end - buf;

    while (1)
    {
        size_t avail = len - (end - buf);

        if ( ! avail)
        {
            len =
                len ? 2 * len :
                testMode(TestLevelRace) ? 1 :
                aBufSize ? aBufSize : getpagesize();

            char *ptr;
            ERROR_UNLESS(
                (ptr = realloc(buf, len)));

            end = ptr + (end - buf);
            buf = ptr;
            continue;
        }

        ssize_t rdlen;
        ERROR_IF(
            (rdlen = readFd(aFd, end, avail),
             -1 == rdlen));
        if ( ! rdlen)
            break;
        end += rdlen;
    }

    rc = end - buf;

    if ( ! rc)
        *aBuf = 0;
    else
    {
        *aBuf = buf;
        buf   = 0;
    }

Finally:

    FINALLY
    ({
        free(buf);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
off_t
lseekFd(int aFd, off_t aOffset, struct WhenceType aWhenceType)
{
    off_t rc = -1;

    int whenceType;
    switch (aWhenceType.mType)
    {
    default:
        ensure(0);

    case WhenceTypeStart_:
        whenceType = SEEK_SET;
        break;

    case WhenceTypeHere_:
        whenceType = SEEK_CUR;
        break;

    case WhenceTypeEnd_:
        whenceType = SEEK_END;
        break;
    }

    rc = lseek(aFd, aOffset, whenceType);

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
lockFd(int aFd, struct LockType aLockType)
{
    int rc = -1;

    int lockType;
    switch (aLockType.mType)
    {
    default:
        ensure(0);

    case LockTypeWrite_:
        lockType = LOCK_EX;
        break;

    case LockTypeRead_:
        lockType = LOCK_SH;
        break;
    }

    int err;
    do
        ERROR_IF(
            (err = flock(aFd, lockType),
             -1 == err && EINTR != errno));
    while (err);

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

    ERROR_IF(
        flock(aFd, LOCK_UN));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
lockFdRegion(int aFd, struct LockType aLockType, off_t aPos, off_t aLen)
{
    int rc = -1;

    int lockType;
    switch (aLockType.mType)
    {
    default:
        ensure(0);

    case LockTypeWrite_:
        lockType = F_WRLCK;
        break;

    case LockTypeRead_:
        lockType = F_RDLCK;
        break;
    }

    struct flock lockRegion =
    {
        .l_type   = lockType,
        .l_whence = SEEK_SET,
        .l_start  = aPos,
        .l_len    = aLen,
    };

    int err;
    do
        ERROR_IF(
            (err = fcntl(aFd, F_SETLKW, &lockRegion),
             -1 == err && EINTR != errno));
    while (err);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
unlockFdRegion(int aFd, off_t aPos, off_t aLen)
{
    int rc = -1;

    struct flock lockRegion =
    {
        .l_type   = F_UNLCK,
        .l_whence = SEEK_SET,
        .l_start  = aPos,
        .l_len    = aLen,
    };

    ERROR_IF(
        fcntl(aFd, F_SETLK, &lockRegion));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
