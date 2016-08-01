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
#ifndef SENTRY_H
#define SENTRY_H

#include "childprocess.h"
#include "umbilical.h"
#include "pidserver.h"

#include "compiler_.h"
#include "stdfdfiller_.h"
#include "socketpair_.h"
#include "jobcontrol_.h"
#include "bellsocketpair_.h"
#include "pidfile_.h"

BEGIN_C_SCOPE;

struct ExitCode;
struct Pipe;

/* -------------------------------------------------------------------------- */
struct Sentry
{
    struct StdFdFiller  mStdFdFiller_;
    struct StdFdFiller *mStdFdFiller;

    struct SocketPair  mUmbilicalSocket_;
    struct SocketPair *mUmbilicalSocket;

    struct ChildProcess  mChildProcess_;
    struct ChildProcess *mChildProcess;

    struct JobControl  mJobControl_;
    struct JobControl *mJobControl;

    struct BellSocketPair  mSyncSocket_;
    struct BellSocketPair *mSyncSocket;

    struct PidFile  mPidFile_;
    struct PidFile *mPidFile;

    struct PidServer  mPidServer_;
    struct PidServer *mPidServer;

    struct UmbilicalProcess  mUmbilicalProcess_;
    struct UmbilicalProcess *mUmbilicalProcess;
};

/* -------------------------------------------------------------------------- */
CHECKED int
createSentry(struct Sentry      *self,
             const char * const *aCmd);

struct Sentry *
closeSentry(struct Sentry *self);

struct Pid
announceSentryPidFile(struct Sentry *self);

CHECKED int
runSentry(struct Sentry   *self,
          struct Pid       aParentPid,
          struct Pipe     *aParentPipe,
          struct ExitCode *aExitCode);

const char *
ownSentryPidFileName(const struct Sentry *self);

/* -------------------------------------------------------------------------- */

END_C_SCOPE;

#endif /* SENTRY_H */
