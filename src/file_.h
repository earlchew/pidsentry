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
#ifndef FILE_H
#define FILE_H

#include <stdbool.h>

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct stat;
struct sockaddr;
struct ucred;

struct Duration;

struct File
{
    int          mFd;
    struct File *mNext;
    struct File *mPrev;
};

/* -------------------------------------------------------------------------- */
int
createFile(struct File *self, int aFd);

int
detachFile(struct File *self);

int
closeFile(struct File *self);

void
walkFileList(void *aOther,
             int (*aVisitor)(void *aOther, const struct File *aFile));

int
dupFile(struct File *self, const struct File *aOther);

int
closeFilePair(struct File **aFile1,
                        struct File **aFile2);

int
nonblockingFile(struct File *self);

int
closeFileOnExec(struct File *self, unsigned aCloseOnExec);

ssize_t
writeFile(struct File *self, const char *aBuf, size_t aLen);

ssize_t
readFile(struct File *self, char *aBuf, size_t aLen);

int
fstatFile(struct File *self, struct stat *aStat);

int
fcntlFileGetFlags(struct File *self);

int
ftruncateFile(struct File *self, off_t aLength);

bool
ownFileValid(const struct File *self);

/* -------------------------------------------------------------------------- */
int
waitFileWriteReady(const struct File     *self,
                   const struct Duration *aTimeout);

int
waitFileReadReady(const struct File     *self,
                  const struct Duration *aTimeout);

/* -------------------------------------------------------------------------- */
int
bindFileSocket(struct File *self, struct sockaddr *aAddr, size_t aAddrLen);

int
connectFileSocket(struct File *self, struct sockaddr *aAddr, size_t aAddrLen);

int
acceptFileSocket(struct File *self, unsigned aFlags);

int
listenFileSocket(struct File *self, unsigned aQueueLen);

ssize_t
sendFileSocket(struct File *self, const char *aBuf, size_t aLen);

ssize_t
recvFileSocket(struct File *self, char *aBuf, size_t aLen);

int
shutdownFileSocketReader(struct File *self);

int
shutdownFileSocketWriter(struct File *self);

int
ownFileSocketName(const struct File *self,
                  struct sockaddr *aAddr, socklen_t *aAddrLen);

int
ownFileSocketPeerName(const struct File *self,
                      struct sockaddr *aAddr, socklen_t *aAddrLen);

int
ownFileSocketError(const struct File *self, int *aError);

int
ownFileSocketPeerCred(const struct File *self, struct ucred *aCred);

/* -------------------------------------------------------------------------- */
int
lockFile(struct File *self, int aType);

int
unlockFile(struct File *self);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* FILE_H */
