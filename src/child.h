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
#ifndef CHILD_H
#define CHILD_H

#include "pipe_.h"
#include "thread_.h"
#include "eventlatch_.h"

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct StdFdFiller;
struct SocketPair;
struct ChildMonitor;
struct UmbilicalProcess;

/* -------------------------------------------------------------------------- */
struct ChildProcess
{
    pid_t mPid;
    pid_t mPgid;

    struct EventLatch mChildLatch;
    struct EventLatch mUmbilicalLatch;

    struct Pipe  mTetherPipe_;
    struct Pipe* mTetherPipe;

    struct
    {
        struct ThreadSigMutex mMutex;
        struct ChildMonitor  *mMonitor;
    } mChildMonitor;
};

/* -------------------------------------------------------------------------- */
void
createChild(struct ChildProcess *self);

void
reapChild(struct ChildProcess *self, pid_t aUmbilicalPid);

int
forkChild(
    struct ChildProcess  *self,
    char                **aCmd,
    struct StdFdFiller   *aStdFdFiller,
    struct SocketPair    *aSyncSocket,
    struct SocketPair    *aUmbilicalSocket);

void
killChild(struct ChildProcess *self, int aSigNum);

void
closeChildTether(struct ChildProcess *self);

void
monitorChildUmbilical(struct ChildProcess *self, pid_t aParentPid);

void
monitorChild(struct ChildProcess     *self,
             struct UmbilicalProcess *aUmbilicalProcess,
             struct File             *aUmbilicalFile);

void
raiseChildSigCont(struct ChildProcess *self);

int
closeChild(struct ChildProcess *self);

/* -------------------------------------------------------------------------- */
void
killChildProcessGroup(struct ChildProcess *self);

void
pauseChildProcessGroup(struct ChildProcess *self);

void
resumeChildProcessGroup(struct ChildProcess *self);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* CHILD_H */
