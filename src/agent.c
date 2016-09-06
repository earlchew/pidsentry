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

#include "fdset_.h"

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

/* -------------------------------------------------------------------------- */
static CHECKED int
raiseAgentSignal_(
    struct Agent *self, int aSigNum, struct Pid aPid, struct Uid aUid)
{
    struct Pid agentPid = self->mAgentPid;

    ensure(agentPid.mPid);

    return kill(agentPid.mPid, aSigNum);
}

static CHECKED int
raiseAgentStop_(struct Agent *self)
{
    struct Pid agentPid = self->mAgentPid;

    ensure(agentPid.mPid);

    return kill(agentPid.mPid, SIGTSTP);
}

static CHECKED int
raiseAgentResume_(struct Agent *self)
{
    struct Pid agentPid = self->mAgentPid;

    ensure(agentPid.mPid);

    return kill(agentPid.mPid, SIGCONT);
}

/* -------------------------------------------------------------------------- */
int
createAgent(struct Agent       *self,
            const char * const *aCmd)
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
static CHECKED int
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

        struct Pid announcePid;
        ERROR_IF(
            (announcePid = announceSentryPidFile(sentry),
             -1 == announcePid.mPid));

        if (announcePid.mPid)
        {
            warn(0,
                 "Pidfile '%s' names active pid %" PRId_Pid,
                 ownSentryPidFileName(sentry), FMTd_Pid(announcePid));

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
        finally_warn_if(rc, self, printAgent);

        sentry = closeSentry(sentry);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct RunAgentProcess_
{
    struct Agent *mAgent;
    struct Pid    mParentPid;
    struct Pipe   mParentPipe_;
    struct Pipe  *mParentPipe;
};

static CHECKED struct RunAgentProcess_ *
closeAgentChildProcess_(struct RunAgentProcess_ *self)
{
    if (self)
    {
        self->mParentPipe = closePipe(self->mParentPipe);
    }

    return 0;
}

static CHECKED int
createAgentChildProcess_(struct RunAgentProcess_ *self,
                         struct Agent            *aAgent)
{
    int rc = -1;

    self->mAgent      = aAgent;
    self->mParentPid  = ownProcessId();
    self->mParentPipe = 0;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
prepareAgentChildProcessFork_(struct RunAgentProcess_     *self,
                              const struct PreForkProcess *aPreFork)
{
    int rc = -1;

    struct StdFdFiller  stdFdFiller_;
    struct StdFdFiller *stdFdFiller = 0;

    /* Ensure that the parent pipe does not inadvertently become
     * stdin, stdout or stderr. */

    ERROR_IF(
        createStdFdFiller(&stdFdFiller_));
    stdFdFiller = &stdFdFiller_;

    ERROR_IF(
        createPipe(&self->mParentPipe_, O_CLOEXEC | O_NONBLOCK));
    self->mParentPipe = &self->mParentPipe_;

    ERROR_IF(
        insertFdSetRange(aPreFork->mWhitelistFds, FdRange(0, INT_MAX)));

    ERROR_IF(
        insertFdSetRange(aPreFork->mBlacklistFds, FdRange(0, INT_MAX)));
    ERROR_IF(
        removeFdSetFile(aPreFork->mBlacklistFds, self->mParentPipe->mWrFile));
    ERROR_IF(
        removeFdSetFile(aPreFork->mBlacklistFds, self->mParentPipe->mRdFile));

    rc = 0;

Finally:

    FINALLY
    ({
        stdFdFiller = closeStdFdFiller(stdFdFiller);
    });

    return rc;
}

static CHECKED int
runAgentChildProcess_(struct RunAgentProcess_ *self)
{
    int rc = -1;

    debug(
        0,
        "running agent pid %" PRId_Pid " in pgid %" PRId_Pgid,
        FMTd_Pid(ownProcessId()), FMTd_Pgid(ownProcessGroupId()));

    ensure(ownProcessId().mPid == ownProcessGroupId().mPgid);

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

static CHECKED int
runAgentProcess_(struct Agent *self, struct ExitCode *aExitCode)
{
    int rc = -1;

    struct JobControl  jobControl_;
    struct JobControl *jobControl = 0;

    struct ParentProcess  parentProcess_;
    struct ParentProcess *parentProcess = 0;

    struct RunAgentProcess_  agentChild_;
    struct RunAgentProcess_ *agentChild = 0;

    struct StdFdFiller  stdFdFiller_;
    struct StdFdFiller *stdFdFiller = 0;

    ERROR_IF(
        createJobControl(&jobControl_));
    jobControl = &jobControl_;

    if (gOptions.mServer.mOrphaned)
    {
        ERROR_IF(
            createParentProcess(&parentProcess_));
        parentProcess = &parentProcess_;
    }

    ERROR_IF(
        createAgentChildProcess_(&agentChild_, self));
    agentChild = &agentChild_;

    struct Pid agentPid;
    ERROR_IF(
        (agentPid = forkProcessChildX(
            ForkProcessSetProcessGroup,
            Pgid(0),
            PreForkProcessMethod(
                agentChild, prepareAgentChildProcessFork_),
            PostForkChildProcessMethod(
                agentChild,
                LAMBDA(
                    int, (struct RunAgentProcess_ *self_),
                    {
                        closePipeWriter(self_->mParentPipe);

                        return 0;
                    })),
            PostForkParentProcessMethod(
                agentChild,
                LAMBDA(
                    int, (struct RunAgentProcess_ *self_,
                          struct Pid               aChildPid),
                    {
                        closePipeReader(self_->mParentPipe);

                        return 0;
                    })),
            ForkProcessMethod(agentChild, runAgentChildProcess_)),
         -1 == agentPid.mPid));

    self->mAgentPid = agentPid;

    /* Be prepared to deliver signals to the agent process only after
     * the process exists. Before this point, these signals will cause
     * the watchdog process to terminate, and the new process will
     * notice via its synchronisation pipe. */

    ERROR_IF(
        watchJobControlSignals(
            jobControl,
            WatchProcessSignalMethod(self, raiseAgentSignal_)));

    ERROR_IF(
        watchJobControlStop(jobControl,
                            WatchProcessMethod(self, raiseAgentStop_),
                            WatchProcessMethod(self, raiseAgentResume_)));

    /* Hold a reference to stderr, but do not hold references to the
     * original stdin and stdout to allow the monitored process to control
     * when those file descriptors are released. */

    {
        ERROR_IF(
            createStdFdFiller(&stdFdFiller_));
        stdFdFiller = &stdFdFiller_;

        ERROR_IF(
            STDIN_FILENO != dup2(
                stdFdFiller->mFile[0]->mFd, STDIN_FILENO));

        ERROR_IF(
            STDOUT_FILENO != dup2(
                stdFdFiller->mFile[1]->mFd, STDOUT_FILENO));

        stdFdFiller = closeStdFdFiller(stdFdFiller);
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

    {
        ERROR_IF(
            unwatchJobControlStop(jobControl));

        ERROR_IF(
            unwatchJobControlSignals(jobControl));

        /* Capture the pid of the agent process, then reset the data member
         * so that any signal races can be caught. */

        agentPid = self->mAgentPid;

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
        finally_warn_if(rc, self, printAgent);

        stdFdFiller   = closeStdFdFiller(stdFdFiller);
        agentChild    = closeAgentChildProcess_(agentChild);
        parentProcess = closeParentProcess(parentProcess);
        jobControl    = closeJobControl(jobControl);
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
