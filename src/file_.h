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

#include "compiler_.h"
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
CHECKED int
temporaryFile(struct File *self);

CHECKED int
createFile(struct File *self, int aFd);

CHECKED int
detachFile(struct File *self);

CHECKED struct File *
closeFile(struct File *self);

void
walkFileList(void *aOther,
             int (*aVisitor)(void *aOther, const struct File *aFile));

CHECKED int
dupFile(struct File *self, const struct File *aOther);

CHECKED int
nonBlockingFile(struct File *self);

CHECKED int
ownFileNonBlocking(const struct File *self);

CHECKED int
closeFileOnExec(struct File *self, unsigned aCloseOnExec);

CHECKED int
ownFileCloseOnExec(const struct File *self);

CHECKED ssize_t
writeFile(struct File *self, const char *aBuf, size_t aLen);

CHECKED ssize_t
readFile(struct File *self, char *aBuf, size_t aLen);

CHECKED off_t
lseekFile(struct File *self, off_t aOffset, struct WhenceType aWhenceType);

CHECKED int
fstatFile(struct File *self, struct stat *aStat);

CHECKED int
fcntlFileGetFlags(struct File *self);

CHECKED int
ftruncateFile(struct File *self, off_t aLength);

bool
ownFileValid(const struct File *self);

/* -------------------------------------------------------------------------- */
CHECKED int
waitFileWriteReady(const struct File     *self,
                   const struct Duration *aTimeout);

CHECKED int
waitFileReadReady(const struct File     *self,
                  const struct Duration *aTimeout);

/* -------------------------------------------------------------------------- */
CHECKED int
bindFileSocket(struct File *self, struct sockaddr *aAddr, size_t aAddrLen);

CHECKED int
connectFileSocket(struct File *self, struct sockaddr *aAddr, size_t aAddrLen);

CHECKED int
acceptFileSocket(struct File *self, unsigned aFlags);

CHECKED int
listenFileSocket(struct File *self, unsigned aQueueLen);

CHECKED ssize_t
sendFileSocket(struct File *self, const char *aBuf, size_t aLen);

CHECKED ssize_t
recvFileSocket(struct File *self, char *aBuf, size_t aLen);

CHECKED ssize_t
sendFileSocketMsg(struct File *self, const struct msghdr *aMsg, int aFlags);

CHECKED ssize_t
recvFileSocketMsg(struct File *self, struct msghdr *aMsg, int aFlags);

CHECKED int
shutdownFileSocketReader(struct File *self);

CHECKED int
shutdownFileSocketWriter(struct File *self);

CHECKED int
ownFileSocketName(const struct File *self,
                  struct sockaddr *aAddr, socklen_t *aAddrLen);

CHECKED int
ownFileSocketPeerName(const struct File *self,
                      struct sockaddr *aAddr, socklen_t *aAddrLen);

CHECKED int
ownFileSocketError(const struct File *self, int *aError);

CHECKED int
ownFileSocketPeerCred(const struct File *self, struct ucred *aCred);

/* -------------------------------------------------------------------------- */
CHECKED int
lockFile(struct File *self, struct LockType aLockType);

CHECKED int
unlockFile(struct File *self);

/* -------------------------------------------------------------------------- */
CHECKED int
lockFileRegion(
    struct File *self, struct LockType aLockType, off_t aPos, off_t aLen);

CHECKED int
unlockFileRegion(struct File *self, off_t aPos, off_t aLen);

struct LockType
ownFileRegionLocked(const struct File *self, off_t aPos, off_t aLen);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* FILE_H */
