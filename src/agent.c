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
#include "sentry.h"
#include "parent.h"

#include "error_.h"
#include "fd_.h"
#include "stdfdfiller_.h"

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>

/* -------------------------------------------------------------------------- */
static void
raiseAgentSignal_(struct Agent *self, int aSigNum)
{
    struct Pid agentPid = self->mAgentPid;

    ensure(agentPid.mPid);

    ABORT_IF(
        kill(agentPid.mPid, aSigNum));
}

static void
raiseAgentStop_(struct Agent *self)
{
    struct Pid agentPid = self->mAgentPid;

    ensure(agentPid.mPid);

    ABORT_IF(
        kill(agentPid.mPid, SIGTSTP));
}

static void
raiseAgentResume_(struct Agent *self)
{
    struct Pid agentPid = self->mAgentPid;

    ensure(agentPid.mPid);

    ABORT_IF(
        kill(agentPid.mPid, SIGCONT));
}

/* -------------------------------------------------------------------------- */
int
createAgent(struct Agent  *self,
            char         **aCmd)
{
    int rc = -1;

    self->mCmd      = aCmd;
    self->mAgentPid = Pid(0);

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
printAgent_(const struct Agent *self, FILE *aFile)
{
    return fprintf(aFile, "<agent %p %s>", self, self->mCmd[0]);
}

/* -------------------------------------------------------------------------- */
void
closeAgent(struct Agent *self)
{
    if (self)
    { }
}

/* -------------------------------------------------------------------------- */
static int
runAgentSentry_(struct Agent    *self,
                struct Pid       aParentPid,
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
            runSentry(sentry, aParentPid, aParentPipe, &exitCode));

    } while (0);

    *aExitCode = exitCode;

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printAgent_, self);

        closeSentry(sentry);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct RunAgentProcess_
{
    struct Agent *mAgent;
    struct Pid    mParentPid;
    struct Pipe  *mParentPipe;
};

static int
runAgent_(struct RunAgentProcess_ *self, struct Pid aPid)
{
    int rc = -1;

    debug(
        0,
        "running agent pid %" PRId_Pid " in pgid %" PRId_Pgid,
        FMTd_Pid(ownProcessId()), FMTd_Pgid(ownProcessGroupId()));

    ensure(ownProcessId().mPid == ownProcessGroupId().mPgid);

    closePipeWriter(self->mParentPipe);

    struct ExitCode exitCode = { EXIT_FAILURE };

    ERROR_IF(
        runAgentSentry_(
            self->mAgent, self->mParentPid, self->mParentPipe, &exitCode));

    debug(0, "exit agent status %" PRId_ExitCode, FMTd_ExitCode(exitCode));

    rc = exitCode.mStatus;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
runAgentProcess_(struct Agent *self, struct ExitCode *aExitCode)
{
    int rc = -1;

    struct JobControl  jobControl_;
    struct JobControl *jobControl = 0;

    struct ParentProcess  parentProcess_;
    struct ParentProcess *parentProcess = 0;

    struct StdFdFiller  stdFdFiller_;
    struct StdFdFiller *stdFdFiller = 0;

    struct Pipe  parentPipe_;
    struct Pipe *parentPipe = 0;

    ERROR_IF(
        createJobControl(&jobControl_));
    jobControl = &jobControl_;

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

    {
        struct RunAgentProcess_ agentChild =
        {
            .mAgent      = self,
            .mParentPid  = ownProcessId(),
            .mParentPipe = parentPipe,
        };

        struct Pid agentPid;
        ERROR_IF(
            (agentPid = forkProcessChild(
                ForkProcessSetProcessGroup,
                Pgid(0),
                ForkProcessMethod(runAgent_, &agentChild)),
             -1 == agentPid.mPid));

        self->mAgentPid = agentPid;
    }

    closePipeReader(parentPipe);

    /* Be prepared to deliver signals to the agent process only after
     * the process exists. Before this point, these signals will cause
     * the watchdog process to terminate, and the new process will
     * notice via its synchronisation pipe. */

    ERROR_IF(
        watchJobControlSignals(jobControl,
                               VoidIntMethod(raiseAgentSignal_, self)));

    ERROR_IF(
        watchJobControlStop(jobControl,
                            VoidMethod(raiseAgentStop_, self),
                            VoidMethod(raiseAgentResume_, self)));

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

    {
        struct ChildProcessState agentState;
        ERROR_IF(
            (agentState = waitProcessChild(self->mAgentPid),
             ChildProcessStateError == agentState.mChildState));

        /* If the agent process has been killed by SIGQUIT and dumped core,
         * then dump core in sympathy. */

        if (ChildProcessStateDumped == agentState.mChildState &&
            SIGQUIT == agentState.mChildStatus)
        {
            quitProcess();
        }
    }

    ERROR_IF(
        unwatchJobControlStop(jobControl));

    ERROR_IF(
        unwatchJobControlSignals(jobControl));

    {
        /* Capture the pid of the agent process, then reset the data member
         * so that any signal races can be caught. */

        struct Pid agentPid = self->mAgentPid;

        self->mAgentPid = Pid(0);

        int agentStatus;
        ERROR_IF(
            reapProcessChild(agentPid, &agentStatus));

        debug(0,
              "reaped agent pid %" PRId_Pid " status %d",
              FMTd_Pid(agentPid),
              agentStatus);

        *aExitCode =
            extractProcessExitStatus(agentStatus, agentPid);
    }

    rc = 0;

Finally:

    FINALLY
    ({
        finally_warn_if(rc, printAgent_, self);

        closePipe(parentPipe);
        closeStdFdFiller(stdFdFiller);
        closeParent(parentProcess);
        closeJobControl(jobControl);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
runAgent(struct Agent    *self,
         struct ExitCode *aExitCode)
{
    int rc = -1;

    if (testAction(TestLevelRace) ||
        ownProcessId().mPid != ownProcessGroupId().mPgid)
    {
        ERROR_IF(
            runAgentProcess_(self, aExitCode));
    }
    else
    {
        ERROR_IF(
            runAgentSentry_(self, ownProcessId(), 0, aExitCode));
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
