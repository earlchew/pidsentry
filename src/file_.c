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

#include "file_.h"
#include "macros_.h"
#include "error_.h"
#include "fd_.h"
#include "timekeeping_.h"
#include "test_.h"

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/socket.h>

/* -------------------------------------------------------------------------- */
static struct File sFileList =
{
    .mFd   = -1,
    .mNext = &sFileList,
    .mPrev = &sFileList,
};

/* -------------------------------------------------------------------------- */
int
createFile(struct File *self, int aFd)
{
    ensure(self != &sFileList);

    int rc = -1;

    self->mFd = aFd;

    /* If the file descriptor is invalid, take care to have preserved
     * errno so that the caller can rely on interpreting errno to
     * discover why the file descriptor is invalid. */

    if (-1 == aFd)
        goto Finally;

    self->mNext =  sFileList.mNext;
    self->mPrev = &sFileList;

    self->mNext->mPrev = self;
    self->mPrev->mNext = self;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc && -1 != self->mFd)
            closeFd(&self->mFd);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
detachFile(struct File *self)
{
    ensure(self != &sFileList);

    int rc = -1;

    if (self)
    {
        if (-1 == self->mFd)
            goto Finally;

        self->mPrev->mNext = self->mNext;
        self->mNext->mPrev = self->mPrev;

        self->mPrev = 0;
        self->mNext = 0;
        self->mFd   = -1;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closeFile(struct File *self)
{
    ensure(self != &sFileList);

    int rc = -1;

    if (self)
    {
        if (-1 == self->mFd)
        {
            errno = EBADF;
            goto Finally;
        }

        if (close(self->mFd))
            goto Finally;

        self->mPrev->mNext = self->mNext;
        self->mNext->mPrev = self->mPrev;

        self->mPrev = 0;
        self->mNext = 0;
        self->mFd   = -1;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
void
walkFileList(void *aOther,
             int aVisitor(void *aOther, const struct File *aFile))
{
    const struct File *fdPtr = &sFileList;

    do
    {
        fdPtr = fdPtr->mNext;

        if (fdPtr == &sFileList)
            break;

    } while ( ! aVisitor(aOther, fdPtr));
}

/* -------------------------------------------------------------------------- */
int
dupFile(struct File *self, const struct File *aOther)
{
    int rc = -1;

    if (createFile(self, dup(aOther->mFd)))
        goto Finally;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            closeFile(self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closeFilePair(struct File **aFile1, struct File **aFile2)
{
    int rc = -1;

    if (*aFile1)
    {
        if (closeFile(*aFile1))
            goto Finally;
        *aFile1 = 0;
    }

    if (*aFile2)
    {
        if (closeFile(*aFile2))
            goto Finally;
        *aFile2 = 0;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
nonblockingFile(struct File *self)
{
    return nonblockingFd(self->mFd);
}

/* -------------------------------------------------------------------------- */
int
closeFileOnExec(struct File *self, unsigned aCloseOnExec)
{
    return closeFdOnExec(self->mFd, aCloseOnExec);
}

/* -------------------------------------------------------------------------- */
int
lockFile(struct File *self, int aType, const struct Duration *aTimeout)
{
    return lockFd(
        self->mFd,
        aType,
        aTimeout ? *aTimeout : Duration(NSECS(Seconds(30))));
}

/* -------------------------------------------------------------------------- */
int
unlockFile(struct File *self)
{
    return unlockFd(self->mFd);
}

/* -------------------------------------------------------------------------- */
ssize_t
writeFile(struct File *self, const char *aBuf, size_t aLen)
{
    return writeFd(self->mFd, aBuf, aLen);
}

/* -------------------------------------------------------------------------- */
ssize_t
readFile(struct File *self, char *aBuf, size_t aLen)
{
    return readFd(self->mFd, aBuf, aLen);
}

/* -------------------------------------------------------------------------- */
int
fstatFile(struct File *self, struct stat *aStat)
{
    return fstat(self->mFd, aStat);
}

/* -------------------------------------------------------------------------- */
int
fcntlFileGetFlags(struct File *self)
{
    return fcntl(self->mFd, F_GETFL);
}

/* -------------------------------------------------------------------------- */
int
ftruncateFile(struct File *self, off_t aLength)
{
    return ftruncate(self->mFd, aLength);
}

/* -------------------------------------------------------------------------- */
int
bindFileSocket(struct File *self, struct sockaddr *aAddr, size_t aAddrLen)
{
    return bind(self->mFd, aAddr, aAddrLen);
}

/* -------------------------------------------------------------------------- */
int
connectFileSocket(struct File *self, struct sockaddr *aAddr, size_t aAddrLen)
{
    return connect(self->mFd, aAddr, aAddrLen);
}

/* -------------------------------------------------------------------------- */
int
acceptFileSocket(struct File *self)
{
    return accept(self->mFd, 0, 0);
}

/* -------------------------------------------------------------------------- */
int
listenFileSocket(struct File *self, unsigned aQueueLen)
{
    return listen(self->mFd, aQueueLen ? aQueueLen : 1);
}

/* -------------------------------------------------------------------------- */
static int
waitFileReady_(const struct File     *self,
               unsigned               aPollMask,
               const struct Duration *aTimeout)
{
    int rc = -1;

    struct pollfd pollfd[1] =
    {
        {
            .fd      = self->mFd,
            .events  = aPollMask,
            .revents = 0,
        },
    };

    struct EventClockTime since = EVENTCLOCKTIME_INIT;
    struct Duration       remaining;

    const struct Duration timeout =
        aTimeout ? *aTimeout : Duration(NanoSeconds(0));

    while (1)
    {
        struct EventClockTime tm = eventclockTime();

        RACE
        ({
            /* In case the process is stopped after the time is
             * latched, check once more if the fds are ready
             * before checking the deadline. */

            int events = poll(pollfd, NUMBEROF(pollfd), 0);
            if (-1 == events)
                goto Finally;

            if (events)
                break;
        });

        int timeout_ms;

        if ( ! aTimeout)
            timeout_ms = -1;
        else
        {
            if (deadlineTimeExpired(&since, timeout, &remaining, &tm))
                break;

            uint64_t remaining_ms = MSECS(remaining.duration).ms;

            timeout_ms = remaining_ms;

            if (0 > timeout_ms || timeout_ms != remaining_ms)
                timeout_ms = INT_MAX;
        }

        switch (poll(pollfd, NUMBEROF(pollfd), timeout_ms))
        {
        case -1:
            if (EINTR == errno)
                continue;
            goto Finally;

        case 0:
            pollfd[0].revents = 0;
            continue;

        default:
            break;
        }

        break;
    }

    rc = !! (pollfd[0].revents & aPollMask);

Finally:

    FINALLY({});

    return rc;
}

int
waitFileWriteReady(const struct File     *self,
                   const struct Duration *aTimeout)
{
    return waitFileReady_(self, POLLOUT, aTimeout);
}

int
waitFileReadReady(const struct File     *self,
                  const struct Duration *aTimeout)
{
    return waitFileReady_(self, POLLPRI | POLLIN, aTimeout);
}

/* -------------------------------------------------------------------------- */
int
ownFileSocketError(const struct File *self, int *aError)
{
    int rc = -1;

    socklen_t len = sizeof(*aError);

    if (getsockopt(self->mFd, SOL_SOCKET, SO_ERROR, aError, &len))
        goto Finally;

    if (sizeof(*aError) != len)
    {
        errno = EINVAL;
        goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
ownFileSocketPeerCred(const struct File *self, struct ucred *aCred)
{
    int rc = -1;

    socklen_t len = sizeof(*aCred);

    if (getsockopt(self->mFd, SOL_SOCKET, SO_PEERCRED, aCred, &len))
        goto Finally;

    if (sizeof(*aCred) != len)
    {
        errno = EINVAL;
        goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
ownFileSocketName(const struct File *self,
                  struct sockaddr *aAddr, socklen_t *aAddrLen)
{
    return getsockname(self->mFd, aAddr, aAddrLen);
}

/* -------------------------------------------------------------------------- */
int
ownFileSocketPeerName(const struct File *self,
                      struct sockaddr *aAddr, socklen_t *aAddrLen)
{
    return getpeername(self->mFd, aAddr, aAddrLen);
}

/* -------------------------------------------------------------------------- */
ssize_t
sendFileSocket(struct File *self, const char *aBuf, size_t aLen)
{
    return send(self->mFd, aBuf, aLen, 0);
}

/* -------------------------------------------------------------------------- */
ssize_t
recvFileSocket(struct File *self, char *aBuf, size_t aLen)
{
    return recv(self->mFd, aBuf, aLen, 0);
}

/* -------------------------------------------------------------------------- */
int
shutdownFileSocketReader(struct File *self)
{
    return shutdown(self->mFd, SHUT_RD);
}

/* -------------------------------------------------------------------------- */
int
shutdownFileSocketWriter(struct File *self)
{
    return shutdown(self->mFd, SHUT_WR);
}

/* -------------------------------------------------------------------------- */
