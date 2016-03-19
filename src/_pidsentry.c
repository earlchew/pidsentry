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

#include "_pidsentry.h"

#include "child.h"
#include "umbilical.h"
#include "keeper.h"
#include "command.h"

#include "env_.h"
#include "macros_.h"
#include "pipe_.h"
#include "socketpair_.h"
#include "unixsocket_.h"
#include "stdfdfiller_.h"
#include "pidfile_.h"
#include "thread_.h"
#include "error_.h"
#include "pollfd_.h"
#include "test_.h"
#include "fd_.h"
#include "dl_.h"
#include "type_.h"
#include "process_.h"

#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/un.h>

/* TODO
 *
 * Use struct Type for other poll loops
 * On receiving SIGABRT, trigger gdb
 * Dump /proc/../task/stack after SIGSTOP, just before delivering SIGABRT
 * -c command test:
 *    Check for obsolete pidfile
 *    Check for file descriptors in command process
 * Write unit test for forkProcessDaemon()
 */

/* -------------------------------------------------------------------------- */
static struct PidFile *
announceChild(struct PidFile           *aPidFile,
              struct Pid                aPid,
              const struct sockaddr_un *aPidKeeperAddr)
{
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

        ABORT_IF(
            openPidFile(aPidFile, O_CLOEXEC | O_CREAT),
            {
                terminate(
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
        writePidFile(aPidFile, aPid, aPidKeeperAddr),
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

    return aPidFile;
}

/* -------------------------------------------------------------------------- */
static struct ExitCode
cmdRunCommand(const char *aPidFileName, char **aCmd)
{
    struct ExitCode exitCode = { EXIT_FAILURE };

    struct Command  command_;
    struct Command *command = 0;
    ERROR_IF(
        createCommand(&command_, aPidFileName));
    command = &command_;

    ERROR_IF(
        runCommand(command, aCmd));

    ERROR_IF(
        reapCommand(command, &exitCode));

Finally:

    FINALLY
    ({
        closeCommand(command);
    });

    return exitCode;
}

/* -------------------------------------------------------------------------- */
static const struct Type * const familyType_ = TYPE("Family");

struct Family
{
    const struct Type *mType;

    struct ChildProcess *mChildProcess;
    struct Pid           mUmbilicalPid;
};

static void
reapFamily_(void *self_)
{
    struct Family *self = self_;

    ensure(familyType_ == self->mType);

    superviseChildProcess(self->mChildProcess, self->mUmbilicalPid);
}

static void
raiseFamilySignal_(void *self_, int aSigNum)
{
    struct Family *self = self_;

    ensure(familyType_ == self->mType);

    return killChild(self->mChildProcess, aSigNum);
}

static void
raiseFamilySigStop_(void *self_)
{
    struct Family *self = self_;

    ensure(familyType_ == self->mType);

    pauseChildProcessGroup(self->mChildProcess);

    ABORT_IF(
        raise(SIGSTOP),
        {
            terminate(
                errno,
                "Unable to stop process pid %" PRId_Pid,
                FMTd_Pid(ownProcessId()));
        });

    resumeChildProcessGroup(self->mChildProcess);
}

static void
raiseFamilySigCont_(void *self_)
{
    struct Family *self = self_;

    ensure(familyType_ == self->mType);

    raiseChildSigCont(self->mChildProcess);
}

static struct ExitCode
cmdMonitorChild(char **aCmd)
{
    ensure(aCmd);

    debug(0,
          "watchdog process pid %" PRId_Pid " pgid %" PRId_Pgid,
          FMTd_Pid(ownProcessId()),
          FMTd_Pgid(ownProcessGroupId()));

    ABORT_IF(
        ignoreProcessSigPipe(),
        {
            terminate(
                errno,
                "Unable to ignore SIGPIPE");
        });

    /* The instance of the StdFdFiller guarantees that any further file
     * descriptors that are opened will not be mistaken for stdin,
     * stdout or stderr. */

    struct StdFdFiller stdFdFiller;

    ABORT_IF(
        createStdFdFiller(&stdFdFiller),
        {
            terminate(
                errno,
                "Unable to create stdin, stdout, stderr filler");
        });

    struct SocketPair umbilicalSocket;
    ABORT_IF(
        createSocketPair(&umbilicalSocket, O_NONBLOCK | O_CLOEXEC),
        {
            terminate(
                errno,
                "Unable to create umbilical socket");
        });

    struct ChildProcess childProcess;
    ABORT_IF(
        createChild(&childProcess),
        {
            terminate(
                errno,
                "Unable to create child process");
        });

    struct Family family =
    {
        .mType         = familyType_,
        .mChildProcess = &childProcess,
        .mUmbilicalPid = Pid(0)
    };

    ABORT_IF(
        watchProcessChildren(VoidMethod(reapFamily_, &family)),
        {
            terminate(
                errno,
                "Unable to add watch on process termination");
        });

    struct SocketPair syncSocket;
    ABORT_IF(
        createSocketPair(&syncSocket, 0),
        {
            terminate(
                errno,
                "Unable to create sync socket");
        });

    ABORT_IF(
        forkChild(
            &childProcess, aCmd, &stdFdFiller, &syncSocket, &umbilicalSocket),
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
        watchProcessSignals(VoidIntMethod(raiseFamilySignal_, &family)),
        {
            terminate(
                errno,
                "Unable to add watch on signals");
        });

    ABORT_IF(
        watchProcessSigStop(VoidMethod(raiseFamilySigStop_, &family)),
        {
            terminate(
                errno,
                "Unable to add watch on process stop");
        });

    ABORT_IF(
        watchProcessSigCont(VoidMethod(raiseFamilySigCont_, &family)),
        {
            terminate(
                errno,
                "Unable to add watch on process continuation");
        });

    /* If a pidfile is required, create it now so that it can
     * be anchored to its directory before changing the current
     * working directory. Note that the pidfile might reside in
     * the current directory. */

    struct PidFile  pidFile_;
    struct PidFile *pidFile = 0;

    if (gOptions.mPidFile)
    {
        ABORT_IF(
            initPidFile(&pidFile_, gOptions.mPidFile),
            {
                terminate(
                    errno,
                    "Cannot initialise pid file '%s'", gOptions.mPidFile);
            });
        pidFile = &pidFile_;
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

    /* Before announcing the chld process, prepare the keeper process
     * that will hold a reference to the child pid, preventing
     * that pid from being re-used and alised to another process. */

    struct KeeperProcess  pidKeeperProcess_;
    struct KeeperProcess *pidKeeperProcess = 0;

    struct SocketPair  pidKeeperTether_;
    struct SocketPair *pidKeeperTether = 0;

    struct sockaddr_un pidKeeperAddr;

    if (pidFile)
    {
        ABORT_IF(
            createKeeperProcess(&pidKeeperProcess_, childProcess.mPgid),
            {
                terminate(
                    errno,
                    "Unable to create child process keeper");
            });
        pidKeeperProcess = &pidKeeperProcess_;

        ABORT_IF(
            createSocketPair(&pidKeeperTether_, O_NONBLOCK | O_CLOEXEC),
            {
                terminate(
                    errno,
                    "Unable to create child process keeper tether");
            });
        pidKeeperTether = &pidKeeperTether_;

        {
            struct UnixSocket pidKeeperSocket;
            ABORT_IF(
                createUnixSocket(&pidKeeperSocket, 0, 0, 0),
                {
                    terminate(
                        errno,
                        "Unable to create process keeper socket");
                });

            ABORT_IF(
                ownUnixSocketName(&pidKeeperSocket, &pidKeeperAddr));

            ABORT_IF(
                pidKeeperAddr.sun_path[0]);

            ABORT_IF(
                forkKeeperProcess(
                    pidKeeperProcess, pidKeeperTether, &pidKeeperSocket),
                {
                    terminate(
                        errno,
                        "Unable to create run process keeper");
                });

            closeUnixSocket(&pidKeeperSocket);
        }

        closeSocketPairChild(pidKeeperTether);

        /* Only identify the watchdog process after all the signal handlers
         * have been installed. The functional tests can use this as an
         * indicator that the watchdog is ready to run the child process.
         *
         * Although the watchdog process can be announed at this point,
         * the announcement is deferred so that it and the umbilical can
         * be announced in a single line at one point. */

        announceChild(pidFile, childProcess.mPid, &pidKeeperAddr);
    }

    /* With the child process launched, close the instance of StdFdFiller
     * so that stdin, stdout and stderr become available for manipulation
     * and will not be closed multiple times. */

    closeStdFdFiller(&stdFdFiller);

    /* Discard the original stdin file descriptor, and instead attach
     * the reading end of the tether as stdin. This means that the
     * watchdog does not contribute any more references to the
     * original stdin file table entry. */

    ABORT_IF(
        STDIN_FILENO != dup2(
            childProcess.mTetherPipe->mRdFile->mFd, STDIN_FILENO),
        {
            terminate(
                errno,
                "Unable to dup tether pipe to stdin");
        });

    /* Now that the tether has been duplicated onto stdin and stdout
     * as required, it is important to close the tether to ensure that
     * the only possible references to the tether pipe remain in the
     * child process, if required, and stdin and stdout in this process. */

    closeChildTether(&childProcess);

    ABORT_IF(
        purgeProcessOrphanedFds(),
        {
            terminate(
                errno,
                "Unable to purge orphaned files");
        });

    /* Monitor the umbilical using another process so that a failure
     * of this process can be detected independently. Only create the
     * monitoring process after all the file descriptors have been
     * purged so that the monitor does not inadvertently hold file
     * descriptors that should only be held by the child. */

    struct UmbilicalProcess umbilicalProcess;
    ABORT_IF(
        createUmbilicalProcess(&umbilicalProcess,
                               &childProcess,
                               &umbilicalSocket,
                               &syncSocket),
        {
            terminate(
                errno,
                "Unable to create umbilical process");
        });

    family.mUmbilicalPid = umbilicalProcess.mPid;

    closeSocketPairChild(&umbilicalSocket);

    if (gOptions.mIdentify)
    {
        TEST_RACE
        ({
            ABORT_IF(
                -1 == dprintf(STDOUT_FILENO,
                              "%" PRId_Pid " "
                              "%" PRId_Pid "\n",
                              FMTd_Pid(ownProcessId()),
                              FMTd_Pid(umbilicalProcess.mPid)),
                {
                    terminate(
                        errno,
                        "Unable to print parent and umbilical pid");
                });
        });
    }

    /* With the child process announced, and the umbilical monitor
     * prepared, allow the child process to run the target program. */

    closeSocketPairChild(&syncSocket);

    TEST_RACE
    ({

        /* The child process is waiting so that the chlid program will
         * run only after the pidfile has been created.
         *
         * Be aware that the supervisor might have sent a signal to the
         * watchdog which will have propagated it to the child, causing
         * the child to terminate. */

        char buf[1] = { 0 };

        ssize_t wrlen;
        ABORT_IF(
            (wrlen = writeFile(syncSocket.mParentFile, buf, 1),
             -1 == wrlen
             ? EPIPE != errno
             : (errno = 0, 1 != wrlen)),
            {
                terminate(
                    errno,
                    "Unable to activate child process");
            });

        /* Now wait for the child to respond to know that it has
         * received the indication that it can start running. */

        ssize_t rdlen;
        ABORT_IF(
            (rdlen = readFile(syncSocket.mParentFile, buf, 1),
             -1 == rdlen
             ? ECONNRESET != errno
             : (errno = 0, 1 != rdlen)),
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
                              FMTd_Pid(childProcess.mPid)),
                {
                    terminate(
                        errno,
                        "Unable to print child pid");
                });
        });
    }

    closeSocketPair(&syncSocket);

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

    /* Monitor the running child until it has either completed of
     * its own accord, or terminated. Once the child has stopped
     * running, release the pid file if one was allocated. */

    ABORT_IF(
        monitorChild(&childProcess,
                     &umbilicalProcess,
                     umbilicalSocket.mParentFile),
        {
            terminate(
                errno,
                "Unable to monitor child process");
        });

    ABORT_IF(
        unwatchProcessSigCont(),
        {
            terminate(
                errno,
                "Unable to remove watch from process continuation");
        });

    ABORT_IF(
        unwatchProcessSignals(),
        {
            terminate(
                errno,
                "Unable to remove watch from signals");
        });

    ABORT_IF(
        unwatchProcessChildren(),
        {
            terminate(
                errno,
                "Unable to remove watch on child process termination");
        });

    if (pidFile)
    {
        ABORT_IF(
            acquireWriteLockPidFile(pidFile),
            {
                terminate(
                    errno,
                    "Cannot lock pid file '%s'", pidFile->mPathName.mFileName);
            });

        destroyPidFile(pidFile);
        pidFile = 0;
    }

    /* Attempt to stop the umbilical process cleanly so that the watchdog
     * can exit in an orderly fashion with the exit status of the child
     * process as the last line emitted. */

    debug(0,
          "stopping umbilical pid %" PRId_Pid,
          FMTd_Pid(umbilicalProcess.mPid));

    int notStopped;
    ABORT_IF(
        (notStopped = stopUmbilicalProcess(&umbilicalProcess),
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

    killChildProcessGroup(&childProcess);

    /* Before reaping the child process, clean up the child process keeper.
     * The keeper process itself will persist until the last reference
     * is dropped. */

    closeSocketPair(pidKeeperTether);
    closeKeeperProcess(pidKeeperProcess);

    /* Reap the child only after the pid file is released. This ensures
     * that any competing reader that manages to sucessfully lock and
     * read the pid file will see the terminated process. */

    debug(0,
          "reaping child pid %" PRId_Pid,
          FMTd_Pid(childProcess.mPid));

    struct Pid childPid = childProcess.mPid;

    int childStatus;
    ABORT_IF(
        reapChild(&childProcess, &childStatus),
        {
            terminate(
                errno,
                "Unable to reap child pid %" PRId_Pid,
                FMTd_Pid(childProcess.mPid));
        });

    closeChild(&childProcess);

    debug(0,
          "reaped child pid %" PRId_Pid " status %d",
          FMTd_Pid(childPid),
          childStatus);

    closeSocketPair(&umbilicalSocket);

    ABORT_IF(
        resetProcessSigPipe(),
        {
            terminate(
                errno,
                "Unable to reset SIGPIPE");
        });

    return extractProcessExitStatus(childStatus, childPid);
}

/* -------------------------------------------------------------------------- */
int
main(int argc, char **argv)
{
    ABORT_IF(
        Test_init("PIDSENTRY_TEST_ERROR"),
        {
            terminate(
                0,
                "Unable to initialise test module");
        });

    ABORT_IF(
        Timekeeping_init(),
        {
            terminate(
                0,
                "Unable to initialise timekeeping module");
        });

    ABORT_IF(
        Process_init(argv[0]),
        {
            terminate(
                errno,
                "Unable to initialise process state");
        });

    struct ExitCode exitCode = { EXIT_FAILURE };

    {
        char **args;
        ERROR_IF(
            processOptions(argc, argv, &args),
            {
                if (EINVAL != errno)
                    message(errno,
                            "Unable to parse command line");
            });

        if (gOptions.mCommand)
            exitCode = cmdRunCommand(gOptions.mPidFile, args);
        else
            exitCode = cmdMonitorChild(args);
    }

Finally:

    Process_exit();
    Timekeeping_exit();

    if (testMode(TestLevelError))
        dprintf(STDERR_FILENO, "%" PRIu64 "\n", testErrorLevel());
    Test_exit();

    return exitCode.mStatus;
}

/* -------------------------------------------------------------------------- */
