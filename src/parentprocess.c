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

#include "parentprocess.h"

#include "ert/timekeeping.h"

#include <stdlib.h>

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
monitorParent_(struct ParentProcess *self)
{
    int rc = -1;

    debug(
        0,
        "watching parent pid %" PRId_Ert_Pid,
        FMTd_Ert_Pid(self->mParentPid));

    while (1)
    {
        ert_monotonicSleep(Ert_Duration(ERT_NSECS(Ert_Seconds(3))));

        struct Ert_Pid parentPid = ert_ownProcessParentId();

        if (1 == parentPid.mPid)
        {
            /* If the parent has terminated, terminate the agent process
             * to trigger either the sentry, if it is running in a
             * separate process, or the umbilical to clean up the
             * child process. */

            if (self->mParentPid.mPid)
                warn(
                    0,
                    "Parent pid %" PRId_Ert_Pid " terminated",
                    FMTd_Ert_Pid(self->mParentPid));
            else
                warn(0, "Parent terminated");

            ert_exitProcess(EXIT_FAILURE);
        }
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
createParentProcess(struct ParentProcess *self)
{
    int rc = - 1;

    /* This process might already have been orphaned since it was created,
     * so its original parent might be lost. As a consequence, only treat
     * init(8) as an adoptive parent. */

    self->mParentPid = ert_ownProcessParentId();
    if (1 == self->mParentPid.mPid)
        self->mParentPid = Ert_Pid(0);

    self->mThread = ert_createThread(&self->mThread_, "parentmonitor", 0,
                                     Ert_ThreadMethod(self, monitorParent_));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
struct ParentProcess *
closeParentProcess(struct ParentProcess *self)
{
    if (self)
    {
        ert_cancelThread(self->mThread);

        ABORT_UNLESS(
            ert_joinThread(self->mThread) && ECANCELED == errno);

        self->mThread = ert_closeThread(self->mThread);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
