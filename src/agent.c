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

#include "agent.h"

#include "type_.h"
#include "error_.h"

static const struct Type * const agentType_ = TYPE("Agent");

/* -------------------------------------------------------------------------- */
#if 0
static void
reapAgent_(void *self_)
{
    struct Agent *self = self_;

    ensure(agentType_ == self->mType);

    struct Pid umbilicalPid =
        self->mUmbilicalProcess ? self->mUmbilicalProcess->mPid : Pid(0);

    superviseChildProcess(self->mChildProcess, umbilicalPid);
}

static void
raiseAgentSignal_(void *self_, int aSigNum)
{
    struct Agent *self = self_;

    ensure(agentType_ == self->mType);

    /* Propagate the signal to the child. Note that SIGQUIT might cause
     * the child to terminate and dump core. Dump core in sympathy if this
     * happens, but do that only if the child actually does so. This is
     * taken care of in reapFamily_(). */

    killChild(self->mChildProcess, aSigNum);
}

static void
raiseAgentStop_(void *self_)
{
    struct Agent *self = self_;

    ensure(agentType_ == self->mType);

    pauseChildProcessGroup(self->mChildProcess);
}

static void
raiseAgentResume_(void *self_)
{
    struct Agent *self = self_;

    ensure(agentType_ == self->mType);

    resumeChildProcessGroup(self->mChildProcess);
}

static void
raiseAgentSigCont_(void *self_)
{
    struct Agent *self = self_;

    ensure(agentType_ == self->mType);

    raiseChildSigCont(self->mChildProcess);
}
#endif

/* -------------------------------------------------------------------------- */
int
createAgent(struct Agent  *self,
            char         **aCmd)
{
    int rc = -1;

    self->mType = agentType_;

    self->mSentry = 0;

    ERROR_IF(
        createSentry(&self->mSentry_, aCmd));
    self->mSentry = &self->mSentry_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            closeSentry(self->mSentry);

            self->mType = 0;
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closeAgent(struct Agent *self)
{
    if (self)
    {
        closeSentry(self->mSentry);

        self->mType = 0;
    }
}

/* -------------------------------------------------------------------------- */
int
runAgent(struct Agent    *self,
         struct ExitCode *aExitCode)
{
    int rc = -1;

    ERROR_IF(
        runSentry(self->mSentry, aExitCode));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
