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


#include <fcntl.h>
#include <unistd.h>


#include <valgrind/valgrind.h>

/* -------------------------------------------------------------------------- */
static CHECKED int
reapSentry_(struct Sentry *self)
{
    struct Pid umbilicalPid =
        self->mUmbilicalProcess ? self->mUmbilicalProcess->mPid : Pid(0);

    return superviseChildProcess(self->mChildProcess, umbilicalPid);
}

static CHECKED int
raiseSentrySignal_(
    struct Sentry *self, int aSigNum, struct Pid aPid, struct Uid aUid)
{
    /* Propagate the signal to the child. Note that SIGQUIT might cause
     * the child to terminate and dump core. Dump core in sympathy if this
     * happens, but do that only if the child actually does so. This is
     * taken care of in reapSentry_() when it calls superviseChildProcess(). */

    return killChildProcess(self->mChildProcess, aSigNum);
}

static CHECKED int
raiseSentryStop_(struct Sentry *self)
{
    return pauseChildProcessGroup(self->mChildProcess);
}

static CHECKED int
raiseSentryResume_(struct Sentry *self)
{
    return resumeChildProcessGroup(self->mChildProcess);
}

static CHECKED int
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

    ERROR_IF(
        createSocketPair(&self->mUmbilicalSocket_, O_NONBLOCK | O_CLOEXEC));
    self->mUmbilicalSocket = &self->mUmbilicalSocket_;

    ERROR_IF(
        createChildProcess(&self->mChildProcess_));
    self->mChildProcess = &self->mChildProcess_;

    ERROR_IF(
        createJobControl(&self->mJobControl_));
    self->mJobControl = &self->mJobControl_;

    ERROR_IF(
        watchJobControlDone(self->mJobControl,
                            WatchProcessMethod(self, reapSentry_)));

    ERROR_IF(
        createBellSocketPair(&self->mSyncSocket_, O_CLOEXEC));
    self->mSyncSocket = &self->mSyncSocket_;

    ERROR_IF(
        forkChildProcess(
            self->mChildProcess,
            aCmd, self->mSyncSocket, self->mUmbilicalSocket));

    /* Be prepared to deliver signals to the child process only after
     * the child exists. Before this point, these signals will cause
     * the watchdog to terminate, and the new child process will
     * notice via its synchronisation pipe. */

    ERROR_IF(
        watchJobControlSignals(
            self->mJobControl,
            WatchProcessSignalMethod(self, raiseSentrySignal_)));

    ERROR_IF(
        watchJobControlStop(self->mJobControl,
                            WatchProcessMethod(self, raiseSentryStop_),
                            WatchProcessMethod(self, raiseSentryResume_)));

    ERROR_IF(
        watchJobControlContinue(self->mJobControl,
                                WatchProcessMethod(self, raiseSentrySigCont_)));

    /* If a pidfile is required, create it now so that it can
     * be anchored to its directory before changing the current
     * working directory. Note that the pidfile might reside in
     * the current directory. */

    if (gOptions.mServer.mPidFile)
    {
        ERROR_IF(
            PathNameStatusOk != initPidFile(
                &self->mPidFile_, gOptions.mServer.mPidFile),
            {
                warn(
                    errno,
                    "Cannot initialise pid file '%s'",
                    gOptions.mServer.mPidFile);
            });
        self->mPidFile = &self->mPidFile_;

        ERROR_IF(
            createPidServer(&self->mPidServer_,
                            self->mChildProcess->mPid));
        self->mPidServer = &self->mPidServer_;
    }

    /* If not running in test mode, change directory to avoid holding
     * a reference that prevents a volume being unmounted. Otherwise
     * do not change directories in case a core file needs to be
     * generated. */

    if ( ! gOptions.mDebug)
    {
        static const char rootDir[] = "/";

        ERROR_IF(
            chdir("/"),
            {
                warn(
                    errno,
                    "Unable to change directory to %s", rootDir);
            });
    }

    /* Discard the original stdin file descriptor, and instead attach
     * the reading end of the tether as stdin. This means that the
     * watchdog does not contribute any more references to the
     * original stdin file table entry. */

    ERROR_IF(
        STDIN_FILENO != duplicateFd(
            self->mChildProcess->mTetherPipe->mRdFile->mFd, STDIN_FILENO));

    /* Now that the tether has been duplicated onto stdin and stdout
     * as required, it is important to close the tether to ensure that
     * the only possible references to the tether pipe remain in the
     * child process, if required, and stdin and stdout in this process. */

    ERROR_IF(
        closeChildProcessTether(self->mChildProcess));

    ERROR_IF(
        purgeProcessOrphanedFds());

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
struct Sentry *
closeSentry(struct Sentry *self)
{
    if (self)
    {
        self->mPidServer       = closePidServer(self->mPidServer);
        self->mPidFile         = destroyPidFile(self->mPidFile);
        self->mSyncSocket      = closeBellSocketPair(self->mSyncSocket);
        self->mJobControl      = closeJobControl(self->mJobControl);
        self->mChildProcess    = closeChildProcess(self->mChildProcess);
        self->mUmbilicalSocket = closeSocketPair(self->mUmbilicalSocket);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
struct Pid
announceSentryPidFile(struct Sentry *self)
{
    /* Attempt to create the pidfile, if required, before creating the
     * umbilical process because it is quite possible for the attempt
     * to create the file to fail, and it is simpler to avoid having
     * clean up the umbilical process. */

    return ! self->mPidFile
        ? Pid(0)
        : writePidFile(
            self->mPidFile,
            self->mChildProcess->mPid,
            &self->mPidServer->mSocketAddr);
}

/* -------------------------------------------------------------------------- */
const char *
ownSentryPidFileName(const struct Sentry *self)
{
    return ! self->mPidFile ? 0 : ownPidFileName(self->mPidFile);
}

/* -------------------------------------------------------------------------- */
int
runSentry(struct Sentry   *self,
          struct Pid       aParentPid,
          struct Pipe     *aParentPipe,
          struct ExitCode *aExitCode)
{
    int rc = -1;

    struct Pid childPid = self->mChildProcess->mPid;

    /* Monitor the watchdog using another process so that a failure
     * of the watchdog can be detected independently. Only create the
     * umbilical process after all the file descriptors have been
     * purged so that the umbilical does not inadvertently hold file
     * descriptors that should only be held by the child process. */

    ERROR_IF(
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

    ensure( ! self->mUmbilicalSocket->mChildSocket);

    /* Beware of the inherent race here between the umbilical starting and
     * terminating, and the recording of the umbilical process. To cover the
     * case that the umbilical might have terminated before the process
     * is recorded, force a supervision run after the process is recorded. */

    ERROR_IF(
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
            ensure(self->mPidFile->mFile);

        TEST_RACE
        ({
            ERROR_IF(
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

        ERROR_IF(
            ringBellSocketPairParent(self->mSyncSocket) && EPIPE != errno);

        /* Now wait for the child to respond to know that it has
         * received the indication that it can start running. */

        ERROR_IF(
            waitBellSocketPairParent(self->mSyncSocket, 0) &&
            EPIPE != errno && ENOENT != errno);
    });

    /* With the child acknowledging that it is ready to start
     * after the pidfile is created, announce the child pid if
     * required. Do this here before releasing the child process
     * so that this content does not become co-mingled with other
     * data on stdout when the child is running untethered. */

    if (gOptions.mServer.mIdentify)
    {
        TEST_RACE
        ({
            ERROR_IF(
                -1 == dprintf(STDOUT_FILENO,
                              "%" PRId_Pid "\n",
                              FMTd_Pid(childPid)));
        });
    }

    if (gOptions.mServer.mAnnounce)
        message(0,
                "started pid %" PRId_Pid " %s",
                FMTd_Pid(childPid),
                ownShellCommandName(self->mChildProcess->mShellCommand));

    TEST_RACE
    ({
        /* The child process is waiting to know that the child pid has
         * been announced. Indicate to the child process that this has
         * been done. */

        ERROR_IF(
            ringBellSocketPairParent(self->mSyncSocket) && EPIPE != errno);
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
        ERROR_IF(
            (valid = ownFdValid(STDOUT_FILENO),
             -1 == valid));
        if ( ! valid)
            discardStdout = true;
    }

    if (discardStdout)
    {
        ERROR_IF(
            nullifyFd(STDOUT_FILENO));
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
        ERROR_IF(
            (err = waitBellSocketPairParent(self->mSyncSocket, 0),
             err
             ? (ENOENT != errno && EPIPE != errno)
             : (errno = 0, true)));
    });

    self->mSyncSocket = closeBellSocketPair(self->mSyncSocket);

    /* Now that the child is no longer sharing any file descriptors or file
     * locks, stop the watchdog if the test requires it. Note that this
     * will race with other threads that might be holding a file lock, etc so
     * the watchdog will be stopped with the resources held. */

    if (testMode(TestLevelSync))
    {
        ERROR_IF(
            raise(SIGSTOP));
    }

    /* Monitor the running child until it has either completed of
     * its own accord, or terminated. Once the child has stopped
     * running, release the pid file if one was allocated. */

    ERROR_IF(
        monitorChildProcess(
            self->mChildProcess,
            self->mUmbilicalProcess,
            self->mUmbilicalSocket->mParentSocket->mSocket->mFile,
            aParentPid,
            aParentPipe));

    /* Attempt to stop the umbilical process cleanly so that the watchdog
     * can exit in an orderly fashion with the exit status of the child
     * process as the last line emitted. */

    struct Pid umbilicalPid = self->mUmbilicalProcess->mPid;

    debug(0,
          "stopping umbilical pid %" PRId_Pid, FMTd_Pid(umbilicalPid));

    int notStopped;
    ERROR_IF(
        (notStopped = stopUmbilicalProcess(self->mUmbilicalProcess),
         notStopped && ETIMEDOUT != errno),
        {
            warn(
                errno,
                "Unable to stop umbilical process pid %" PRId_Pid,
                FMTd_Pid(umbilicalPid));
        });

    if (notStopped)
        warn(0,
             "Unable to stop umbilical process pid %" PRId_Pid" cleanly",
             FMTd_Pid(umbilicalPid));

    self->mUmbilicalSocket = closeSocketPair(self->mUmbilicalSocket);

    /* The child process group is cleaned up from both the umbilical process
     * and the watchdog with the expectation that at least one of them
     * will succeed. At this point, the child process has already terminated
     * so killing the child process group will not change its exit
     * status. */

    ERROR_IF(
        killChildProcessGroup(self->mChildProcess));

    if (gOptions.mServer.mAnnounce)
        message(0,
                "stopped pid %" PRId_Pid " %s",
                FMTd_Pid(childPid),
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
        ERROR_IF(
            acquirePidFileWriteLock(self->mPidFile));

        self->mPidFile = destroyPidFile(self->mPidFile);
    }

    /* The child process has terminated, and the umbilical process should
     * have terminated, so detach the signal watchers. After this point
     * a signal received by the watchdog will likely cause it to terminate,
     * but the child process group has already been cleaned up and the
     * the only action left from here is to reap the child process. */

    ERROR_IF(
        unwatchJobControlContinue(self->mJobControl));

    ERROR_IF(
        unwatchJobControlStop(self->mJobControl));

    ERROR_IF(
        unwatchJobControlSignals(self->mJobControl));

    ERROR_IF(
        unwatchJobControlDone(self->mJobControl));

    /* Reap the child only after the pid file is released. This ensures
     * that any competing reader that manages to sucessfully lock and
     * read the pid file will see the terminated process. */

    debug(0, "reaping child pid %" PRId_Pid, FMTd_Pid(childPid));

    int childStatus;
    ERROR_IF(
        reapChildProcess(self->mChildProcess, &childStatus));

    self->mChildProcess = closeChildProcess(self->mChildProcess);

    debug(0,
          "reaped child pid %" PRId_Pid " status %d",
          FMTd_Pid(childPid),
          childStatus);

    *aExitCode = extractProcessExitStatus(childStatus, childPid);

    /* Normally allow the umbilical process to terminate asynchronously,
     * but if running under valgrind, combine the exit codes to be
     * sure that the exit code only indicates success if the umbilical
     * process is also successful. */

    if (RUNNING_ON_VALGRIND)
    {
        int umbilicalStatus;
        ERROR_IF(
            reapProcessChild(umbilicalPid, &umbilicalStatus));

        debug(0,
              "reaped umbilical pid %" PRId_Pid " status %d",
              FMTd_Pid(umbilicalPid),
              umbilicalStatus);

        struct ExitCode umbilicalExitCode =
            extractProcessExitStatus(umbilicalStatus, umbilicalPid);

        ERROR_IF(
            umbilicalExitCode.mStatus,
            {
                errno = 0;

                warn(
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
