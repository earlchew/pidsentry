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

#include "shellcommand.h"

#include "ert/compiler.h"
#include "ert/pid.h"
#include "ert/pipe.h"
#include "ert/thread.h"
#include "ert/eventlatch.h"

#include <stdio.h>
#include <sys/types.h>

ERT_BEGIN_C_SCOPE;

struct Ert_SocketPair;
struct Ert_BellSocketPair;

struct ChildMonitor;
struct UmbilicalProcess;

/* -------------------------------------------------------------------------- */
struct ChildProcess
{
    struct Ert_Pid  mPid;
    struct Ert_Pgid mPgid;

    struct ShellCommand  mShellCommand_;
    struct ShellCommand *mShellCommand;

    struct
    {
        struct Ert_EventLatch  mChild_;
        struct Ert_EventLatch *mChild;
        struct Ert_EventLatch  mUmbilical_;
        struct Ert_EventLatch *mUmbilical;
    } mLatch;

    struct Ert_Pipe  mTetherPipe_;
    struct Ert_Pipe *mTetherPipe;

    struct
    {
        struct Ert_ThreadSigMutex  mMutex_;
        struct Ert_ThreadSigMutex *mMutex;
        struct ChildMonitor       *mMonitor;
    } mChildMonitor;
};

/* -------------------------------------------------------------------------- */
ERT_CHECKED int
createChildProcess(struct ChildProcess *self);

int
printChildProcessMonitor(const struct ChildMonitor *self, FILE *aFile);

ERT_CHECKED int
superviseChildProcess(struct ChildProcess *self, struct Ert_Pid aUmbilicalPid);

ERT_CHECKED int
forkChildProcess(
    struct ChildProcess       *self,
    const char * const        *aCmd,
    struct Ert_BellSocketPair *aSyncSocket,
    struct Ert_SocketPair     *aUmbilicalSocket);

ERT_CHECKED int
killChildProcess(struct ChildProcess *self, int aSigNum);

ERT_CHECKED int
closeChildProcessTether(struct ChildProcess *self);

ERT_CHECKED int
monitorChildProcess(struct ChildProcess     *self,
                    struct UmbilicalProcess *aUmbilicalProcess,
                    struct Ert_File         *aUmbilicalFile,
                    struct Ert_Pid           aParentPid,
                    struct Ert_Pipe         *aParentPipe);

ERT_CHECKED int
raiseChildProcessSigCont(struct ChildProcess *self);

ERT_CHECKED int
reapChildProcess(struct ChildProcess *self, int *aStatus);

struct ChildProcess *
closeChildProcess(struct ChildProcess *self);

int
printChildProcess(const struct ChildProcess *self, FILE *aFile);

/* -------------------------------------------------------------------------- */
ERT_CHECKED int
killChildProcessGroup(struct ChildProcess *self);

ERT_CHECKED int
pauseChildProcessGroup(struct ChildProcess *self);

ERT_CHECKED int
resumeChildProcessGroup(struct ChildProcess *self);

/* -------------------------------------------------------------------------- */

ERT_END_C_SCOPE;

#endif /* CHILDPROCESS_H */
