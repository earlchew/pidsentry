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
#ifndef PIDFILE_H
#define PIDFILE_H

#include "int_.h"
#include "pathname_.h"
#include "pid_.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sockaddr_un;

struct PidFile
{
    struct PathName        mPathName_;
    struct PathName       *mPathName;
    struct File            mFile_;
    struct File           *mFile;
    const struct LockType *mLock;
};

enum PidFileStatus
{
    PidFileStatusError     = -1,
    PidFileStatusOk        = 0,
    PidFileStatusCollision = 1,
};

/* -------------------------------------------------------------------------- */
INT
initPidFile(struct PidFile *self, const char *aFileName);

void
destroyPidFile(struct PidFile *self);

/* -------------------------------------------------------------------------- */
INT
openPidFile(struct PidFile *self, unsigned aFlags);

struct PidFile *
closePidFile(struct PidFile *self);

struct Pid
readPidFile(const struct PidFile *self, struct sockaddr_un *aPidServerAddr);

INT
acquirePidFileWriteLock(struct PidFile *self);

INT
acquirePidFileReadLock(struct PidFile *self);

enum PidFileStatus
writePidFile(struct PidFile           *self,
             struct Pid                aPid,
             const struct sockaddr_un *aPidServerAddr);

const char *
ownPidFileName(const struct PidFile *self);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* PIDFILE_H */
