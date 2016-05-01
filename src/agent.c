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
#include "parent.h"

#include "type_.h"
#include "error_.h"
#include "fd_.h"
#include "stdfdfiller_.h"

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>

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

    self->mCmd = aCmd;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            closeAgent(self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
printAgent_(const void *self_, FILE *aFile)
{
    const struct Agent *self = self_;

    return fprintf(aFile, "<agent %p %s>", self, self->mCmd[0]);
}

/* -------------------------------------------------------------------------- */
void
closeAgent(struct Agent *self)
{
    if (self)
        self->mType = 0;
}

/* -------------------------------------------------------------------------- */
static int
runAgentSentry_(struct Agent    *self,
                struct Pipe     *aParentPipe,
                struct ExitCode *aExitCode)
{
    int rc = -1;

    struct ExitCode exitCode = { EXIT_FAILURE };

    struct Sentry  sentry_;
    struct Sentry *sentry = 0;

    do
    {
        ERROR_IF(
            createSentry(&sentry_, self->mCmd));
        sentry = &sentry_;

        enum PidFileStatus status;
        ERROR_IF(
            (status = announceSentryPidFile(sentry),
             PidFileStatusError == status));

        if (PidFileStatusOk != status)
        {
            switch (status)
            {
            default:
                ensure(0);

            case PidFileStatusCollision:
                warn(0,
                     "Unable to write pidfile '%s'",
                     ownSentryPidFileName(sentry));
                break;
            }
            break;
        }

        ERROR_IF(
            runSentry(sentry, aParentPipe, &exitCode));

    } while (0);

    *aExitCode = exitCode;

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printAgent_);

        closeSentry(sentry);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
runAgent(struct Agent    *self,
         struct ExitCode *aExitCode)
{
    int rc = -1;

    struct ParentProcess  parentProcess_;
    struct ParentProcess *parentProcess = 0;

    struct StdFdFiller  stdFdFiller_;
    struct StdFdFiller *stdFdFiller = 0;

    struct Pipe  parentPipe_;
    struct Pipe *parentPipe = 0;

    if (gOptions.mOrphaned)
    {
        ERROR_IF(
            createParent(&parentProcess_));
        parentProcess = &parentProcess_;
    }

    {
        ERROR_IF(
            createStdFdFiller(&stdFdFiller_));
        stdFdFiller = &stdFdFiller_;

        ERROR_IF(
            createPipe(&parentPipe_, O_CLOEXEC | O_NONBLOCK));
        parentPipe = &parentPipe_;

        closeStdFdFiller(stdFdFiller);
        stdFdFiller = 0;
    }

    struct Pid agentPid;
    ERROR_IF(
        (agentPid = forkProcessChild(
            ForkProcessSetProcessGroup, Pgid(0)),
         -1 == agentPid.mPid));

    if ( ! agentPid.mPid)
    {
        debug(
            0,
            "running agent pid %" PRId_Pid " in pgid %" PRId_Pgid,
            FMTd_Pid(ownProcessId()), FMTd_Pgid(ownProcessGroupId()));

        closePipeWriter(parentPipe);

        struct ExitCode exitCode = { EXIT_FAILURE };

        ABORT_IF(
            runAgentSentry_(self, parentPipe, &exitCode));

        debug(0, "exit agent status %" PRId_ExitCode, FMTd_ExitCode(exitCode));

        exitProcess(exitCode.mStatus);
    }

    closePipeReader(parentPipe);

    {
        struct ProcessAppLock *appLock = createProcessAppLock();

        int whiteList[] =
        {
            STDIN_FILENO,
            STDOUT_FILENO,
            STDERR_FILENO,
            parentPipe->mWrFile->mFd,
            ownProcessAppLockFile(appLock)->mFd,
        };

        ERROR_IF(
            closeFdDescriptors(whiteList, NUMBEROF(whiteList)));

        destroyProcessAppLock(appLock);
    }

    int agentStatus;
    ERROR_IF(
        reapProcessChild(agentPid, &agentStatus));

    debug(0,
          "reaped agent pid %" PRId_Pid " status %d",
          FMTd_Pid(agentPid),
          agentStatus);

    *aExitCode =
        extractProcessExitStatus(agentStatus, agentPid);

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, self, printAgent_);

        closePipe(parentPipe);
        closeStdFdFiller(stdFdFiller);
        closeParent(parentProcess);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
