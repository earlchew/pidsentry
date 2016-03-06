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

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>

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
        aNameLen ? aNameLen : getpid() ^ MSECS(monotonicTime().monotonic).ms;

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
