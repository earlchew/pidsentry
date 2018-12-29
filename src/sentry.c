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

#include "options_.h"

#include <fcntl.h>
#include <unistd.h>

#include <valgrind/valgrind.h>

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
reapSentry_(struct Sentry *self)
{
    struct Ert_Pid umbilicalPid =
        self->mUmbilicalProcess ? self->mUmbilicalProcess->mPid : Ert_Pid(0);

    return superviseChildProcess(self->mChildProcess, umbilicalPid);
}

static ERT_CHECKED int
raiseSentrySignal_(
    struct Sentry *self, int aSigNum, struct Ert_Pid aPid, struct Ert_Uid aUid)
{
    /* Propagate the signal to the child. Note that SIGQUIT might cause
     * the child to terminate and dump core. Dump core in sympathy if this
     * happens, but do that only if the child actually does so. This is
     * taken care of in reapSentry_() when it calls superviseChildProcess(). */

    return killChildProcess(self->mChildProcess, aSigNum);
}

static ERT_CHECKED int
raiseSentryStop_(struct Sentry *self)
{
    return pauseChildProcessGroup(self->mChildProcess);
}

static ERT_CHECKED int
raiseSentryResume_(struct Sentry *self)
{
    return resumeChildProcessGroup(self->mChildProcess);
}

static ERT_CHECKED int
raiseSentrySigCont_(struct Sentry *self)
{
    return raiseChildProcessSigCont(self->mChildProcess);
}

/* -------------------------------------------------------------------------- */
int
createSentry(struct Sentry      *self,
             const char * const *aCmd)
{
    int rc = -1;

    self->mUmbilicalSocket  = 0;
    self->mChildProcess     = 0;
    self->mJobControl       = 0;
    self->mSyncSocket       = 0;
    self->mPidFile          = 0;
    self->mPidServer        = 0;
    self->mUmbilicalProcess = 0;

    ERT_ERROR_IF(
        ert_createSocketPair(&self->mUmbilicalSocket_, O_NONBLOCK | O_CLOEXEC));
    self->mUmbilicalSocket = &self->mUmbilicalSocket_;

    ERT_ERROR_IF(
        createChildProcess(&self->mChildProcess_));
    self->mChildProcess = &self->mChildProcess_;

    ERT_ERROR_IF(
        ert_createJobControl(&self->mJobControl_));
    self->mJobControl = &self->mJobControl_;

    ERT_ERROR_IF(
        ert_watchJobControlDone(
            self->mJobControl,
            Ert_WatchProcessMethod(self, reapSentry_)));

    ERT_ERROR_IF(
        ert_createBellSocketPair(&self->mSyncSocket_, O_CLOEXEC));
    self->mSyncSocket = &self->mSyncSocket_;

    ERT_ERROR_IF(
        forkChildProcess(
            self->mChildProcess,
            aCmd, self->mSyncSocket, self->mUmbilicalSocket));

    /* Be prepared to deliver signals to the child process only after
     * the child exists. Before this point, these signals will cause
     * the watchdog to terminate, and the new child process will
     * notice via its synchronisation pipe. */

    ERT_ERROR_IF(
        ert_watchJobControlSignals(
            self->mJobControl,
            Ert_WatchProcessSignalMethod(self, raiseSentrySignal_)));

    ERT_ERROR_IF(
        ert_watchJobControlStop(
            self->mJobControl,
            Ert_WatchProcessMethod(self, raiseSentryStop_),
            Ert_WatchProcessMethod(self, raiseSentryResume_)));

    ERT_ERROR_IF(
        ert_watchJobControlContinue(
            self->mJobControl,
            Ert_WatchProcessMethod(self, raiseSentrySigCont_)));

    /* If a pidfile is required, create it now so that it can
     * be anchored to its directory before changing the current
     * working directory. Note that the pidfile might reside in
     * the current directory. */

    if (gOptions.mServer.mPidFile)
    {
        ERT_ERROR_IF(
            Ert_PathNameStatusOk != initPidFile(
                &self->mPidFile_, gOptions.mServer.mPidFile),
            {
                ert_warn(
                    errno,
                    "Cannot initialise pid file '%s'",
                    gOptions.mServer.mPidFile);
            });
        self->mPidFile = &self->mPidFile_;

        ERT_ERROR_IF(
            createPidServer(&self->mPidServer_,
                            self->mChildProcess->mPid));
        self->mPidServer = &self->mPidServer_;
    }

    /* If not running in test mode, change directory to avoid holding
     * a reference that prevents a volume being unmounted. Otherwise
     * do not change directories in case a core file needs to be
     * generated. */

    if ( ! ert_debuglevel(0))
    {
        static const char rootDir[] = "/";

        ERT_ERROR_IF(
            chdir("/"),
            {
                ert_warn(
                    errno,
                    "Unable to change directory to %s", rootDir);
            });
    }

    /* Discard the original stdin file descriptor, and instead attach
     * the reading end of the tether as stdin. This means that the
     * watchdog does not contribute any more references to the
     * original stdin file table entry. */

    ERT_ERROR_IF(
        STDIN_FILENO != ert_duplicateFd(
            self->mChildProcess->mTetherPipe->mRdFile->mFd, STDIN_FILENO));

    /* Now that the tether has been duplicated onto stdin and stdout
     * as required, it is important to close the tether to ensure that
     * the only possible references to the tether pipe remain in the
     * child process, if required, and stdin and stdout in this process. */

    ERT_ERROR_IF(
        closeChildProcessTether(self->mChildProcess));

    ERT_ERROR_IF(
        ert_purgeProcessOrphanedFds());

    rc = 0;

Ert_Finally:

    ERT_FINALLY
    ({
        if (rc)
            closeSentry(self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct Sentry *
closeSentry(struct Sentry *self)
{
    if (self)
    {
        self->mPidServer       = closePidServer(self->mPidServer);
        self->mPidFile         = destroyPidFile(self->mPidFile);
        self->mSyncSocket      = ert_closeBellSocketPair(self->mSyncSocket);
        self->mJobControl      = ert_closeJobControl(self->mJobControl);
        self->mChildProcess    = closeChildProcess(self->mChildProcess);
        self->mUmbilicalSocket = ert_closeSocketPair(self->mUmbilicalSocket);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
struct Ert_Pid
announceSentryPidFile(struct Sentry *self)
{
    /* Attempt to create the pidfile, if required, before creating the
     * umbilical process because it is quite possible for the attempt
     * to create the file to fail, and it is simpler to avoid having
     * clean up the umbilical process. */

    return ! self->mPidFile
        ? Ert_Pid(0)
        : createPidFile(
            self->mPidFile,
            self->mChildProcess->mPid,
            &self->mPidServer->mSocketAddr,
            gOptions.mServer.mPidFileMode);
}

/* -------------------------------------------------------------------------- */
const char *
ownSentryPidFileName(const struct Sentry *self)
{
    return ! self->mPidFile ? 0 : ownPidFileName(self->mPidFile);
}

/* -------------------------------------------------------------------------- */
int
runSentry(struct Sentry       *self,
          struct Ert_Pid       aParentPid,
          struct Ert_Pipe     *aParentPipe,
          struct Ert_ExitCode *aExitCode)
{
    int rc = -1;

    struct Ert_Pid childPid = self->mChildProcess->mPid;

    /* Monitor the watchdog using another process so that a failure
     * of the watchdog can be detected independently. Only create the
     * umbilical process after all the file descriptors have been
     * purged so that the umbilical does not inadvertently hold file
     * descriptors that should only be held by the child process. */

    ERT_ERROR_IF(
        createUmbilicalProcess(&self->mUmbilicalProcess_,
                               self->mChildProcess,
                               self->mUmbilicalSocket,
                               self->mPidServer),
        {
            ert_terminate(
                errno,
                "Unable to create umbilical process");
        });
    self->mUmbilicalProcess = &self->mUmbilicalProcess_;

    ert_ensure( ! self->mUmbilicalSocket->mChildSocket);

    /* Beware of the inherent race here between the umbilical starting and
     * terminating, and the recording of the umbilical process. To cover the
     * case that the umbilical might have terminated before the process
     * is recorded, force a supervision run after the process is recorded. */

    ERT_ERROR_IF(
        reapSentry_(self));

    /* The PidServer instance will continue to run in the umbilical process,
       so the instance that was created in the watchdog is no longer
       required. */

    self->mPidServer = closePidServer(self->mPidServer);

    if (gOptions.mServer.mIdentify)
    {
        /* Ensure that the pidfile, if requested, is created before the
         * process pids are identified. The unit test assumes that this
         * is the case. */

        if (self->mPidFile)
            ert_ensure(self->mPidFile->mFile);

        ERT_TEST_RACE
        ({
            ERT_ERROR_IF(
                -1 == dprintf(STDOUT_FILENO,
                              "%" PRId_Ert_Pid " "
                              "%" PRId_Ert_Pid " "
                              "%" PRId_Ert_Pid "\n",
                              FMTd_Ert_Pid(aParentPid),
                              FMTd_Ert_Pid(ert_ownProcessId()),
                              FMTd_Ert_Pid(self->mUmbilicalProcess->mPid)),
                {
                    ert_terminate(
                        errno,
                        "Unable to print parent %" PRId_Ert_Pid ", "
                        "sentry pid %" PRId_Ert_Pid " and "
                        "umbilical pid %" PRId_Ert_Pid,
                        FMTd_Ert_Pid(aParentPid),
                        FMTd_Ert_Pid(ert_ownProcessId()),
                        FMTd_Ert_Pid(self->mUmbilicalProcess->mPid));
                });
        });
    }

    /* With the child process announced, and the umbilical monitor
     * prepared, allow the child process to run the target program. */

    ert_closeBellSocketPairChild(self->mSyncSocket);

    ERT_TEST_RACE
    ({
        /* The child process is waiting so that the child program will
         * run only after the pidfile has been created.
         *
         * Be aware that the supervisor might have sent a signal to the
         * watchdog which will have propagated it to the child, causing
         * the child to terminate. */

        ERT_ERROR_IF(
            ert_ringBellSocketPairParent(self->mSyncSocket) && EPIPE != errno);

        /* Now wait for the child to respond to know that it has
         * received the indication that it can start running. */

        ERT_ERROR_IF(
            ert_waitBellSocketPairParent(self->mSyncSocket, 0) &&
            EPIPE != errno && ENOENT != errno);
    });

    /* With the child acknowledging that it is ready to start
     * after the pidfile is created, announce the child pid if
     * required. Do this here before releasing the child process
     * so that this content does not become co-mingled with other
     * data on stdout when the child is running untethered. */

    if (gOptions.mServer.mIdentify)
    {
        ERT_TEST_RACE
        ({
            ERT_ERROR_IF(
                -1 == dprintf(STDOUT_FILENO,
                              "%" PRId_Ert_Pid "\n",
                              FMTd_Ert_Pid(childPid)));
        });
    }

    if (gOptions.mServer.mAnnounce)
        ert_message(0,
                "started pid %" PRId_Ert_Pid " %s",
                FMTd_Ert_Pid(childPid),
                ownShellCommandName(self->mChildProcess->mShellCommand));

    ERT_TEST_RACE
    ({
        /* The child process is waiting to know that the child pid has
         * been announced. Indicate to the child process that this has
         * been done. */

        ERT_ERROR_IF(
            ert_ringBellSocketPairParent(self->mSyncSocket) && EPIPE != errno);
    });

    /* Avoid closing the original stdout file descriptor only if
     * there is a need to copy the contents of the tether to it.
     * Otherwise, close the original stdout and open it as a sink so
     * that the watchdog does not contribute any more references to the
     * original stdout file table entry. */

    bool discardStdout = gOptions.mServer.mQuiet;

    if ( ! gOptions.mServer.mTether)
        discardStdout = true;
    else
    {
        int valid;
        ERT_ERROR_IF(
            (valid = ert_ownFdValid(STDOUT_FILENO),
             -1 == valid));
        if ( ! valid)
            discardStdout = true;
    }

    if (discardStdout)
    {
        ERT_ERROR_IF(
            ert_nullifyFd(STDOUT_FILENO));
    }

    ERT_TEST_RACE
    ({
        /* Wait until the child has started the target child program to
         * know that the child is no longer sharing any file descriptors
         * or file locks.
         *
         * This is important to avoid deadlocks when the watchdog is
         * stopped by SIGSTOP, especially as part of test. */

        int err;
        ERT_ERROR_IF(
            (err = ert_waitBellSocketPairParent(self->mSyncSocket, 0),
             err
             ? (ENOENT != errno && EPIPE != errno)
             : (errno = 0, true)));
    });

    self->mSyncSocket = ert_closeBellSocketPair(self->mSyncSocket);

    /* Now that the child is no longer sharing any file descriptors or file
     * locks, stop the watchdog if the test requires it. Note that this
     * will race with other threads that might be holding a file lock, etc so
     * the watchdog will be stopped with the resources held. */

    if (ert_testMode(Ert_TestLevelSync))
    {
        ERT_ERROR_IF(
            raise(SIGSTOP));
    }

    /* Monitor the running child until it has either completed of
     * its own accord, or terminated. Once the child has stopped
     * running, release the pid file if one was allocated. */

    ERT_ERROR_IF(
        monitorChildProcess(
            self->mChildProcess,
            self->mUmbilicalProcess,
            self->mUmbilicalSocket->mParentSocket->mSocket->mFile,
            aParentPid,
            aParentPipe));

    /* Attempt to stop the umbilical process cleanly so that the watchdog
     * can exit in an orderly fashion with the exit status of the child
     * process as the last line emitted. */

    struct Ert_Pid umbilicalPid = self->mUmbilicalProcess->mPid;

    ert_debug(
        0,
        "stopping umbilical pid %" PRId_Ert_Pid, FMTd_Ert_Pid(umbilicalPid));

    int notStopped;
    ERT_ERROR_IF(
        (notStopped = stopUmbilicalProcess(self->mUmbilicalProcess),
         notStopped && ETIMEDOUT != errno),
        {
            ert_warn(
                errno,
                "Unable to stop umbilical process pid %" PRId_Ert_Pid,
                FMTd_Ert_Pid(umbilicalPid));
        });

    if (notStopped)
        ert_warn(
            0,
            "Unable to stop umbilical process pid %" PRId_Ert_Pid" cleanly",
            FMTd_Ert_Pid(umbilicalPid));

    self->mUmbilicalSocket = ert_closeSocketPair(self->mUmbilicalSocket);

    /* The child process group is cleaned up from both the umbilical process
     * and the watchdog with the expectation that at least one of them
     * will succeed. At this point, the child process has already terminated
     * so killing the child process group will not change its exit
     * status. */

    ERT_ERROR_IF(
        killChildProcessGroup(self->mChildProcess));

    if (gOptions.mServer.mAnnounce)
        ert_message(0,
                "stopped pid %" PRId_Ert_Pid " %s",
                FMTd_Ert_Pid(childPid),
                ownShellCommandName(self->mChildProcess->mShellCommand));

    /* If a pid file is in use, do not reap the child process until
     * a lock on the pid file can be acquired, and the pid file invalidated.
     *
     * Also do not acquire the pid file lock and destroy the pid file until
     * after the umbilical has been stopped, to avoid triggering the
     * umbilical should there be an extended lock acquisition time. Note
     * that this is symmetric with what occurs when the pid file is
     * locked and initialised before the umbilical process is started.
     *
     * On the other side, readers would acquire a lock on the pid file
     * before reading the pid file and connecting to the pid server. */

    if (self->mPidFile)
    {
        ERT_ERROR_IF(
            acquirePidFileWriteLock(self->mPidFile));

        self->mPidFile = destroyPidFile(self->mPidFile);
    }

    /* The child process has terminated, and the umbilical process should
     * have terminated, so detach the signal watchers. After this point
     * a signal received by the watchdog will likely cause it to terminate,
     * but the child process group has already been cleaned up and the
     * the only action left from here is to reap the child process. */

    ERT_ERROR_IF(
        ert_unwatchJobControlContinue(self->mJobControl));

    ERT_ERROR_IF(
        ert_unwatchJobControlStop(self->mJobControl));

    ERT_ERROR_IF(
        ert_unwatchJobControlSignals(self->mJobControl));

    ERT_ERROR_IF(
        ert_unwatchJobControlDone(self->mJobControl));

    /* Reap the child only after the pid file is released. This ensures
     * that any competing reader that manages to sucessfully lock and
     * read the pid file will see the terminated process. */

    ert_debug(0, "reaping child pid %" PRId_Ert_Pid, FMTd_Ert_Pid(childPid));

    int childStatus;
    ERT_ERROR_IF(
        reapChildProcess(self->mChildProcess, &childStatus));

    self->mChildProcess = closeChildProcess(self->mChildProcess);

    ert_debug(
        0,
        "reaped child pid %" PRId_Ert_Pid " status %d",
        FMTd_Ert_Pid(childPid),
        childStatus);

    *aExitCode = ert_extractProcessExitStatus(childStatus, childPid);

    /* Normally allow the umbilical process to terminate asynchronously,
     * but if running under valgrind, combine the exit codes to be
     * sure that the exit code only indicates success if the umbilical
     * process is also successful. */

    if (RUNNING_ON_VALGRIND)
    {
        int umbilicalStatus;
        ERT_ERROR_IF(
            ert_reapProcessChild(umbilicalPid, &umbilicalStatus));

        ert_debug(
            0,
            "reaped umbilical pid %" PRId_Ert_Pid " status %d",
            FMTd_Ert_Pid(umbilicalPid),
            umbilicalStatus);

        struct Ert_ExitCode umbilicalExitCode =
            ert_extractProcessExitStatus(umbilicalStatus, umbilicalPid);

        ERT_ERROR_IF(
            umbilicalExitCode.mStatus,
            {
                errno = 0;

                ert_warn(
                    0,
                    "Umbilical process pid %" PRId_Ert_Pid " "
                    "exit code %" PRId_Ert_ExitCode,
                    FMTd_Ert_Pid(umbilicalPid),
                    FMTd_Ert_ExitCode(umbilicalExitCode));
            });
    }

    rc = 0;

Ert_Finally:

    ERT_FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
