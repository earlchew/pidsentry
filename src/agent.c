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
#include "parentprocess.h"

#include "options_.h"

#include "ert/fdset.h"

#include <stdlib.h>
#include <fcntl.h>

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
raiseAgentSignal_(
    struct Agent *self, int aSigNum, struct Ert_Pid aPid, struct Ert_Uid aUid)
{
    struct Ert_Pid agentPid = self->mAgentPid;

    ert_ensure(agentPid.mPid);

    return kill(agentPid.mPid, aSigNum);
}

static ERT_CHECKED int
raiseAgentStop_(struct Agent *self)
{
    struct Ert_Pid agentPid = self->mAgentPid;

    ert_ensure(agentPid.mPid);

    return kill(agentPid.mPid, SIGTSTP);
}

static ERT_CHECKED int
raiseAgentResume_(struct Agent *self)
{
    struct Ert_Pid agentPid = self->mAgentPid;

    ert_ensure(agentPid.mPid);

    return kill(agentPid.mPid, SIGCONT);
}

/* -------------------------------------------------------------------------- */
int
createAgent(struct Agent       *self,
            const char * const *aCmd)
{
    int rc = -1;

    self->mCmd      = aCmd;
    self->mAgentPid = Ert_Pid(0);

    rc = 0;

Ert_Finally:

    ERT_FINALLY
    ({
        if (rc)
            closeAgent(self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
printAgent(const struct Agent *self, FILE *aFile)
{
    return fprintf(aFile, "<agent %p %s>", self, self->mCmd[0]);
}

/* -------------------------------------------------------------------------- */
struct Agent *
closeAgent(struct Agent *self)
{
    return 0;
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
runAgentSentry_(struct Agent        *self,
                struct Ert_Pid       aParentPid,
                struct Ert_Pipe     *aParentPipe,
                struct Ert_ExitCode *aExitCode)
{
    int rc = -1;

    struct Ert_ExitCode exitCode = { EXIT_FAILURE };

    struct Sentry  sentry_;
    struct Sentry *sentry = 0;

    do
    {
        ERT_ERROR_IF(
            createSentry(&sentry_, self->mCmd));
        sentry = &sentry_;

        struct Ert_Pid announcePid;
        ERT_ERROR_IF(
            (announcePid = announceSentryPidFile(sentry),
             -1 == announcePid.mPid));

        if (announcePid.mPid)
        {
            ert_warn(
                0,
                "Pidfile '%s' names active pid %" PRId_Ert_Pid,
                ownSentryPidFileName(sentry), FMTd_Ert_Pid(announcePid));

            break;
        }

        ERT_ERROR_IF(
            runSentry(sentry, aParentPid, aParentPipe, &exitCode));

    } while (0);

    *aExitCode = exitCode;

    rc = 0;

Ert_Finally:

    ERT_FINALLY
    ({
        ert_finally_warn_if(rc, self, printAgent);

        sentry = closeSentry(sentry);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct RunAgentProcess_
{
    struct Agent    *mAgent;
    struct Ert_Pid   mParentPid;
    struct Ert_Pipe  mParentPipe_;
    struct Ert_Pipe *mParentPipe;
};

static ERT_CHECKED struct RunAgentProcess_ *
closeAgentChildProcess_(struct RunAgentProcess_ *self)
{
    if (self)
    {
        self->mParentPipe = ert_closePipe(self->mParentPipe);
    }

    return 0;
}

static ERT_CHECKED int
createAgentChildProcess_(struct RunAgentProcess_ *self,
                         struct Agent            *aAgent)
{
    int rc = -1;

    self->mAgent      = aAgent;
    self->mParentPid  = ert_ownProcessId();
    self->mParentPipe = 0;

    rc = 0;

Ert_Finally:

    ERT_FINALLY({});

    return rc;
}

static ERT_CHECKED int
prepareAgentChildProcessFork_(struct RunAgentProcess_         *self,
                              const struct Ert_PreForkProcess *aPreFork)
{
    int rc = -1;

    ERT_ERROR_IF(
        ert_createPipe(&self->mParentPipe_, O_CLOEXEC | O_NONBLOCK));
    self->mParentPipe = &self->mParentPipe_;

    ERT_ERROR_IF(
        ert_fillFdSet(aPreFork->mWhitelistFds));

    ERT_ERROR_IF(
        ert_fillFdSet(aPreFork->mBlacklistFds));
    ERT_ERROR_IF(
        ert_removeFdSetFile(
            aPreFork->mBlacklistFds, self->mParentPipe->mWrFile));
    ERT_ERROR_IF(
        ert_removeFdSetFile(
            aPreFork->mBlacklistFds, self->mParentPipe->mRdFile));

    rc = 0;

Ert_Finally:

    ERT_FINALLY({});

    return rc;
}

static ERT_CHECKED int
runAgentChildProcess_(struct RunAgentProcess_ *self)
{
    int rc = -1;

    ert_debug(
        0,
        "running agent pid %" PRId_Ert_Pid " in pgid %" PRId_Ert_Pgid,
        FMTd_Ert_Pid(ert_ownProcessId()),
        FMTd_Ert_Pgid(ert_ownProcessGroupId()));

    ert_ensure(ert_ownProcessId().mPid == ert_ownProcessGroupId().mPgid);

    struct Ert_ExitCode exitCode = { EXIT_FAILURE };

    ERT_ERROR_IF(
        runAgentSentry_(
            self->mAgent, self->mParentPid, self->mParentPipe, &exitCode));

    ert_debug(
        0,
        "exit agent status %" PRId_Ert_ExitCode,
        FMTd_Ert_ExitCode(exitCode));

    rc = exitCode.mStatus;

Ert_Finally:

    ERT_FINALLY({});

    return rc;
}

static ERT_CHECKED int
runAgentProcess_(struct Agent *self, struct Ert_ExitCode *aExitCode)
{
    int rc = -1;

    struct Ert_JobControl  jobControl_;
    struct Ert_JobControl *jobControl = 0;

    struct ParentProcess  parentProcess_;
    struct ParentProcess *parentProcess = 0;

    struct RunAgentProcess_  agentChild_;
    struct RunAgentProcess_ *agentChild = 0;

    ERT_ERROR_IF(
        ert_createJobControl(&jobControl_));
    jobControl = &jobControl_;

    if (gOptions.mServer.mOrphaned)
    {
        ERT_ERROR_IF(
            createParentProcess(&parentProcess_));
        parentProcess = &parentProcess_;
    }

    ERT_ERROR_IF(
        createAgentChildProcess_(&agentChild_, self));
    agentChild = &agentChild_;

    struct Ert_Pid agentPid;
    ERT_ERROR_IF(
        (agentPid = ert_forkProcessChild(
            Ert_ForkProcessSetProcessGroup,
            Ert_Pgid(0),
            Ert_PreForkProcessMethod(
                agentChild, prepareAgentChildProcessFork_),
            Ert_PostForkChildProcessMethod(
                agentChild,
                ERT_LAMBDA(
                    int, (struct RunAgentProcess_ *self_),
                    {
                        ert_closePipeWriter(self_->mParentPipe);

                        return 0;
                    })),
            Ert_PostForkParentProcessMethod(
                agentChild,
                ERT_LAMBDA(
                    int, (struct RunAgentProcess_ *self_,
                          struct Ert_Pid           aChildPid),
                    {
                        ert_closePipeReader(self_->mParentPipe);

                        return 0;
                    })),
            Ert_ForkProcessMethod(agentChild, runAgentChildProcess_)),
         -1 == agentPid.mPid));

    self->mAgentPid = agentPid;

    /* Be prepared to deliver signals to the agent process only after
     * the process exists. Before this point, these signals will cause
     * the watchdog process to terminate, and the new process will
     * notice via its synchronisation pipe. */

    ERT_ERROR_IF(
        ert_watchJobControlSignals(
            jobControl,
            Ert_WatchProcessSignalMethod(self, raiseAgentSignal_)));

    ERT_ERROR_IF(
        ert_watchJobControlStop(
            jobControl,
            Ert_WatchProcessMethod(self, raiseAgentStop_),
            Ert_WatchProcessMethod(self, raiseAgentResume_)));

    {
        struct Ert_ChildProcessState agentState;
        ERT_ERROR_IF(
            (agentState = ert_waitProcessChild(self->mAgentPid),
             Ert_ChildProcessStateError == agentState.mChildState));

        /* If the agent process has been killed by SIGQUIT and dumped core,
         * then dump core in sympathy. */

        if (Ert_ChildProcessStateDumped == agentState.mChildState &&
            SIGQUIT == agentState.mChildStatus)
        {
            ert_quitProcess();
        }
    }

    {
        ERT_ERROR_IF(
            ert_unwatchJobControlStop(jobControl));

        ERT_ERROR_IF(
            ert_unwatchJobControlSignals(jobControl));

        /* Capture the pid of the agent process, then reset the data member
         * so that any signal races can be caught. */

        agentPid = self->mAgentPid;

        self->mAgentPid = Ert_Pid(0);

        int agentStatus;
        ERT_ERROR_IF(
            ert_reapProcessChild(agentPid, &agentStatus));

        ert_debug(
            0,
            "reaped agent pid %" PRId_Ert_Pid " status %d",
            FMTd_Ert_Pid(agentPid),
            agentStatus);

        *aExitCode =
            ert_extractProcessExitStatus(agentStatus, agentPid);
    }

    rc = 0;

Ert_Finally:

    ERT_FINALLY
    ({
        ert_finally_warn_if(rc, self, printAgent);

        agentChild    = closeAgentChildProcess_(agentChild);
        parentProcess = closeParentProcess(parentProcess);
        jobControl    = ert_closeJobControl(jobControl);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
runAgent(struct Agent        *self,
         struct Ert_ExitCode *aExitCode)
{
    int rc = -1;

    /* Only a process group leader can house the agent that owns the sentry.
     * The child process that runs the umbilical uses an anchor fixed
     * to the process group of the agent. This ensures that the pgid of the
     * anchor will not be repurposed for the lifetime of the pidsentry,
     * and the umbilical can kill the process group of the sentry even
     * if the sentry process itself has terminated. */

    if (ert_testAction(Ert_TestLevelRace) ||
        ert_ownProcessId().mPid != ert_ownProcessGroupId().mPgid)
    {
        ERT_ERROR_IF(
            runAgentProcess_(self, aExitCode));
    }
    else
    {
        ERT_ERROR_IF(
            runAgentSentry_(self, ert_ownProcessId(), 0, aExitCode));
    }

    rc = 0;

Ert_Finally:

    ERT_FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
