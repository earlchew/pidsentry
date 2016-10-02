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

#include "socket_.h"
#include "macros_.h"
#include "error_.h"
#include "process_.h"

#include <fcntl.h>

#include <valgrind/valgrind.h>

/* -------------------------------------------------------------------------- */
struct Socket *
closeSocket(struct Socket *self)
{
    if (self)
        self->mFile = closeFile(self->mFile);

    return 0;
}

/* -------------------------------------------------------------------------- */
int
createSocket(struct Socket *self, int aFd)
{
    int rc = -1;

    self->mFile = 0;

    ERROR_IF(
        createFile(&self->mFile_, aFd));
    self->mFile = &self->mFile_;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
bool
ownSocketValid(const struct Socket *self)
{
    return ownFileValid(self->mFile);
}

/* -------------------------------------------------------------------------- */
ssize_t
writeSocket(struct Socket *self,
            const char *aBuf, size_t aLen, const struct Duration *aTimeout)
{
    return writeFile(self->mFile, aBuf, aLen, aTimeout);
}

/* -------------------------------------------------------------------------- */
ssize_t
readSocket(struct Socket *self,
           char *aBuf, size_t aLen, const struct Duration *aTimeout)
{
    return readFile(self->mFile, aBuf, aLen, aTimeout);
}

/* -------------------------------------------------------------------------- */
ssize_t
writeSocketDeadline(struct Socket *self,
                    const char *aBuf, size_t aLen, struct Deadline *aDeadline)
{
    return writeFileDeadline(self->mFile, aBuf, aLen, aDeadline);
}

/* -------------------------------------------------------------------------- */
ssize_t
readSocketDeadline(struct Socket *self,
                   char *aBuf, size_t aLen, struct Deadline *aDeadline)
{
    return readFileDeadline(self->mFile, aBuf, aLen, aDeadline);
}

/* -------------------------------------------------------------------------- */
int
bindSocket(struct Socket *self, struct sockaddr *aAddr, size_t aAddrLen)
{
    return bind(self->mFile->mFd, aAddr, aAddrLen);
}

/* -------------------------------------------------------------------------- */
int
connectSocket(struct Socket *self, struct sockaddr *aAddr, size_t aAddrLen)
{
    int rc;

    do
        rc = connect(self->mFile->mFd, aAddr, aAddrLen);
    while (rc && EINTR == errno);

    return rc;
}

/* -------------------------------------------------------------------------- */
int
acceptSocket(struct Socket *self, unsigned aFlags)
{
    int rc    = -1;
    int fd    = -1;
    int flags = 0;

    struct ProcessAppLock *appLock = 0;

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

    while (1)
    {
        ERROR_IF(
            (fd = accept4(self->mFile->mFd, 0, 0, flags),
             -1 == fd && EINTR != errno));
        if (-1 != fd)
            break;
    }

    rc = fd;

Finally:

    FINALLY
    ({
        if (-1 == rc)
            fd = closeFd(fd);
        appLock = destroyProcessAppLock(appLock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
listenSocket(struct Socket *self, unsigned aQueueLen)
{
    return listen(self->mFile->mFd, aQueueLen ? aQueueLen : 1);
}

/* -------------------------------------------------------------------------- */
int
waitSocketWriteReady(const struct Socket   *self,
                     const struct Duration *aTimeout)
{
    return waitFileWriteReady(self->mFile, aTimeout);
}

/* -------------------------------------------------------------------------- */
int
waitSocketReadReady(const struct Socket   *self,
                    const struct Duration *aTimeout)
{
    return waitFileReadReady(self->mFile, aTimeout);
}

/* -------------------------------------------------------------------------- */
int
ownSocketError(const struct Socket *self, int *aError)
{
    int rc = -1;

    socklen_t len = sizeof(*aError);

    ERROR_IF(
        getsockopt(self->mFile->mFd, SOL_SOCKET, SO_ERROR, aError, &len));

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
ownSocketPeerCred(const struct Socket *self, struct ucred *aCred)
{
    int rc = -1;

    socklen_t len = sizeof(*aCred);

    ERROR_IF(
        getsockopt(self->mFile->mFd, SOL_SOCKET, SO_PEERCRED, aCred, &len));

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
ownSocketName(const struct Socket *self,
              struct sockaddr *aAddr, socklen_t *aAddrLen)
{
    return getsockname(self->mFile->mFd, aAddr, aAddrLen);
}

/* -------------------------------------------------------------------------- */
int
ownSocketPeerName(const struct Socket *self,
                  struct sockaddr *aAddr, socklen_t *aAddrLen)
{
    return getpeername(self->mFile->mFd, aAddr, aAddrLen);
}

/* -------------------------------------------------------------------------- */
ssize_t
sendSocket(struct Socket *self, const char *aBuf, size_t aLen)
{
    ssize_t rc;

    do
        rc = send(self->mFile->mFd, aBuf, aLen, 0);
    while (-1 == rc && EINTR == errno);

    return rc;
}

/* -------------------------------------------------------------------------- */
ssize_t
recvSocket(struct Socket *self, char *aBuf, size_t aLen)
{
    ssize_t rc;

    do
        rc = recv(self->mFile->mFd, aBuf, aLen, 0);
    while (-1 == rc && EINTR == errno);

    return rc;
}

/* -------------------------------------------------------------------------- */
ssize_t
sendSocketMsg(struct Socket *self, const struct msghdr *aMsg, int aFlags)
{
    ssize_t rc;

    do
        rc = sendmsg(self->mFile->mFd, aMsg, aFlags);
    while (-1 == rc && EINTR == errno);

    return rc;
}

/* -------------------------------------------------------------------------- */
ssize_t
recvSocketMsg(struct Socket *self, struct msghdr *aMsg, int aFlags)
{
    ssize_t rc;

    do
        rc = recvmsg(self->mFile->mFd, aMsg, aFlags);
    while (-1 == rc && EINTR == errno);

    return rc;
}

/* -------------------------------------------------------------------------- */
int
shutdownSocketReader(struct Socket *self)
{
    return shutdown(self->mFile->mFd, SHUT_RD);
}

/* -------------------------------------------------------------------------- */
int
shutdownSocketWriter(struct Socket *self)
{
    return shutdown(self->mFile->mFd, SHUT_WR);
}

/* -------------------------------------------------------------------------- */
