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
#ifndef FD_H
#define FD_H


#include <stdbool.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct Duration;

/* -------------------------------------------------------------------------- */
struct LockType
{
    enum
    {
        LockTypeWrite_,
        LockTypeRead_,
    } mType;
};

#ifdef __cplusplus
#define LockTypeWrite_ LockType::LockTypeWrite_
#define LockTypeRead_  LockType::LockTypeRead_
#endif

#define LockTypeWrite ((struct LockType) { mType : LockTypeWrite_ })
#define LockTypeRead  ((struct LockType) { mType : LockTypeRead_ })

/* -------------------------------------------------------------------------- */
struct WhenceType
{
    enum
    {
        WhenceTypeStart_,
        WhenceTypeHere_,
        WhenceTypeEnd_,
    } mType;
};

#ifdef __cplusplus
#define WhenceTypeStart_ WhenceType::WhenceTypeStart_
#define WhenceTypeHere_  WhenceType::WhenceTypeHere_
#define WhenceTypeEnd_   WhenceType::WhenceTypeEnd_
#endif

#define WhenceTypeStart ((struct WhenceType) { mType : WhenceTypeStart_ })
#define WhenceTypeHere  ((struct WhenceType) { mType : WhenceTypeHere_ })
#define WhenceTypeEnd   ((struct WhenceType) { mType : WhenceTypeEnd_ })

/* -------------------------------------------------------------------------- */
void
closeFd(int *aFd);

int
closeFdDescriptors(const int *aWhiteList, size_t aWhiteListLen);

bool
stdFd(int aFd);

int
nullifyFd(int aFd);

int
nonBlockingFd(int aFd);

int
ownFdNonBlocking(int aFd);

int
ownFdValid(int aFd);

int
closeFdOnExec(int aFd, unsigned aCloseOnExec);

int
ownFdCloseOnExec(int aFd);

ssize_t
spliceFd(int aSrcFd, int aDstFd, size_t aLen, unsigned aFlags);

ssize_t
writeFd(int aFd, const char *aBuf, size_t aLen);

ssize_t
readFd(int aFd, char *aBuf, size_t aLen);

off_t
lseekFd(int aFd, off_t aOffset, struct WhenceType aWhenceType);

ssize_t
readFdFully(int aFd, char **aBuf, size_t aBufSize);

/* -------------------------------------------------------------------------- */
int
lockFd(int aFd, struct LockType aLockType);

int
unlockFd(int aFd);

/* -------------------------------------------------------------------------- */
int
lockFdRegion(int aFd, struct LockType aLockType, off_t aPos, off_t aLen);

int
unlockFdRegion(int aFd, off_t aPos, off_t aLen);

/* -------------------------------------------------------------------------- */
int
waitFdWriteReady(int aFd, const struct Duration *aTimeout);

int
waitFdReadReady(int aFd, const struct Duration *aTimeout);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* FD_H */
