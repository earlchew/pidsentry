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

#include "int_.h"
#include "fd_.h"

#include <stdbool.h>

#include <sys/queue.h>
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
    int              mFd;
    LIST_ENTRY(File) mList;
};

/* -------------------------------------------------------------------------- */
INT
temporaryFile(struct File *self);

INT
createFile(struct File *self, int aFd);

INT
detachFile(struct File *self);

struct File *
closeFile(struct File *self);

void
walkFileList(void *aOther,
             int (*aVisitor)(void *aOther, const struct File *aFile));

INT
dupFile(struct File *self, const struct File *aOther);

INT
nonBlockingFile(struct File *self);

INT
ownFileNonBlocking(const struct File *self);

INT
closeFileOnExec(struct File *self, unsigned aCloseOnExec);

INT
ownFileCloseOnExec(const struct File *self);

ssize_t
writeFile(struct File *self, const char *aBuf, size_t aLen);

ssize_t
readFile(struct File *self, char *aBuf, size_t aLen);

off_t
lseekFile(struct File *self, off_t aOffset, struct WhenceType aWhenceType);

INT
fstatFile(struct File *self, struct stat *aStat);

INT
fcntlFileGetFlags(struct File *self);

INT
ftruncateFile(struct File *self, off_t aLength);

bool
ownFileValid(const struct File *self);

/* -------------------------------------------------------------------------- */
INT
waitFileWriteReady(const struct File     *self,
                   const struct Duration *aTimeout);

INT
waitFileReadReady(const struct File     *self,
                  const struct Duration *aTimeout);

/* -------------------------------------------------------------------------- */
INT
bindFileSocket(struct File *self, struct sockaddr *aAddr, size_t aAddrLen);

INT
connectFileSocket(struct File *self, struct sockaddr *aAddr, size_t aAddrLen);

INT
acceptFileSocket(struct File *self, unsigned aFlags);

INT
listenFileSocket(struct File *self, unsigned aQueueLen);

ssize_t
sendFileSocket(struct File *self, const char *aBuf, size_t aLen);

ssize_t
recvFileSocket(struct File *self, char *aBuf, size_t aLen);

ssize_t
sendFileSocketMsg(struct File *self, const struct msghdr *aMsg, int aFlags);

ssize_t
recvFileSocketMsg(struct File *self, struct msghdr *aMsg, int aFlags);

INT
shutdownFileSocketReader(struct File *self);

INT
shutdownFileSocketWriter(struct File *self);

INT
ownFileSocketName(const struct File *self,
                  struct sockaddr *aAddr, socklen_t *aAddrLen);

INT
ownFileSocketPeerName(const struct File *self,
                      struct sockaddr *aAddr, socklen_t *aAddrLen);

INT
ownFileSocketError(const struct File *self, int *aError);

INT
ownFileSocketPeerCred(const struct File *self, struct ucred *aCred);

/* -------------------------------------------------------------------------- */
INT
lockFile(struct File *self, struct LockType aLockType);

INT
unlockFile(struct File *self);

/* -------------------------------------------------------------------------- */
INT
lockFileRegion(
    struct File *self, struct LockType aLockType, off_t aPos, off_t aLen);

INT
unlockFileRegion(struct File *self, off_t aPos, off_t aLen);

struct LockType
ownFileRegionLocked(const struct File *self, off_t aPos, off_t aLen);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* FILE_H */
