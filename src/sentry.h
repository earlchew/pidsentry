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

#include "pidfile_.h"

#include "ert/compiler.h"
#include "ert/stdfdfiller.h"
#include "ert/socketpair.h"
#include "ert/jobcontrol.h"
#include "ert/bellsocketpair.h"

ERT_BEGIN_C_SCOPE;

struct Ert_ExitCode;
struct Ert_Pipe;

/* -------------------------------------------------------------------------- */
struct Sentry
{
    struct Ert_SocketPair  mUmbilicalSocket_;
    struct Ert_SocketPair *mUmbilicalSocket;

    struct ChildProcess  mChildProcess_;
    struct ChildProcess *mChildProcess;

    struct Ert_JobControl  mJobControl_;
    struct Ert_JobControl *mJobControl;

    struct Ert_BellSocketPair  mSyncSocket_;
    struct Ert_BellSocketPair *mSyncSocket;

    struct PidFile  mPidFile_;
    struct PidFile *mPidFile;

    struct PidServer  mPidServer_;
    struct PidServer *mPidServer;

    struct UmbilicalProcess  mUmbilicalProcess_;
    struct UmbilicalProcess *mUmbilicalProcess;
};

/* -------------------------------------------------------------------------- */
ERT_CHECKED int
createSentry(struct Sentry      *self,
             const char * const *aCmd);

struct Sentry *
closeSentry(struct Sentry *self);

struct Ert_Pid
announceSentryPidFile(struct Sentry *self);

ERT_CHECKED int
runSentry(struct Sentry       *self,
          struct Ert_Pid       aParentPid,
          struct Ert_Pipe     *aParentPipe,
          struct Ert_ExitCode *aExitCode);

const char *
ownSentryPidFileName(const struct Sentry *self);

/* -------------------------------------------------------------------------- */

ERT_END_C_SCOPE;

#endif /* SENTRY_H */
