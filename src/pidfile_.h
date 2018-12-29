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

#include "ert/compiler.h"
#include "ert/pathname.h"
#include "ert/pid.h"
#include "ert/mode.h"

#include <stdio.h>

ERT_BEGIN_C_SCOPE;

struct sockaddr_un;

struct PidSignature;

struct PidFile
{
    struct Ert_PathName        mPathName_;
    struct Ert_PathName       *mPathName;
    struct Ert_File            mFile_;
    struct Ert_File           *mFile;
    const struct Ert_LockType *mLock;
};

/* -------------------------------------------------------------------------- */
ERT_CHECKED enum Ert_PathNameStatus
initPidFile(struct PidFile *self, const char *aFileName);

int
printPidFile(const struct PidFile *self, FILE *aFile);

ERT_CHECKED struct PidFile *
destroyPidFile(struct PidFile *self);

/* -------------------------------------------------------------------------- */
ERT_CHECKED struct Ert_Pid
openPidFile(struct PidFile *self, unsigned aFlags);

ERT_CHECKED int
closePidFile(struct PidFile *self);

ERT_CHECKED struct PidSignature *
readPidFile(const struct PidFile *self, struct sockaddr_un *aPidServerAddr);

ERT_CHECKED int
acquirePidFileWriteLock(struct PidFile *self);

ERT_CHECKED int
acquirePidFileReadLock(struct PidFile *self);

ERT_CHECKED struct Ert_Pid
createPidFile(struct PidFile           *self,
              struct Ert_Pid            aPid,
              const struct sockaddr_un *aPidServerAddr,
              struct Ert_Mode           aMode);

const char *
ownPidFileName(const struct PidFile *self);

/* -------------------------------------------------------------------------- */

ERT_END_C_SCOPE;

#endif /* PIDFILE_H */
