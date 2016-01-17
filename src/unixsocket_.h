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
#ifndef UNIXSOCKET_H
#define UNIXSOCKET_H

#include "file_.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Duration;

struct sockaddr_un;
struct ucred;

struct UnixSocket
{
    struct File  mFile_;
    struct File *mFile;
};

/* -------------------------------------------------------------------------- */
int
createUnixSocket(struct UnixSocket *self,
                 const char        *aName,
                 size_t             aNameLen,
                 unsigned           aQueueLen);

int
acceptUnixSocket(struct UnixSocket       *self,
                 const struct UnixSocket *aServer,
                 unsigned                 aFlags);

int
connectUnixSocket(struct UnixSocket *self,
                 const char         *aName,
                 size_t              aNameLen);

int
closeUnixSocket(struct UnixSocket *self);

bool
ownUnixSocketValid(const struct UnixSocket *self);

int
shutdownUnixSocketReader(struct UnixSocket *self);

int
shutdownUnixSocketWriter(struct UnixSocket *self);

int
waitUnixSocketWriteReady(const struct UnixSocket *self,
                         const struct Duration   *aTimeout);

int
waitUnixSocketReadReady(const struct UnixSocket *self,
                        const struct Duration   *aTimeout);

int
ownUnixSocketPeerName(const struct UnixSocket *self,
                      struct sockaddr_un      *aAddr);

int
ownUnixSocketName(const struct UnixSocket *self,
                  struct sockaddr_un      *aAddr);

int
ownUnixSocketError(const struct UnixSocket *self, int *aError);

int
ownUnixSocketPeerCred(const struct UnixSocket *self, struct ucred *aCred);

ssize_t
sendUnixSocket(struct UnixSocket *self, const char *aBuf, size_t aLen);

ssize_t
recvUnixSocket(struct UnixSocket *self, char *aBuf, size_t aLen);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* UNIXSOCKET_H */
