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
#include "thread_.h"

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <valgrind/valgrind.h>

/* -------------------------------------------------------------------------- */
static struct
{
    LIST_HEAD(, File) mHead;
    pthread_mutex_t   mMutex;
}
fileList_ =
{
    .mHead  = LIST_HEAD_INITIALIZER(File),
    .mMutex = PTHREAD_MUTEX_INITIALIZER,
};

/* -------------------------------------------------------------------------- */
int
createFile(struct File *self, int aFd)
{
    int rc = -1;

    self->mFd = aFd;

    /* If the file descriptor is invalid, take care to have preserved
     * errno so that the caller can rely on interpreting errno to
     * discover why the file descriptor is invalid. */

    int err = errno;
    ERROR_IF(
        (-1 == aFd),
        {
            errno = err;
        });

    lockMutex(&fileList_.mMutex);
    {
        LIST_INSERT_HEAD(&fileList_.mHead, self, mList);
    }
    unlockMutex(&fileList_.mMutex);

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
    int rc = -1;

    if (self)
    {
        ERROR_IF(
            -1 == self->mFd);

        lockMutex(&fileList_.mMutex);
        {
            LIST_REMOVE(self, mList);
        }
        unlockMutex(&fileList_.mMutex);

        self->mFd = -1;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closeFile(struct File *self)
{
    if (self && -1 != self->mFd)
    {
        closeFd(&self->mFd);

        lockMutex(&fileList_.mMutex);
        {
            LIST_REMOVE(self, mList);
        }
        unlockMutex(&fileList_.mMutex);

        self->mFd = -1;
    }
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
    lockMutex(&fileList_.mMutex);
    {
        const struct File *filePtr;

        LIST_FOREACH(filePtr, &fileList_.mHead, mList)
        {
            if (aVisitor(aOther, filePtr))
                break;
        }
    }
    unlockMutex(&fileList_.mMutex);
}

/* -------------------------------------------------------------------------- */
int
dupFile(struct File *self_, const struct File *aOther)
{
    int rc = -1;

    struct File *self = 0;

    ERROR_IF(
        createFile(self_, dup(aOther->mFd)));
    self = self_;

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
void
closeFilePair(struct File **aFile1, struct File **aFile2)
{
    if (*aFile1)
    {
        closeFile(*aFile1);
        *aFile1 = 0;
    }

    if (*aFile2)
    {
        closeFile(*aFile2);
        *aFile2 = 0;
    }
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
    int fd    = -1;
    int flags = 0;

    struct ProcessAppLock *applock = 0;

    switch (aFlags)
    {
    default:
        ERROR_IF(
            true,
            {
                errno = EINVAL;
            });

    case 0:                                                            break;
    case O_NONBLOCK | O_CLOEXEC: flags = SOCK_NONBLOCK | SOCK_CLOEXEC; break;
    case O_NONBLOCK:             flags = SOCK_NONBLOCK;                break;
    case O_CLOEXEC:              flags = SOCK_CLOEXEC;                 break;
    }

    if ( ! RUNNING_ON_VALGRIND)
        ERROR_IF(
            (fd = accept4(self->mFd, 0, 0, flags),
             -1 == fd));
    else
    {
        applock = createProcessAppLock();

        ERROR_IF(
            (fd = accept(self->mFd, 0, 0),
             -1 == fd));

        if (flags & SOCK_CLOEXEC)
            ERROR_IF(
                closeFdOnExec(fd, O_CLOEXEC));

        if (flags & SOCK_NONBLOCK)
            ERROR_IF(
                nonblockingFd(fd));
    }

    rc = fd;

Finally:

    FINALLY
    ({
        if (-1 == rc)
            closeFd(&fd);
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

    ERROR_IF(
        getsockopt(self->mFd, SOL_SOCKET, SO_ERROR, aError, &len));

    ERROR_IF(
        sizeof(*aError) != len,
        {
            errno = EINVAL;
        });

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

    ERROR_IF(
        getsockopt(self->mFd, SOL_SOCKET, SO_PEERCRED, aCred, &len));

    ERROR_IF(
        sizeof(*aCred) != len,
        {
            errno = EINVAL;
        });

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
ssize_t
sendFileSocketMsg(struct File *self, const struct msghdr *aMsg, int aFlags)
{
    return sendmsg(self->mFd, aMsg, aFlags);
}

/* -------------------------------------------------------------------------- */
ssize_t
recvFileSocketMsg(struct File *self, struct msghdr *aMsg, int aFlags)
{
    return recvmsg(self->mFd, aMsg, aFlags);
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
