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

#include "unixsocket_.h"
#include "macros_.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <sys/un.h>

/* -------------------------------------------------------------------------- */
static void
createRandomName(struct sockaddr_un *aSockAddr)
{
    static const char hexDigits[] = "0123456789abcdef";

    char *bp = aSockAddr->sun_path;
    char *ep = bp + sizeof(aSockAddr->sun_path);

    for (unsigned ix = 0; sizeof(aSockAddr->sun_path) / 4 > ix; ++ix)
    {
        uint32_t rnd = random();

        bp[0] = hexDigits[rnd % sizeof(hexDigits)]; rnd >>= 8;
        bp[1] = hexDigits[rnd % sizeof(hexDigits)]; rnd >>= 8;
        bp[2] = hexDigits[rnd % sizeof(hexDigits)]; rnd >>= 8;
        bp[3] = hexDigits[rnd % sizeof(hexDigits)]; rnd >>= 8;
    }

    uint32_t rnd = random();

    while (bp < ep)
    {
        *bp++ = hexDigits[rnd % sizeof(hexDigits)]; rnd >>= 8;
    }
}

int
createUnixSocket(struct UnixSocket *self,
                 const char *aName,
                 size_t aNameLen)
{
    int rc = -1;

    self->mFile = 0;

    if (createFile(&self->mFile_,
                   socket(AF_UNIX, SOCK_STREAM, 0)))
        goto Finally;
    self->mFile = &self->mFile_;

    struct sockaddr_un sockAddr;

    while (1)
    {
        createRandomName(&sockAddr);

        sockAddr.sun_family = AF_UNIX;
        sockAddr.sun_path[0] = 0;

        if (bindFileSocket(self->mFile,
                           (struct sockaddr *) &sockAddr, sizeof(sockAddr)))
        {
            if (EADDRINUSE == errno)
                continue;
            goto Finally;
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

    return rc;
}

/* -------------------------------------------------------------------------- */
int
acceptUnixSocket(struct UnixSocket *self,
                 const struct UnixSocket *aServer)
{
    int rc = -1;

    self->mFile = 0;

    if (createFile(&self->mFile_, acceptFileSocket(aServer->mFile)))
        goto Finally;
    self->mFile = &self->mFile_;

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
connectSocket(struct UnixSocket *self,
              const char *aName,
              size_t aNameLen)
{
    int rc = 0;

    self->mFile = 0;

    struct sockaddr_un sockAddr;

    if ( ! aNameLen)
        aNameLen = strlen(aName);
    if ( ! aNameLen)
    {
        errno = EINVAL;
        goto Finally;
    }
    if (sizeof(sockAddr.sun_path) < aNameLen)
        goto Finally;

    sockAddr.sun_family = AF_UNIX;
    memcpy(sockAddr.sun_path, aName, aNameLen);

    if (createFile(&self->mFile_,
                   socket(AF_UNIX, SOCK_STREAM, 0)))
        goto Finally;
    self->mFile = &self->mFile_;

    if (bindFileSocket(self->mFile,
                       (struct sockaddr *) &sockAddr, sizeof(sockAddr)))
        goto Finally;

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
closeUnixSocket(struct UnixSocket *self)
{
    int rc = -1;

    if (closeFile(self->mFile))
        goto Finally;

    self->mFile = 0;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
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
