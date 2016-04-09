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

#include "type_.h"
#include "pidfile_.h"
#include "error_.h"
#include "fd_.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

#include <sys/un.h>

static const struct Type * const sentryType_ = TYPE("Sentry");

/* -------------------------------------------------------------------------- */
static int
announceChild_(struct PidFile    *aPidFile,
               struct Pid         aPid,
               struct sockaddr_un aPidServerAddr)
{
    int rc = -1;

    for (int zombie = -1; zombie; )
    {
        if (0 < zombie)
        {
            /* If the pidfile has become a zombie, it is possible to
             * delete it here, but do not attempt to do so, and instead
             * rely on the correct deletion semantics to be used when
             * a new attempt is made to open the pidfile. */

            ABORT_IF(
                releaseLockPidFile(aPidFile),
                {
                    terminate(
                        errno,
                        "Cannot release lock on pid file '%s'",
                        aPidFile->mPathName.mFileName);
                });

            debug(0,
                  "disregarding zombie pid file '%s'",
                  aPidFile->mPathName.mFileName);

            closePidFile(aPidFile);
        }

        ERROR_IF(
            openPidFile(aPidFile, O_CLOEXEC | O_CREAT),
            {
                warn(
                    errno,
                    "Cannot create pid file '%s'",
                    aPidFile->mPathName.mFileName);
            });

        /* It is not possible to create the pidfile and acquire a flock
         * as an atomic operation. The flock can only be acquired after
         * the pidfile exists. Since this newly created pidfile is empty,
         * it resembles an closed pidfile, and in the intervening time,
         * another process might have removed it and replaced it with
         * another, turning the pidfile held by this process into a zombie. */

        ABORT_IF(
            acquireWriteLockPidFile(aPidFile),
            {
                terminate(
                    errno,
                    "Cannot acquire write lock on pid file '%s'",
                    aPidFile->mPathName.mFileName);
            });

        ABORT_IF(
            (zombie = detectPidFileZombie(aPidFile),
             0 > zombie),
            {
                terminate(
                    errno,
                    "Unable to obtain status of pid file '%s'",
                    aPidFile->mPathName.mFileName);
            });
    }

    /* At this point, this process has a newly created, empty and locked
     * pidfile. The pidfile cannot be deleted because a write lock must
     * be held for deletion to occur. */

    debug(0, "initialised pid file '%s'", aPidFile->mPathName.mFileName);

    ABORT_IF(
        writePidFile(aPidFile, aPid, &aPidServerAddr),
        {
            terminate(
                errno,
                "Cannot write to pid file '%s'", aPidFile->mPathName.mFileName);
        });

    /* The pidfile was locked on creation, and now that it is completely
     * initialised, it is ok to release the flock. Any other process will
     * check and see that the pidfile refers to a live process, and refrain
     * from deleting it. */

    ABORT_IF(
        releaseLockPidFile(aPidFile),
        {
            terminate(
                errno,
                "Cannot unlock pid file '%s'", aPidFile->mPathName.mFileName);
        });

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
reapSentry_(void *self_)
{
    struct Sentry *self = self_;

    ensure(sentryType_ == self->mType);

    struct Pid umbilicalPid =
        self->mUmbilicalProcess ? self->mUmbilicalProcess->mPid : Pid(0);

    superviseChildProcess(self->mChildProcess, umbilicalPid);
}

static void
raiseSentrySignal_(void *self_, int aSigNum)
{
    struct Sentry *self = self_;

    ensure(sentryType_ == self->mType);

    /* Propagate the signal to the child. Note that SIGQUIT might cause
     * the child to terminate and dump core. Dump core in sympathy if this
     * happens, but do that only if the child actually does so. This is
     * taken care of in reapFamily_(). */

    killChild(self->mChildProcess, aSigNum);
}

static void
raiseSentryStop_(void *self_)
{
    struct Sentry *self = self_;

    ensure(sentryType_ == self->mType);

    pauseChildProcessGroup(self->mChildProcess);
}

static void
raiseSentryResume_(void *self_)
{
    struct Sentry *self = self_;

    ensure(sentryType_ == self->mType);

    resumeChildProcessGroup(self->mChildProcess);
}

static void
raiseSentrySigCont_(void *self_)
{
    struct Sentry *self = self_;

    ensure(sentryType_ == self->mType);

    raiseChildSigCont(self->mChildProcess);
}

/* -------------------------------------------------------------------------- */
int
createSentry(struct Sentry *self,
             char         **aCmd)
{
    int rc = -1;

    self->mType = sentryType_;

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
                            VoidMethod(reapSentry_, self)));

    ERROR_IF(
        createBellSocketPair(&self->mSyncSocket_, 0));
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
        watchJobControlSignals(self->mJobControl,
                               VoidIntMethod(raiseSentrySignal_, self)),
        {
            terminate(
                errno,
                "Unable to add watch on signals");
        });

    ABORT_IF(
        watchJobControlStop(self->mJobControl,
                            VoidMethod(raiseSentryStop_, self),
                            VoidMethod(raiseSentryResume_, self)),
        {
            terminate(
                errno,
                "Unable to add watch on process stop");
        });

    ABORT_IF(
        watchJobControlContinue(self->mJobControl,
                                VoidMethod(raiseSentrySigCont_, self)),
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

    closeChildTether(self->mChildProcess);

    ABORT_IF(
        purgeProcessOrphanedFds(),
        {
            terminate(
                errno,
                "Unable to purge orphaned files");
        });

    /* Attempt to create the pidfile, if required, before creating the
     * umbilical process because it is quite possible for the attempt
     * to create the file to fail, and it is simpler to avoid having
     * clean up the umbilical process. */

    if (self->mPidFile)
    {
        ERROR_IF(
            announceChild_(
                self->mPidFile,
                self->mChildProcess->mPid,
                self->mPidServer->mSocketAddr));
    }

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

    /* The PidServer instance will continue to run in the umbilical process,
       so the instance that was created in the watchdog is no longer
       required. */

    closePidServer(self->mPidServer);
    self->mPidServer = 0;

    if (gOptions.mIdentify)
    {
        TEST_RACE
        ({
            ABORT_IF(
                -1 == dprintf(STDOUT_FILENO,
                              "%" PRId_Pid " "
                              "%" PRId_Pid "\n",
                              FMTd_Pid(ownProcessId()),
                              FMTd_Pid(self->mUmbilicalProcess->mPid)),
                {
                    terminate(
                        errno,
                        "Unable to print parent and umbilical pid");
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
                    "Unable to activate child process");
            });

        /* Now wait for the child to respond to know that it has
         * received the indication that it can start running. */

        ABORT_IF(
            waitBellSocketPairParent(self->mSyncSocket, 0) && EPIPE != errno,
            {
                terminate(
                    errno,
                    "Unable synchronise child process");
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

    closeBellSocketPair(self->mSyncSocket);
    self->mSyncSocket = 0;

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

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            closePidServer(self->mPidServer);
            destroyPidFile(self->mPidFile);
            closeBellSocketPair(self->mSyncSocket);
            closeJobControl(self->mJobControl);
            closeChild(self->mChildProcess);
            closeSocketPair(self->mUmbilicalSocket);
            closeStdFdFiller(self->mStdFdFiller);

            self->mType = 0;
        }
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

        self->mType = 0;
    }
}

/* -------------------------------------------------------------------------- */
int
runSentry(struct Sentry   *self,
          struct ExitCode *aExitCode)
{
    int rc = -1;

    /* Monitor the running child until it has either completed of
     * its own accord, or terminated. Once the child has stopped
     * running, release the pid file if one was allocated. */

    ABORT_IF(
        monitorChild(self->mChildProcess,
                     self->mUmbilicalProcess,
                     self->mUmbilicalSocket->mParentFile),
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
            acquireWriteLockPidFile(self->mPidFile),
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

    debug(0,
          "stopping umbilical pid %" PRId_Pid,
          FMTd_Pid(self->mUmbilicalProcess->mPid));

    int notStopped;
    ABORT_IF(
        (notStopped = stopUmbilicalProcess(self->mUmbilicalProcess),
         notStopped && ETIMEDOUT != errno),
        {
            terminate(errno, "Unable to stop umbilical process");
        });

    if (notStopped)
        warn(0, "Umable to stop umbilical process cleanly");

    /* The child process group is cleaned up from both the umbilical process
     * and the watchdog with the expectation that at least one of them
     * will succeed. At this point, the child process has already terminated
     * so killing the child process group will not change its exit
     * status. */

    killChildProcessGroup(self->mChildProcess);

    /* Reap the child only after the pid file is released. This ensures
     * that any competing reader that manages to sucessfully lock and
     * read the pid file will see the terminated process. */

    debug(0,
          "reaping child pid %" PRId_Pid,
          FMTd_Pid(self->mChildProcess->mPid));

    struct Pid childPid = self->mChildProcess->mPid;

    int childStatus;
    reapChild(self->mChildProcess, &childStatus);

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

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
