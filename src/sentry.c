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

#include "sentry.h"
#include "pidserver.h"
#include "umbilical.h"

#include "pidfile_.h"
#include "error_.h"
#include "fd_.h"
#include "process_.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

#include <sys/un.h>

#include <valgrind/valgrind.h>

/* -------------------------------------------------------------------------- */
static int
reapSentry_(struct Sentry *self)
{
    struct Pid umbilicalPid =
        self->mUmbilicalProcess ? self->mUmbilicalProcess->mPid : Pid(0);

    return superviseChildProcess(self->mChildProcess, umbilicalPid);
}

static int
raiseSentrySignal_(struct Sentry *self, int aSigNum)
{
    /* Propagate the signal to the child. Note that SIGQUIT might cause
     * the child to terminate and dump core. Dump core in sympathy if this
     * happens, but do that only if the child actually does so. This is
     * taken care of in reapSentry_() when it calls superviseChildProcess(). */

    return killChild(self->mChildProcess, aSigNum);
}

static int
raiseSentryStop_(struct Sentry *self)
{
    return pauseChildProcessGroup(self->mChildProcess);
}

static int
raiseSentryResume_(struct Sentry *self)
{
    return resumeChildProcessGroup(self->mChildProcess);
}

static int
raiseSentrySigCont_(struct Sentry *self)
{
    return raiseChildSigCont(self->mChildProcess);
}

/* -------------------------------------------------------------------------- */
int
createSentry(struct Sentry *self,
             char         **aCmd)
{
    int rc = -1;

    self->mStdFdFiller      = 0;
    self->mUmbilicalSocket  = 0;
    self->mChildProcess     = 0;
    self->mJobControl       = 0;
    self->mSyncSocket       = 0;
    self->mPidFile          = 0;
    self->mPidServer        = 0;
    self->mUmbilicalProcess = 0;

    /* The instance of the StdFdFiller guarantees that any further file
     * descriptors that are opened will not be mistaken for stdin,
     * stdout or stderr. */

    ERROR_IF(
        createStdFdFiller(&self->mStdFdFiller_));
    self->mStdFdFiller = &self->mStdFdFiller_;

    ERROR_IF(
        createSocketPair(&self->mUmbilicalSocket_, O_NONBLOCK | O_CLOEXEC));
    self->mUmbilicalSocket = &self->mUmbilicalSocket_;

    ERROR_IF(
        createChild(&self->mChildProcess_));
    self->mChildProcess = &self->mChildProcess_;

    ERROR_IF(
        createJobControl(&self->mJobControl_));
    self->mJobControl = &self->mJobControl_;

    ERROR_IF(
        watchJobControlDone(self->mJobControl,
                            IntMethod(reapSentry_, self)));

    ERROR_IF(
        createBellSocketPair(&self->mSyncSocket_, O_CLOEXEC));
    self->mSyncSocket = &self->mSyncSocket_;

    ABORT_IF(
        forkChild(
            self->mChildProcess,
            aCmd,
            self->mStdFdFiller, self->mSyncSocket, self->mUmbilicalSocket),
        {
            terminate(
                errno,
                "Unable to fork child process");
        });

    /* Be prepared to deliver signals to the child process only after
     * the child exists. Before this point, these signals will cause
     * the watchdog to terminate, and the new child process will
     * notice via its synchronisation pipe. */

    ABORT_IF(
        watchJobControlSignals(
            self->mJobControl,
            IntIntMethod(raiseSentrySignal_, self)),
        {
            terminate(
                errno,
                "Unable to add watch on signals");
        });

    ABORT_IF(
        watchJobControlStop(self->mJobControl,
                            IntMethod(raiseSentryStop_, self),
                            IntMethod(raiseSentryResume_, self)),
        {
            terminate(
                errno,
                "Unable to add watch on process stop");
        });

    ABORT_IF(
        watchJobControlContinue(self->mJobControl,
                                IntMethod(raiseSentrySigCont_, self)),
        {
            terminate(
                errno,
                "Unable to add watch on process continuation");
        });

    /* If a pidfile is required, create it now so that it can
     * be anchored to its directory before changing the current
     * working directory. Note that the pidfile might reside in
     * the current directory. */

    if (gOptions.mPidFile)
    {
        ABORT_IF(
            initPidFile(&self->mPidFile_, gOptions.mPidFile),
            {
                terminate(
                    errno,
                    "Cannot initialise pid file '%s'", gOptions.mPidFile);
            });
        self->mPidFile = &self->mPidFile_;

        ABORT_IF(
            createPidServer(&self->mPidServer_),
            {
                terminate(
                    errno,
                    "Cannot create pid server for '%s'", gOptions.mPidFile);
            });
        self->mPidServer = &self->mPidServer_;
    }

    /* If not running in test mode, change directory to avoid holding
     * a reference that prevents a volume being unmounted. Otherwise
     * do not change directories in case a core file needs to be
     * generated. */

    if ( ! gOptions.mDebug)
    {
        static const char rootDir[] = "/";

        ABORT_IF(
            chdir("/"),
            {
                terminate(
                    errno,
                    "Unable to change directory to %s", rootDir);
            });
    }

    /* With the child process launched, close the instance of StdFdFiller
     * so that stdin, stdout and stderr become available for manipulation
     * and will not be closed multiple times. */

    closeStdFdFiller(self->mStdFdFiller);
    self->mStdFdFiller = 0;

    /* Discard the original stdin file descriptor, and instead attach
     * the reading end of the tether as stdin. This means that the
     * watchdog does not contribute any more references to the
     * original stdin file table entry. */

    ABORT_IF(
        STDIN_FILENO != dup2(
            self->mChildProcess->mTetherPipe->mRdFile->mFd, STDIN_FILENO),
        {
            terminate(
                errno,
                "Unable to dup tether pipe to stdin");
        });

    /* Now that the tether has been duplicated onto stdin and stdout
     * as required, it is important to close the tether to ensure that
     * the only possible references to the tether pipe remain in the
     * child process, if required, and stdin and stdout in this process. */

    ABORT_IF(
        closeChildTether(self->mChildProcess));

    ABORT_IF(
        purgeProcessOrphanedFds(),
        {
            terminate(
                errno,
                "Unable to purge orphaned files");
        });

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            closeSentry(self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closeSentry(struct Sentry *self)
{
    if (self)
    {
        closePidServer(self->mPidServer);
        destroyPidFile(self->mPidFile);
        closeBellSocketPair(self->mSyncSocket);
        closeJobControl(self->mJobControl);
        closeChild(self->mChildProcess);
        closeSocketPair(self->mUmbilicalSocket);
        closeStdFdFiller(self->mStdFdFiller);
    }
}

/* -------------------------------------------------------------------------- */
enum PidFileStatus
announceSentryPidFile(struct Sentry *self)
{
    /* Attempt to create the pidfile, if required, before creating the
     * umbilical process because it is quite possible for the attempt
     * to create the file to fail, and it is simpler to avoid having
     * clean up the umbilical process. */

    return ! self->mPidFile
        ? PidFileStatusOk
        : writePidFile(
            self->mPidFile,
            self->mChildProcess->mPid,
            &self->mPidServer->mSocketAddr);
}

/* -------------------------------------------------------------------------- */
const char *
ownSentryPidFileName(const struct Sentry *self)
{
    return ! self->mPidFile ? 0 : self->mPidFile->mPathName.mFileName;
}

/* -------------------------------------------------------------------------- */
int
runSentry(struct Sentry   *self,
          struct Pid       aParentPid,
          struct Pipe     *aParentPipe,
          struct ExitCode *aExitCode)
{
    int rc = -1;

    /* Monitor the watchdog using another process so that a failure
     * of the watchdog can be detected independently. Only create the
     * umbilical process after all the file descriptors have been
     * purged so that the umbilical does not inadvertently hold file
     * descriptors that should only be held by the child process. */

    ABORT_IF(
        createUmbilicalProcess(&self->mUmbilicalProcess_,
                               self->mChildProcess,
                               self->mUmbilicalSocket,
                               self->mPidServer),
        {
            terminate(
                errno,
                "Unable to create umbilical process");
        });
    self->mUmbilicalProcess = &self->mUmbilicalProcess_;

    closeSocketPairChild(self->mUmbilicalSocket);

    /* Beware of the inherent race here between the umbilical starting and
     * terminating, and the recording of the umbilical process. To cover the
     * case that the umbilical might have terminated before the process
     * is recorded, force a supervision run after the process is recorded. */

    reapSentry_(self);

    /* The PidServer instance will continue to run in the umbilical process,
       so the instance that was created in the watchdog is no longer
       required. */

    closePidServer(self->mPidServer);
    self->mPidServer = 0;

    if (gOptions.mIdentify)
    {
        /* Ensure that the pidfile, if requested, is created before the
         * process pids are identified. The unit test assumes that this
         * is the case. */

        if (self->mPidFile)
            ensure(self->mPidFile->mFile);

        TEST_RACE
        ({
            ABORT_IF(
                -1 == dprintf(STDOUT_FILENO,
                              "%" PRId_Pid " "
                              "%" PRId_Pid " "
                              "%" PRId_Pid "\n",
                              FMTd_Pid(aParentPid),
                              FMTd_Pid(ownProcessId()),
                              FMTd_Pid(self->mUmbilicalProcess->mPid)),
                {
                    terminate(
                        errno,
                        "Unable to print parent %" PRId_Pid ", "
                        "sentry pid %" PRId_Pid " and "
                        "umbilical pid %" PRId_Pid,
                        FMTd_Pid(aParentPid),
                        FMTd_Pid(ownProcessId()),
                        FMTd_Pid(self->mUmbilicalProcess->mPid));
                });
        });
    }

    /* With the child process announced, and the umbilical monitor
     * prepared, allow the child process to run the target program. */

    closeBellSocketPairChild(self->mSyncSocket);

    TEST_RACE
    ({
        /* The child process is waiting so that the child program will
         * run only after the pidfile has been created.
         *
         * Be aware that the supervisor might have sent a signal to the
         * watchdog which will have propagated it to the child, causing
         * the child to terminate. */

        ABORT_IF(
            ringBellSocketPairParent(self->mSyncSocket) && EPIPE != errno,
            {
                terminate(
                    errno,
                    "Unable to synchronise child process with "
                    "pid file creation");
            });

        /* Now wait for the child to respond to know that it has
         * received the indication that it can start running. */

        ABORT_IF(
            waitBellSocketPairParent(self->mSyncSocket, 0) &&
            EPIPE != errno && ENOENT != errno,
            {
                terminate(
                    errno,
                    "Unable synchronise with child process after "
                    "pid file creation");
            });
    });

    /* With the child acknowledging that it is ready to start
     * after the pidfile is created, announce the child pid if
     * required. Do this here before releasing the child process
     * so that this content does not become co-mingled with other
     * data on stdout when the child is running untethered. */

    if (gOptions.mIdentify)
    {
        TEST_RACE
        ({
            ABORT_IF(
                -1 == dprintf(STDOUT_FILENO,
                              "%" PRId_Pid "\n",
                              FMTd_Pid(self->mChildProcess->mPid)),
                {
                    terminate(
                        errno,
                        "Unable to print child pid");
                });
        });
    }

    TEST_RACE
    ({
        /* The child process is waiting to know that the child pid has
         * been announced. Indicate to the child process that this has
         * been done. */

        ABORT_IF(
            ringBellSocketPairParent(self->mSyncSocket) && EPIPE != errno,
            {
                terminate(
                    errno,
                    "Unable to synchronise child process with "
                    "pid announcement");
            });
    });

    /* Avoid closing the original stdout file descriptor only if
     * there is a need to copy the contents of the tether to it.
     * Otherwise, close the original stdout and open it as a sink so
     * that the watchdog does not contribute any more references to the
     * original stdout file table entry. */

    bool discardStdout = gOptions.mQuiet;

    if ( ! gOptions.mTether)
        discardStdout = true;
    else
    {
        int valid;
        ABORT_IF(
            (valid = ownFdValid(STDOUT_FILENO), -1 == valid),
            {
                terminate(
                    errno,
                    "Unable to check validity of stdout");
            });
        if ( ! valid)
            discardStdout = true;
    }

    if (discardStdout)
    {
        ABORT_IF(
            nullifyFd(STDOUT_FILENO),
            {
                terminate(
                    errno,
                    "Unable to nullify stdout");
            });
    }

    TEST_RACE
    ({
        /* Wait until the child has started the target child program to
         * know that the child is no longer sharing any file descriptors
         * or file locks.
         *
         * This is important to avoid deadlocks when the watchdog is
         * stopped by SIGSTOP, especially as part of test. */

        int err;
        ABORT_IF(
            (err = waitBellSocketPairParent(self->mSyncSocket, 0),
             ! err || (ENOENT != errno && EPIPE != errno)),
            {
                terminate(
                    err ? errno : 0,
                    "Unable synchronise with child process after "
                    "program execution");
            });
    });

    closeBellSocketPair(self->mSyncSocket);
    self->mSyncSocket = 0;

    /* Now that the child is no longer sharing any file descriptors or file
     * locks, stop the watchdog if the test requires it. Note that this
     * will race with other threads that might be holding a file lock, etc so
     * the watchdog will be stopped with the resources held. */

    if (testMode(TestLevelSync))
    {
        ABORT_IF(
            raise(SIGSTOP),
            {
                terminate(
                    errno,
                    "Unable to stop process pid %" PRId_Pid,
                    FMTd_Pid(ownProcessId()));
            });
    }

    /* Monitor the running child until it has either completed of
     * its own accord, or terminated. Once the child has stopped
     * running, release the pid file if one was allocated. */

    ABORT_IF(
        monitorChild(self->mChildProcess,
                     self->mUmbilicalProcess,
                     self->mUmbilicalSocket->mParentSocket->mFile,
                     aParentPid,
                     aParentPipe),
        {
            terminate(
                errno,
                "Unable to monitor child process");
        });

    ABORT_IF(
        unwatchJobControlContinue(self->mJobControl),
        {
            terminate(
                errno,
                "Unable to remove watch from process continuation");
        });

    ABORT_IF(
        unwatchJobControlSignals(self->mJobControl),
        {
            terminate(
                errno,
                "Unable to remove watch from signals");
        });

    ABORT_IF(
        unwatchJobControlDone(self->mJobControl),
        {
            terminate(
                errno,
                "Unable to remove watch on child process termination");
        });

    if (self->mPidFile)
    {
        ABORT_IF(
            acquirePidFileWriteLock(self->mPidFile),
            {
                terminate(
                    errno,
                    "Cannot lock pid file '%s'",
                    self->mPidFile->mPathName.mFileName);
            });

        destroyPidFile(self->mPidFile);
        self->mPidFile = 0;
    }

    /* Attempt to stop the umbilical process cleanly so that the watchdog
     * can exit in an orderly fashion with the exit status of the child
     * process as the last line emitted. */

    struct Pid umbilicalPid = self->mUmbilicalProcess->mPid;

    debug(0,
          "stopping umbilical pid %" PRId_Pid, FMTd_Pid(umbilicalPid));

    int notStopped;
    ABORT_IF(
        (notStopped = stopUmbilicalProcess(self->mUmbilicalProcess),
         notStopped && ETIMEDOUT != errno),
        {
            terminate(
                errno,
                "Unable to stop umbilical process pid %" PRId_Pid,
                FMTd_Pid(umbilicalPid));
        });

    if (notStopped)
        warn(0,
             "Unable to stop umbilical process pid %" PRId_Pid" cleanly",
             FMTd_Pid(umbilicalPid));

    /* The child process group is cleaned up from both the umbilical process
     * and the watchdog with the expectation that at least one of them
     * will succeed. At this point, the child process has already terminated
     * so killing the child process group will not change its exit
     * status. */

    ABORT_IF(
        killChildProcessGroup(self->mChildProcess));

    /* Reap the child only after the pid file is released. This ensures
     * that any competing reader that manages to sucessfully lock and
     * read the pid file will see the terminated process. */

    struct Pid childPid = self->mChildProcess->mPid;

    debug(0, "reaping child pid %" PRId_Pid, FMTd_Pid(childPid));

    int childStatus;
    ABORT_IF(
        reapChild(self->mChildProcess, &childStatus));

    closeChild(self->mChildProcess);
    self->mChildProcess = 0;

    debug(0,
          "reaped child pid %" PRId_Pid " status %d",
          FMTd_Pid(childPid),
          childStatus);

    closeSocketPair(self->mUmbilicalSocket);
    self->mUmbilicalSocket = 0;

    ABORT_IF(
        resetProcessSigPipe(),
        {
            terminate(
                errno,
                "Unable to reset SIGPIPE");
        });

    *aExitCode = extractProcessExitStatus(childStatus, childPid);

    /* Normally allow the umbilical process to terminate asynchronously,
     * but if running under valgrind, combine the exit codes to be
     * sure that the exit code only indicates success if the umbilical
     * process is also successful. */

    if (RUNNING_ON_VALGRIND)
    {
        int umbilicalStatus;
        ABORT_IF(
            reapProcessChild(umbilicalPid, &umbilicalStatus),
            {
                terminate(
                    errno,
                    "Unable to reap umbilical process pid %" PRId_Pid "status",
                    FMTd_Pid(umbilicalPid));
            });

        debug(0,
              "reaped umbilical pid %" PRId_Pid " status %d",
              FMTd_Pid(umbilicalPid),
              umbilicalStatus);

        struct ExitCode umbilicalExitCode =
            extractProcessExitStatus(umbilicalStatus, umbilicalPid);

        ABORT_IF(
            umbilicalExitCode.mStatus,
            {
                terminate(
                    0,
                    "Umbilical process pid %" PRId_Pid " "
                    "exit code %" PRId_ExitCode,
                    FMTd_Pid(umbilicalPid),
                    FMTd_ExitCode(umbilicalExitCode));
            });
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
