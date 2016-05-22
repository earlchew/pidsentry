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

#include "bellsocketpair_.h"
#include "error_.h"

/* -------------------------------------------------------------------------- */
int
createBellSocketPair(struct BellSocketPair *self, unsigned aFlags)
{
    int rc = -1;

    self->mSocketPair = 0;

    ERROR_IF(
        createSocketPair(&self->mSocketPair_, aFlags));
    self->mSocketPair = &self->mSocketPair_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            self->mSocketPair = closeSocketPair(self->mSocketPair);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closeBellSocketPairParent(struct BellSocketPair *self)
{
    closeSocketPairParent(self->mSocketPair);
}

/* -------------------------------------------------------------------------- */
void
closeBellSocketPairChild(struct BellSocketPair *self)
{
    closeSocketPairChild(self->mSocketPair);
}

/* -------------------------------------------------------------------------- */
struct BellSocketPair *
closeBellSocketPair(struct BellSocketPair *self)
{
    if (self)
        self->mSocketPair = closeSocketPair(self->mSocketPair);

    return 0;
}

/* -------------------------------------------------------------------------- */
static CHECKED int
ringBellSocketPair_(struct UnixSocket *aSocket)
{
    int rc = -1;

    char buf[1] = { 0 };

    int err;

    err = -1;
    ERROR_IF(
        (err = waitUnixSocketWriteReady(aSocket, 0),
         -1 == err || ! err),
        {
            if ( ! err)
                errno = EWOULDBLOCK;
        });

    err = -1;
    ERROR_IF(
        (err = sendUnixSocket(aSocket, buf, 1),
         1 != err),
        {
            if ( ! err)
                errno = ENOENT;
            else if (ECONNRESET == errno)
                errno = EPIPE;
        });

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
ringBellSocketPairParent(struct BellSocketPair *self)
{
    return ringBellSocketPair_(self->mSocketPair->mParentSocket);
}

int
ringBellSocketPairChild(struct BellSocketPair *self)
{
    return ringBellSocketPair_(self->mSocketPair->mChildSocket);
}

/* -------------------------------------------------------------------------- */
static CHECKED int
waitBellSocketPair_(struct UnixSocket     *aSocket,
                    const struct Duration *aTimeout)
{
    int rc = -1;

    int err;

    err = -1;
    ERROR_IF(
        (err = waitUnixSocketReadReady(aSocket, aTimeout),
         -1 == err || ! err),
        {
            if ( ! err)
                errno = EWOULDBLOCK;
        });

    char buf[1];

    err = -1;
    ERROR_IF(
        (err = recvUnixSocket(aSocket, buf, 1),
         1 != err),
        {
            if ( ! err)
                errno = ENOENT;
            else if (ECONNRESET == errno)
                errno = EPIPE;
        });

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
waitBellSocketPairParent(struct BellSocketPair *self,
                         const struct Duration *aTimeout)
{
    return waitBellSocketPair_(self->mSocketPair->mParentSocket, aTimeout);
}

int
waitBellSocketPairChild(struct BellSocketPair *self,
                        const struct Duration *aTimeout)
{
    return waitBellSocketPair_(self->mSocketPair->mChildSocket, aTimeout);
}

/* -------------------------------------------------------------------------- */
