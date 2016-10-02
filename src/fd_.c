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
#include "deadline_.h"
#include "fdset_.h"
#include "system_.h"
#include "eintr_.h"

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
int
openFd(const char *aPath, int aFlags, mode_t aMode)
{
    int rc = -1;

    int fd;

    do
    {
        ERROR_IF(
            (fd = open(aPath, aFlags, aMode),
             -1 == fd && EINTR != errno));
    }
    while (-1 == fd);

    rc = fd;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
openBlockedFd_(int aSelector, int aFd)
{
    int rc = -1;
    int fd = -1;

    int pipefd[2];

    ERROR_IF(
        pipe2(pipefd, O_CLOEXEC));

    int selected   = 0 + aSelector;
    int unselected = 1 - aSelector;

    pipefd[unselected] = closeFd(pipefd[unselected]);

    if (aFd != pipefd[selected])
    {
        fd = pipefd[selected];

        ERROR_IF(
            aFd != duplicateFd(fd, aFd));

        fd = closeFd(fd);
    }

    rc = 0;

Finally:

    FINALLY
    ({
        fd = closeFd(fd);
    });

    return rc;
}

static int
openBlockedInput_(int aFd)
{
    return openBlockedFd_(0, aFd);
}

static int
openBlockedOutput_(int aFd)
{
    return openBlockedFd_(1, aFd);
}

int
openStdFds(void)
{
    int rc = -1;

    /* Create a file descriptor to take the place of stdin, stdout or stderr
     * as required. Any created file descriptor will automatically be
     * closed on exec(). If an input file descriptor is created, that
     * descriptor will return eof on any attempted read. Conversely
     * if an output file descriptor is created, that descriptor will
     * return eof or SIGPIPE on any attempted write. */

    int validStdin;
    ERROR_IF(
        (validStdin = ownFdValid(STDIN_FILENO),
         -1 == validStdin));

    int validStdout;
    ERROR_IF(
        (validStdout = ownFdValid(STDOUT_FILENO),
         -1 == validStdout));

    int validStderr;
    ERROR_IF(
        (validStderr = ownFdValid(STDERR_FILENO),
         -1 == validStderr));

    if ( ! validStdin)
        ERROR_IF(
            openBlockedInput_(STDIN_FILENO));

    if ( ! validStdout && ! validStderr)
    {
        ERROR_IF(
            openBlockedOutput_(STDOUT_FILENO));
        ERROR_IF(
            STDERR_FILENO != duplicateFd(STDOUT_FILENO, STDERR_FILENO));
    }
    else if ( ! validStdout)
        ERROR_IF(
            openBlockedOutput_(STDOUT_FILENO));
    else if ( ! validStderr)
        ERROR_IF(
            openBlockedOutput_(STDERR_FILENO));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closeFd(int aFd)
{
    if (-1 != aFd)
    {
        /* Assume Posix semantics as described in:
         *    http://austingroupbugs.net/view.php?id=529
         *
         * Posix semantics are normally available if POSIX_CLOSE_RESTART
         * is defined, but this code will simply assume that these semantics are
         * available.
         *
         * For Posix, if POSIX_CLOSE_RESTART is zero, close() will never return
         * EINTR. Otherwise, close() can return EINTR but it is unspecified
         * if any function other than close() can be used on the file
         * descriptor. Fundamentally this means that if EINTR is returned,
         * the only useful thing that can be done is to close the file
         * descriptor. */

        int err;
        do
        {
            ABORT_IF(
                (err = close(aFd),
                 err && EINTR != errno && EINPROGRESS != errno));
        } while (err && EINTR == errno);
    }

    return -1;
}

/* -------------------------------------------------------------------------- */
struct FdWhiteListVisitor_
{
    int mFd;

    struct rlimit mFdLimit;
};

static int
closeFdWhiteListVisitor_(
    struct FdWhiteListVisitor_ *self, struct FdRange aRange)
{
    int rc = -1;

    /* Be careful not to underflow or overflow the arithmetic representation
     * if rlim_cur happens to be zero, or much wider than aFdLast. */

    int fdEnd = 0;

    if (self->mFdLimit.rlim_cur)
    {
        fdEnd =
            aRange.mRhs < self->mFdLimit.rlim_cur
            ? aRange.mRhs + 1
            : self->mFdLimit.rlim_cur;
    }

    int fdBegin = aRange.mLhs;

    if (fdBegin > fdEnd)
        fdBegin = fdEnd;

    for (int fd = self->mFd; fd < fdBegin; ++fd)
    {
        int valid;

        ERROR_IF(
            (valid = ownFdValid(fd),
             -1 == valid));

        while (valid && closeFd(fd))
            break;
    }

    self->mFd = fdEnd;

    rc = (self->mFdLimit.rlim_cur <= fdEnd);

Finally:

    FINALLY({});

    return rc;
}

int
closeFdExceptWhiteList(const struct FdSet *aFdSet)
{
    int rc = -1;

    struct FdWhiteListVisitor_ whiteListVisitor;

    whiteListVisitor.mFd = 0;

    ERROR_IF(
        getrlimit(RLIMIT_NOFILE, &whiteListVisitor.mFdLimit));

    ERROR_IF(
        -1 == visitFdSet(
            aFdSet,
            FdSetVisitor(&whiteListVisitor, closeFdWhiteListVisitor_)));

    /* Cover off all the file descriptors from the end of the last
     * whitelisted range, to the end of the process file descriptor
     * range. */

    ERROR_UNLESS(
        1 == closeFdWhiteListVisitor_(
            &whiteListVisitor,
            FdRange(
                whiteListVisitor.mFdLimit.rlim_cur,
                whiteListVisitor.mFdLimit.rlim_cur)));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
struct FdBlackListVisitor_
{
    struct rlimit mFdLimit;
};

static int
closeFdBlackListVisitor_(
    struct FdBlackListVisitor_ *self, struct FdRange aRange)
{
    int rc = -1;

    int done = 1;

    int fd = aRange.mLhs;

    while (fd != self->mFdLimit.rlim_cur)
    {
        int valid;

        ERROR_IF(
            (valid = ownFdValid(fd),
             -1 == valid));

        while (valid && closeFd(fd))
            break;

        if (fd == aRange.mRhs)
        {
            done = 0;
            break;
        }

        ++fd;
    }

    rc = done;

Finally:

    FINALLY({});

    return rc;
}

int
closeFdOnlyBlackList(const struct FdSet *aFdSet)
{
    int rc = -1;

    struct FdBlackListVisitor_ blackListVisitor;

    ERROR_IF(
        getrlimit(RLIMIT_NOFILE, &blackListVisitor.mFdLimit));

    ERROR_IF(
        -1 == visitFdSet(
            aFdSet,
            FdSetVisitor(&blackListVisitor, closeFdBlackListVisitor_)));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static CHECKED int
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

                    while (valid && closeFd(fd))
                        break;
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

    int prevFlags;
    ERROR_IF(
        (prevFlags = fcntl(aFd, F_GETFD),
         -1 == prevFlags));

    int nextFlags = (prevFlags & ~FD_CLOEXEC) | closeOnExec;

    if (prevFlags != nextFlags)
        ERROR_IF(
            fcntl(aFd, F_SETFD, nextFlags));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
duplicateFd(int aFd, int aTargetFd)
{
    /* Strangely dup() and dup2() do not preserve FD_CLOEXEC when duplicating
     * the file descriptor. This function interrogates the source file
     * descriptor, and transfers the FD_CLOEXEC flag when duplicating
     * the source to the target file descriptor. */

    int rc = -1;
    int fd = -1;

    int cloexec;
    ERROR_IF(
        (cloexec = ownFdCloseOnExec(aFd),
         -1 == cloexec));

    if (-1 == aTargetFd)
    {
        long otherFd = 0;

        ERROR_IF(
            (fd = fcntl(aFd, cloexec ? F_DUPFD_CLOEXEC : F_DUPFD, otherFd),
             -1 == fd));
    }
    else if (aFd == aTargetFd)
    {
        fd = aTargetFd;
    }
    else
    {
        ERROR_IF(
            (fd = dup3(aFd, aTargetFd, cloexec ? O_CLOEXEC : 0),
             -1 == fd));
    }

    rc = fd;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
nullifyFd(int aFd)
{
    int rc = -1;

    int fd = -1;

    int closeExec;
    ERROR_IF(
        (closeExec = ownFdCloseOnExec(aFd),
         -1 == closeExec));

    if (closeExec)
        closeExec = O_CLOEXEC;

    ERROR_IF(
        (fd = openFd(devNullPath_, O_WRONLY | closeExec, 0),
         -1 == fd));

    if (fd == aFd)
        fd = -1;
    else
        ERROR_IF(
            aFd != duplicateFd(fd, aFd));

    rc = 0;

Finally:

    FINALLY
    ({
        fd = closeFd(fd);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
nonBlockingFd(int aFd, unsigned aNonBlocking)
{
    int rc = -1;

    unsigned nonBlocking = 0;

    if (aNonBlocking)
    {
        ERROR_IF(
            aNonBlocking != O_NONBLOCK,
            {
                errno = EINVAL;
            });

        nonBlocking = O_NONBLOCK;
    }

    int prevStatusFlags;
    ERROR_IF(
        (prevStatusFlags = fcntl(aFd, F_GETFL),
         -1 == prevStatusFlags));

    int nextStatusFlags = (prevStatusFlags & ~O_NONBLOCK) | nonBlocking;

    if (prevStatusFlags != nextStatusFlags)
    {
        if (nonBlocking)
        {
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
        }

        ERROR_IF(
            fcntl(aFd, F_SETFL, nextStatusFlags));
    }

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
ownFdFlags(int aFd)
{
    int rc = -1;

    int flags = -1;

    ERROR_IF(
        (flags = fcntl(aFd, F_GETFL),
         -1 == flags));

    rc = flags;

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
        (-1 == ownFdFlags(aFd) && (valid = 0, EBADF != errno)));

    rc = valid;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
ioctlFd(int aFd, int aReq, void *aArg)
{
    int rc;

    do
    {
        rc = ioctl(aFd, aReq, aArg);
    } while (-1 == rc && EINTR == errno);

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
static CHECKED int
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

    const struct Duration timeout = aTimeout ? *aTimeout : ZeroDuration;

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
                 -1 == events && EINTR != errno));

            if (0 > events)
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
static CHECKED int
waitFdReadyDeadline_(int aFd, unsigned aPollMask, struct Deadline *aDeadline)
{
    int rc = -1;

    int ready = -1;

    if ( ! aDeadline)
        ready = waitFdReady_(aFd, aPollMask, 0);
    else
    {
        struct FdReadyDeadline
        {
            int      mFd;
            unsigned mPollMask;

        } readyDeadline = {
            .mFd       = aFd,
            .mPollMask = aPollMask,
        };

        ERROR_IF(
            (ready = checkDeadlineExpired(
                aDeadline,
                DeadlinePollMethod(
                    &readyDeadline,
                    LAMBDA(
                        int, (struct FdReadyDeadline *self_),
                        {
                            return waitFdReady_(
                                self_->mFd,
                                self_->mPollMask,
                                &ZeroDuration);
                        })),
                DeadlineWaitMethod(
                    &readyDeadline,
                    LAMBDA(
                        int, (struct FdReadyDeadline *self_,
                              const struct Duration  *aTimeout),
                        {
                            return waitFdReady_(
                                self_->mFd,
                                self_->mPollMask,
                                aTimeout);
                        }))),
             -1 == ready));
    }

    rc = ready;

Finally:

    FINALLY({});

    return rc;
}

int
waitFdWriteReadyDeadline(int aFd, struct Deadline *aDeadline)
{
    return waitFdReadyDeadline_(aFd, POLLOUT, aDeadline);
}

int
waitFdReadReadyDeadline(int aFd, struct Deadline *aDeadline)
{
    return waitFdReadyDeadline_(aFd, POLLPRI | POLLIN, aDeadline);
}

/* -------------------------------------------------------------------------- */
static ssize_t
readFdDeadline_(int aFd,
                char *aBuf, size_t aLen, struct Deadline *aDeadline,
                ssize_t aReader(int, void *, size_t))
{
    ssize_t rc = -1;

    char *bufPtr = aBuf;
    char *bufEnd = bufPtr + aLen;

    while (bufPtr != bufEnd)
    {
        if (aDeadline)
        {
            int ready = -1;

            ERROR_IF(
                (ready = checkDeadlineExpired(
                    aDeadline,
                    DeadlinePollMethod(
                        &aFd,
                        LAMBDA(
                            int, (int *fd),
                            {
                                return waitFdReadReady(*fd, &ZeroDuration);
                            })),
                    DeadlineWaitMethod(
                        &aFd,
                        LAMBDA(
                            int, (int *fd,
                                  const struct Duration *aTimeout),
                            {
                                return waitFdReadReady(*fd, aTimeout);
                            }))),
                 -1 == ready && bufPtr == aBuf));

            if (-1 == ready)
                break;

            if ( ! ready)
                continue;
        }

        ssize_t len;

        ERROR_IF(
            (len = read(aFd, bufPtr, bufEnd - bufPtr),
             -1 == len && (EINTR       != errno &&
                           EWOULDBLOCK != errno &&
                           EAGAIN      != errno) && bufPtr == aBuf));

        if ( ! len)
            break;

        if (-1 == len)
        {
            if (EINTR == errno)
                continue;

            if (EWOULDBLOCK == errno || EAGAIN == errno)
            {
                int rdReady;
                ERROR_IF(
                    (rdReady = waitFdReadReadyDeadline(aFd, aDeadline),
                     -1 == rdReady && bufPtr == aBuf));

                if (0 <= rdReady)
                    continue;
            }

            break;
        }

        bufPtr += len;
    }

    rc = bufPtr - aBuf;

Finally:

    FINALLY({});

    return rc;
}

ssize_t
readFdDeadline(int aFd,
               char *aBuf, size_t aLen, struct Deadline *aDeadline)
{
    return readFdDeadline_(aFd, aBuf, aLen, aDeadline, read);
}

/* -------------------------------------------------------------------------- */
static ssize_t
writeFdDeadline_(int aFd,
                 const char *aBuf, size_t aLen, struct Deadline *aDeadline,
                 ssize_t aWriter(int, const void *, size_t))
{
    ssize_t rc = -1;

    const char *bufPtr = aBuf;
    const char *bufEnd = bufPtr + aLen;

    while (bufPtr != bufEnd)
    {
        if (aDeadline)
        {
            int ready = -1;

            ERROR_IF(
                (ready = checkDeadlineExpired(
                    aDeadline,
                    DeadlinePollMethod(
                        &aFd,
                        LAMBDA(
                            int, (int *fd),
                            {
                                return waitFdWriteReady(*fd, &ZeroDuration);
                            })),
                    DeadlineWaitMethod(
                        &aFd,
                        LAMBDA(
                            int, (int *fd,
                                  const struct Duration *aTimeout),
                            {
                                return waitFdWriteReady(*fd, aTimeout);
                            }))),
                 -1 == ready && bufPtr == aBuf));

            if (-1 == ready)
                break;

            if ( ! ready)
                continue;
        }

        ssize_t len;

        ERROR_IF(
            (len = aWriter(aFd, bufPtr, bufEnd - bufPtr),
             -1 == len && (EINTR       != errno &&
                           EWOULDBLOCK != errno &&
                           EAGAIN      != errno) && bufPtr == aBuf));

        if ( ! len)
            break;

        if (-1 == len)
        {
            if (EINTR == errno)
                continue;

            if (EWOULDBLOCK == errno || EAGAIN == errno)
            {
                int wrReady;
                ERROR_IF(
                    (wrReady = waitFdWriteReadyDeadline(aFd, aDeadline),
                     -1 == wrReady && bufPtr == aBuf));

                if (0 <= wrReady)
                    continue;
            }

            break;
        }

        bufPtr += len;
    }

    rc = bufPtr - aBuf;

Finally:

    FINALLY({});

    return rc;
}

ssize_t
writeFdDeadline(int aFd,
                const char *aBuf, size_t aLen, struct Deadline *aDeadline)
{
    return writeFdDeadline_(aFd, aBuf, aLen, aDeadline, &write);
}

/* -------------------------------------------------------------------------- */
static ssize_t
readFd_(int aFd,
        char *aBuf, size_t aLen, const struct Duration *aTimeout,
        ssize_t aReader(int, void *, size_t))
{
    ssize_t rc = -1;

    struct Deadline  deadline_;
    struct Deadline *deadline = 0;

    if (aTimeout)
    {
        ERROR_IF(
            createDeadline(&deadline_, aTimeout));
        deadline = &deadline_;
    }

    rc = readFdDeadline_(aFd, aBuf, aLen, deadline, aReader);

Finally:

    FINALLY
    ({
        deadline = closeDeadline(deadline);
    });

    return rc;
}

ssize_t
readFd(int aFd,
       char *aBuf, size_t aLen, const struct Duration *aTimeout)
{
    return readFd_(aFd, aBuf, aLen, aTimeout, read);
}

ssize_t
readFdRaw(int aFd,
          char *aBuf, size_t aLen, const struct Duration *aTimeout)
{
    return readFd_(aFd, aBuf, aLen, aTimeout, read_raw);
}

/* -------------------------------------------------------------------------- */
static ssize_t
writeFd_(int aFd,
         const char *aBuf, size_t aLen, const struct Duration *aTimeout,
         ssize_t aWriter(int, const void *, size_t))
{
    ssize_t rc = -1;

    struct Deadline  deadline_;
    struct Deadline *deadline = 0;

    if (aTimeout)
    {
        ERROR_IF(
            createDeadline(&deadline_, aTimeout));
        deadline = &deadline_;
    }

    rc = writeFdDeadline_(aFd, aBuf, aLen, deadline, aWriter);

Finally:

    FINALLY
    ({
        deadline = closeDeadline(deadline);
    });

    return rc;
}

ssize_t
writeFd(int aFd,
        const char *aBuf, size_t aLen, const struct Duration *aTimeout)
{
    return writeFd_(aFd, aBuf, aLen, aTimeout, write);
}

ssize_t
writeFdRaw(int aFd,
           const char *aBuf, size_t aLen, const struct Duration *aTimeout)
{
    return writeFd_(aFd, aBuf, aLen, aTimeout, write_raw);
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
                aBufSize ? aBufSize : fetchSystemPageSize();

            char *ptr;
            ERROR_UNLESS(
                (ptr = realloc(buf, len)));

            end = ptr + (end - buf);
            buf = ptr;
            continue;
        }

        ssize_t rdlen;
        ERROR_IF(
            (rdlen = readFd(aFd, end, avail, 0),
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
             err && EINTR != errno));
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
        .l_pid    = getpid(),
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
struct LockType
ownFdRegionLocked(int aFd, off_t aPos, off_t aLen)
{
    int rc = -1;

    struct LockType lockType = LockTypeUnlocked;

    struct flock lockRegion =
    {
        .l_type   = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start  = aPos,
        .l_len    = aLen,
    };

    ERROR_IF(
        fcntl(aFd, F_GETLK, &lockRegion));

    switch (lockRegion.l_type)
    {
    default:
        ERROR_IF(
            true,
            {
                errno = EIO;
            });

    case F_UNLCK:
        break;

    case F_RDLCK:
        if (lockRegion.l_pid != ownProcessId().mPid)
            lockType = LockTypeRead;
        break;

    case F_WRLCK:
        if (lockRegion.l_pid != ownProcessId().mPid)
            lockType = LockTypeWrite;
        break;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc ? LockTypeError : lockType;
}

/* -------------------------------------------------------------------------- */
