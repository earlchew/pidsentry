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

#include "file_.h"
#include "macros_.h"
#include "error_.h"
#include "fd_.h"
#include "process_.h"
#include "test_.h"

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <valgrind/valgrind.h>

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
        {
            if (closeFd(&self->mFd))
                terminate(
                    errno, "Unable to close file descriptor %d", self->mFd);
        }
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

        if (closeFd(&self->mFd))
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
bool
ownFileValid(const struct File *self)
{
    return self && -1 != self->mFd;
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
        {
            if (closeFile(self))
                terminate(
                    errno, "Unable to close file descriptor %d", self->mFd);
        }
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
lockFile(struct File *self, int aType)
{
    return lockFd(self->mFd, aType);
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
acceptFileSocket(struct File *self, unsigned aFlags)
{
    int rc    = -1;
    int flags = 0;

    struct ProcessAppLock *applock = 0;

    switch (aFlags)
    {
    default:
        errno = EINVAL;
        goto Finally;

    case 0:                                                            break;
    case O_NONBLOCK | O_CLOEXEC: flags = SOCK_NONBLOCK | SOCK_CLOEXEC; break;
    case O_NONBLOCK:             flags = SOCK_NONBLOCK;                break;
    case O_CLOEXEC:              flags = SOCK_CLOEXEC;                 break;
    }

    if ( ! RUNNING_ON_VALGRIND)
        rc = accept4(self->mFd, 0, 0, flags);
    else
    {
        applock = createProcessAppLock();

        rc = accept(self->mFd, 0, 0);
        if (-1 == rc)
            goto Finally;

        if (aFlags & O_NONBLOCK)
        {
            if (nonblockingFd(rc))
                goto Finally;
        }

        if (aFlags & O_CLOEXEC)
        {
            if (closeFdOnExec(rc, O_CLOEXEC))
                goto Finally;
        }
    }

Finally:

    FINALLY
    ({
        destroyProcessAppLock(applock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
listenFileSocket(struct File *self, unsigned aQueueLen)
{
    return listen(self->mFd, aQueueLen ? aQueueLen : 1);
}

/* -------------------------------------------------------------------------- */
int
waitFileWriteReady(const struct File     *self,
                   const struct Duration *aTimeout)
{
    return waitFdWriteReady(self->mFd, aTimeout);
}

/* -------------------------------------------------------------------------- */
int
waitFileReadReady(const struct File     *self,
                  const struct Duration *aTimeout)
{
    return waitFdReadReady(self->mFd, aTimeout);
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
