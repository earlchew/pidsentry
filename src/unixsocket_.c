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

#include "unixsocket_.h"
#include "timekeeping_.h"
#include "error_.h"
#include "macros_.h"
#include "fd_.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/file.h>
#include <sys/un.h>

/* -------------------------------------------------------------------------- */
static void
createRandomName(struct sockaddr_un *aSockAddr, uint32_t *aRandom)
{
    static const char hexDigits[16] = "0123456789abcdef";

    char *bp = aSockAddr->sun_path;
    char *ep = bp + sizeof(aSockAddr->sun_path);

    typedef char check[sizeof(aSockAddr->sun_path) > 40 ? 1 : 0];

    *bp++ = 0;

    for (unsigned ix = 0; 10 > ix; ++ix)
    {
        /* LCG(2^32, 69069, 0, 1)
         * http://mathforum.org/kb/message.jspa?messageID=1608043 */

        *aRandom = *aRandom * 69069 + 1;

        uint32_t rnd = *aRandom;

        bp[0] = hexDigits[rnd % sizeof(hexDigits)]; rnd >>= 8;
        bp[1] = hexDigits[rnd % sizeof(hexDigits)]; rnd >>= 8;
        bp[2] = hexDigits[rnd % sizeof(hexDigits)]; rnd >>= 8;
        bp[3] = hexDigits[rnd % sizeof(hexDigits)]; rnd >>= 8;

        bp += 4;
    }

    memset(bp, 0, ep-bp);
}

int
createUnixSocket(
    struct UnixSocket *self,
    const char        *aName,
    size_t             aNameLen,
    unsigned           aQueueLen)
{
    int rc = -1;

    self->mFile = 0;

    ERROR_IF(
        createFile(&self->mFile_,
                   socket(AF_UNIX,
                          SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)));
    self->mFile = &self->mFile_;

    /* Do not use random() from stdlib to avoid perturbing the behaviour of
     * programs that themselves use the PRNG from the library. */

    uint32_t rnd =
        aNameLen
        ? aNameLen
        : ownProcessId().mPid ^ MSECS(monotonicTime().monotonic).ms;

    while (1)
    {
        struct sockaddr_un sockAddr;

        if ( ! aName)
        {
            createRandomName(&sockAddr, &rnd);
            sockAddr.sun_family = AF_UNIX;
            sockAddr.sun_path[0] = 0;
        }
        else
        {
            sockAddr.sun_family = AF_UNIX;
            memset(sockAddr.sun_path, 0, sizeof(sockAddr.sun_path));

            if ( ! aNameLen)
                strncpy(sockAddr.sun_path, aName, sizeof(sockAddr.sun_path));
            else
            {
                ERROR_IF(
                    sizeof(sockAddr.sun_path) < aNameLen,
                    {
                        errno = EINVAL;
                    });
                memcpy(sockAddr.sun_path, aName, aNameLen);
            }
        }

        /* Only perform an automatic retry if requested. This is
         * primarily to allow the unit test to verify correct
         * operation of the retry and name generation code. */

        int err;
        ERROR_IF(
            (err = bindFileSocket(
                self->mFile,
                (struct sockaddr *) &sockAddr, sizeof(sockAddr)),
             err && (EADDRINUSE != errno || aName || aNameLen)));

        if (err)
            continue;

        ERROR_IF(
            listenFileSocket(self->mFile, aQueueLen));

        break;
    }

    rc = 0;

Finally:

    FINALLY(
    {
        if (rc)
            closeFile(self->mFile);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
acceptUnixSocket(
    struct UnixSocket *self, const struct UnixSocket *aServer)
{
    int rc = -1;

    self->mFile = 0;

    while (1)
    {
        int err;
        ERROR_IF(
            (err = createFile(
                &self->mFile_,
                acceptFileSocket(aServer->mFile, O_NONBLOCK | O_CLOEXEC)),
             err && EINTR != errno));

        if ( ! err)
            break;
    }
    self->mFile = &self->mFile_;

    rc = 0;

Finally:

    FINALLY(
    {
        if (rc)
            closeFile(self->mFile);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
connectUnixSocket(struct UnixSocket *self, const char *aName, size_t aNameLen)
{
    int rc = -1;
    int err = 0;

    self->mFile = 0;

    struct sockaddr_un sockAddr;

    if ( ! aNameLen)
        aNameLen = strlen(aName);
    ERROR_UNLESS(
        aNameLen,
        {
            errno = EINVAL;
        });
    ERROR_IF(
        sizeof(sockAddr.sun_path) < aNameLen);

    sockAddr.sun_family = AF_UNIX;
    memcpy(sockAddr.sun_path, aName, aNameLen);
    memset(
        sockAddr.sun_path + aNameLen, 0, sizeof(sockAddr.sun_path) - aNameLen);

    ERROR_IF(
        createFile(
            &self->mFile_,
            socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)));
    self->mFile = &self->mFile_;

    while (1)
    {
        ERROR_IF(
            (err = connectFileSocket(
                self->mFile,
                (struct sockaddr *) &sockAddr, sizeof(sockAddr)),
             err && EINTR != errno && EINPROGRESS != errno));

        if (err)
        {
            if (EINTR == errno)
                continue;

            err = EINPROGRESS;
        }

        break;
    }

    rc = 0;

Finally:

    FINALLY(
    {
        if (rc)
            closeFile(self->mFile);
    });

    return rc ? rc : err ? err : 0;
}

/* -------------------------------------------------------------------------- */
void
closeUnixSocket(struct UnixSocket *self)
{
    if (self)
    {
        closeFile(self->mFile);
        self->mFile = 0;
    }
}

/* -------------------------------------------------------------------------- */
int
sendUnixSocketFd(struct UnixSocket *self, int aFd)
{
    int rc = -1;

    struct msghdr msg;

    char cmsgbuf[CMSG_SPACE(sizeof aFd)];

    char buf[1] = { 0 };

    struct iovec iov[1];

    iov[0].iov_base = buf;
    iov[0].iov_len  = sizeof(buf);

    msg.msg_name       = 0;
    msg.msg_namelen    = 0;
    msg.msg_iov        = iov;
    msg.msg_iovlen     = NUMBEROF(iov);
    msg.msg_flags      = 0;
    msg.msg_control    = cmsgbuf;
    msg.msg_controllen = CMSG_LEN(sizeof(aFd));

    memset(cmsgbuf, 0, sizeof(cmsgbuf));
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(aFd));

    ensure(msg.msg_controllen >= cmsg->cmsg_len);

    int *bufPtr = (void *) CMSG_DATA(cmsg);

    bufPtr[0] = aFd;

    ERROR_IF(
        sizeof(buf) != sendFileSocketMsg(self->mFile, &msg, 0));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
recvUnixSocketFd_(struct UnixSocket *self, struct msghdr *aMsg)
{
    int rc = -1;

    int    *fdPtr = 0;
    size_t  fdLen = 0;

    int fd = -1;

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(aMsg);
         cmsg;
         cmsg = CMSG_NXTHDR(aMsg, cmsg))
    {
        if (SOL_SOCKET == cmsg->cmsg_level &&
            SCM_RIGHTS == cmsg->cmsg_type &&
            CMSG_LEN(sizeof(fd)) == cmsg->cmsg_len)
        {
            ++fdLen;
        }
    }

    int fdBuf_[fdLen ? fdLen : 1];

    if (fdLen)
    {
        size_t ix;

        for (ix = 0; fdLen > ix; ++ix)
            fdBuf_[ix] = -1;
        fdPtr = fdBuf_;

        ix = 0;
        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(aMsg);
             cmsg;
             cmsg = CMSG_NXTHDR(aMsg, cmsg))
        {
            if (SOL_SOCKET == cmsg->cmsg_level &&
                SCM_RIGHTS == cmsg->cmsg_type &&
                CMSG_LEN(sizeof(fd)) == cmsg->cmsg_len)
            {
                const int *bufPtr = (const void *) CMSG_DATA(cmsg);

                fdPtr[ix++] = *bufPtr;
            }
        }

        ensure(fdLen == ix);
    }

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(aMsg);
         cmsg;
         cmsg = CMSG_NXTHDR(aMsg, cmsg))
    {
        ERROR_IF(
            cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type  != SCM_RIGHTS ||
            CMSG_LEN(sizeof(fd)) != cmsg->cmsg_len);
    }

    ERROR_UNLESS(
        1 == fdLen && 0 <= fdPtr[0]);

    fd       = fdPtr[0];
    fdPtr[0] = -1;

    rc = fd;

Finally:

    FINALLY
    ({
        if (-1 != rc)
        {
            if (fdPtr)
            {
                for (size_t ix = 0; fdLen > ix; ++ix)
                    closeFd(&fdPtr[ix]);
            }
        }
    });

    return rc;
}

int
recvUnixSocketFd(struct UnixSocket *self, unsigned aFlags)
{
    int rc = -1;
    int fd = -1;

    char cmsgbuf[CMSG_SPACE(sizeof(fd))];

    ERROR_IF(
        aFlags & ~ O_CLOEXEC);

    unsigned flags = 0;

    if (aFlags & O_CLOEXEC)
        flags |= MSG_CMSG_CLOEXEC;

    char buf[1];

    struct iovec iov[1];

    iov[0].iov_base = buf;
    iov[0].iov_len  = sizeof(buf);

    struct msghdr msg;

    msg.msg_name    = 0;
    msg.msg_namelen = 0;
    msg.msg_iov     = iov;
    msg.msg_iovlen  = NUMBEROF(iov);
    msg.msg_flags   = 0;

    msg.msg_control    = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    ssize_t rdlen;
    ERROR_IF(
        (rdlen = recvFileSocketMsg(self->mFile, &msg, flags),
         -1 == rdlen || (errno = EIO, sizeof(buf) != rdlen) || buf[0]));

    ERROR_UNLESS(
        msg.msg_controllen);

    ERROR_IF(
        (fd = recvUnixSocketFd_(self, &msg),
         -1 == fd));

    ERROR_IF(
        MSG_CTRUNC & msg.msg_flags,
        {
            errno = EIO;
        });

    rc = fd;
    fd = -1;

Finally:

    FINALLY
    ({
        closeFd(&fd);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
bool
ownUnixSocketValid(const struct UnixSocket *self)
{
    return ownFileValid(self->mFile);
}

/* -------------------------------------------------------------------------- */
int
shutdownUnixSocketReader(struct UnixSocket *self)
{
    return shutdownFileSocketReader(self->mFile);
}

/* -------------------------------------------------------------------------- */
int
shutdownUnixSocketWriter(struct UnixSocket *self)
{
    return shutdownFileSocketWriter(self->mFile);
}

/* -------------------------------------------------------------------------- */
int
waitUnixSocketWriteReady(const struct UnixSocket *self,
                         const struct Duration   *aTimeout)
{
    return waitFileWriteReady(self->mFile, aTimeout);
}

/* -------------------------------------------------------------------------- */
int
waitUnixSocketReadReady(const struct UnixSocket *self,
                        const struct Duration   *aTimeout)
{
    return waitFileReadReady(self->mFile, aTimeout);
}

/* -------------------------------------------------------------------------- */
ssize_t
sendUnixSocket(struct UnixSocket *self, const char *aBuf, size_t aLen)
{
    return sendFileSocket(self->mFile, aBuf, aLen);
}

/* -------------------------------------------------------------------------- */
ssize_t
recvUnixSocket(struct UnixSocket *self, char *aBuf, size_t aLen)
{
    return recvFileSocket(self->mFile, aBuf, aLen);
}

/* -------------------------------------------------------------------------- */
int
ownUnixSocketName(const struct UnixSocket *self,
                  struct sockaddr_un *aAddr)
{
    socklen_t addrLen = sizeof(*aAddr);

    return ownFileSocketName(self->mFile, (struct sockaddr *) aAddr, &addrLen);
}

/* -------------------------------------------------------------------------- */
int
ownUnixSocketPeerName(const struct UnixSocket *self,
                      struct sockaddr_un *aAddr)
{
    socklen_t addrLen = sizeof(*aAddr);

    return ownFileSocketPeerName(
        self->mFile, (struct sockaddr *) aAddr, &addrLen);
}

/* -------------------------------------------------------------------------- */
int
ownUnixSocketError(const struct UnixSocket *self, int *aError)
{
    return ownFileSocketError(self->mFile, aError);
}

/* -------------------------------------------------------------------------- */
int
ownUnixSocketPeerCred(const struct UnixSocket *self, struct ucred *aCred)
{
    return ownFileSocketPeerCred(self->mFile, aCred);
}

/* -------------------------------------------------------------------------- */
