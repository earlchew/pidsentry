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

#include "_blackdog.h"

#include "child.h"
#include "umbilical.h"

#include "env_.h"
#include "macros_.h"
#include "pipe_.h"
#include "socketpair_.h"
#include "stdfdfiller_.h"
#include "pidfile_.h"
#include "thread_.h"
#include "error_.h"
#include "pollfd_.h"
#include "test_.h"
#include "fd_.h"
#include "dl_.h"
#include "type_.h"

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
 * cmdRunCommand() is too big, break it up
 * Add test case for SIGKILL of watchdog and child not watching tether
 * Fix vgcore being dropped everywhere
 * Implement process lock in child process
 * When announcing child, ensure pid is active first
 * Use struct Type for other poll loops
 * Propagate signal termination from child to parent
 * Improve FINALLY tracking
 * Use struct Pid for type safety
 * On receiving SIGABRT, trigger gdb
 * Isolate signal delivery to one thread
 */

/* -------------------------------------------------------------------------- */
static void
announceChild(pid_t aPid, struct PidFile *aPidFile, const char *aPidFileName)
{
    for (int zombie = -1; zombie; )
    {
        if (0 < zombie)
        {
            debug(0, "discarding zombie pid file '%s'", aPidFileName);

            if (closePidFile(aPidFile))
                terminate(
                    errno,
                    "Cannot close pid file '%s'", aPidFileName);
        }

        if (createPidFile(aPidFile, aPidFileName))
            terminate(
                errno,
                "Cannot create pid file '%s'", aPidFileName);

        /* It is not possible to create the pidfile and acquire a flock
         * as an atomic operation. The flock can only be acquired after
         * the pidfile exists. Since this newly created pidfile is empty,
         * it resembles an closed pidfile, and in the intervening time,
         * another process might have removed it and replaced it with
         * another. */

        if (acquireWriteLockPidFile(aPidFile))
            terminate(
                errno,
                "Cannot acquire write lock on pid file '%s'", aPidFileName);

        zombie = detectPidFileZombie(aPidFile);

        if (0 > zombie)
            terminate(
                errno,
                "Unable to obtain status of pid file '%s'", aPidFileName);
    }

    debug(0, "initialised pid file '%s'", aPidFileName);

    if (writePidFile(aPidFile, aPid))
        terminate(
            errno,
            "Cannot write to pid file '%s'", aPidFileName);

    /* The pidfile was locked on creation, and now that it
     * is completely initialised, it is ok to release
     * the flock. */

    if (releaseLockPidFile(aPidFile))
        terminate(
            errno,
            "Cannot unlock pid file '%s'", aPidFileName);
}

/* -------------------------------------------------------------------------- */
static struct ExitCode
cmdPrintPidFile(const char *aFileName)
{
    struct ExitCode exitCode = { 1 };

    struct PidFile pidFile;

    if (openPidFile(&pidFile, aFileName))
    {
        if (ENOENT != errno)
            terminate(
                errno,
                "Unable to open pid file '%s'", aFileName);
        return exitCode;
    }

    if (acquireReadLockPidFile(&pidFile))
        terminate(
            errno,
            "Unable to acquire read lock on pid file '%s'", aFileName);

    pid_t pid = readPidFile(&pidFile);

    switch (pid)
    {
    default:
        if (-1 != dprintf(STDOUT_FILENO, "%jd\n", (intmax_t) pid))
            exitCode.mStatus = 0;
        break;
    case 0:
        break;
    case -1:
        terminate(
            errno,
            "Unable to read pid file '%s'", aFileName);
    }

    FINALLY
    ({
        if (closePidFile(&pidFile))
            terminate(
                errno,
                "Unable to close pid file '%s'", aFileName);
    });

    return exitCode;
}

/* -------------------------------------------------------------------------- */
static const struct Type * const familyType_ = TYPE("Family");

struct Family
{
    const struct Type *mType;

    struct ChildProcess *mChildProcess;
    pid_t                mUmbilicalPid;
};

static void
reapFamily_(void *self_)
{
    struct Family *self = self_;

    ensure(familyType_ == self->mType);

    reapChild(self->mChildProcess, self->mUmbilicalPid);
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

    if (raise(SIGSTOP))
        terminate(
            errno,
            "Unable to stop process");

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
cmdRunCommand(char **aCmd)
{
    ensure(aCmd);

    debug(0,
          "watchdog process pid %jd pgid %jd",
          (intmax_t) getpid(),
          (intmax_t) getpgid(0));

    if (ignoreProcessSigPipe())
        terminate(
            errno,
            "Unable to ignore SIGPIPE");

    /* The instance of the StdFdFiller guarantees that any further file
     * descriptors that are opened will not be mistaken for stdin,
     * stdout or stderr. */

    struct StdFdFiller stdFdFiller;

    if (createStdFdFiller(&stdFdFiller))
        terminate(
            errno,
            "Unable to create stdin, stdout, stderr filler");

    struct SocketPair umbilicalSocket;
    if (createSocketPair(&umbilicalSocket, O_NONBLOCK | O_CLOEXEC))
        terminate(
            errno,
            "Unable to create umbilical socket");

    struct ChildProcess childProcess;
    createChild(&childProcess);

    struct Family family =
    {
        .mType         = familyType_,
        .mChildProcess = &childProcess,
        .mUmbilicalPid = 0
    };

    if (watchProcessChildren(VoidMethod(reapFamily_, &family)))
        terminate(
            errno,
            "Unable to add watch on process termination");

    struct SocketPair syncSocket;
    if (createSocketPair(&syncSocket, 0))
        terminate(
            errno,
            "Unable to create sync socket");

    if (forkChild(
            &childProcess, aCmd, &stdFdFiller, &syncSocket, &umbilicalSocket))
        terminate(
            errno,
            "Unable to fork child");

    /* Be prepared to deliver signals to the child process only after
     * the child exists. Before this point, these signals will cause
     * the watchdog to terminate, and the new child process will
     * notice via its synchronisation pipe. */

    if (watchProcessSignals(VoidIntMethod(raiseFamilySignal_, &family)))
        terminate(
            errno,
            "Unable to add watch on signals");

    if (watchProcessSigStop(VoidMethod(raiseFamilySigStop_, &family)))
        terminate(
            errno,
            "Unable to add watch on process stop");

    if (watchProcessSigCont(VoidMethod(raiseFamilySigCont_, &family)))
        terminate(
            errno,
            "Unable to add watch on process continuation");

    /* Only identify the watchdog process after all the signal handlers
     * have been installed. The functional tests can use this as an
     * indicator that the watchdog is ready to run the child process.
     *
     * Although the watchdog process can be announed at this point,
     * the announcement is deferred so that it and the umbilical can
     * be announced in a single line at one point. */

    struct PidFile  pidFile_;
    struct PidFile *pidFile = 0;

    if (gOptions.mPidFile)
    {
        const char *pidFileName = gOptions.mPidFile;

        pid_t pid = gOptions.mPid;

        switch (pid)
        {
        default:
            break;
        case -1:
            pid = getpid(); break;
        case 0:
            pid = childProcess.mPid; break;
        }

        pidFile = &pidFile_;

        announceChild(pid, pidFile, pidFileName);
    }

    /* With the child process launched, close the instance of StdFdFiller
     * so that stdin, stdout and stderr become available for manipulation
     * and will not be closed multiple times. */

    if (closeStdFdFiller(&stdFdFiller))
        terminate(
            errno,
            "Unable to close stdin, stdout and stderr fillers");

    /* Discard the original stdin file descriptor, and instead attach
     * the reading end of the tether as stdin. This means that the
     * watchdog does not contribute any more references to the
     * original stdin file table entry. */

    if (STDIN_FILENO != dup2(
            childProcess.mTetherPipe->mRdFile->mFd, STDIN_FILENO))
        terminate(
            errno,
            "Unable to dup tether pipe to stdin");

    /* Now that the tether has been duplicated onto stdin and stdout
     * as required, it is important to close the tether to ensure that
     * the only possible references to the tether pipe remain in the
     * child process, if required, and stdin and stdout in this process. */

    closeChildTether(&childProcess);

    if (purgeProcessOrphanedFds())
        terminate(
            errno,
            "Unable to purge orphaned files");

    /* Monitor the umbilical using another process so that a failure
     * of this process can be detected independently. Only create the
     * monitoring process after all the file descriptors have been
     * purged so that the monitor does not inadvertently hold file
     * descriptors that should only be held by the child. */

    struct UmbilicalProcess umbilicalProcess;
    if (createUmbilicalProcess(&umbilicalProcess,
                               &childProcess,
                               &umbilicalSocket,
                               &syncSocket,
                               pidFile))
        terminate(
            errno,
            "Unable to create umbilical process");

    family.mUmbilicalPid = umbilicalProcess.mPid;

    if (closeSocketPairChild(&umbilicalSocket))
        terminate(
            errno,
            "Unable to close umbilical child socket");

    if (gOptions.mIdentify)
        RACE
        ({
            if (-1 == dprintf(STDOUT_FILENO,
                              "%jd %jd\n",
                              (intmax_t) getpid(),
                              (intmax_t) umbilicalProcess.mPid))
                terminate(
                    errno,
                    "Unable to print parent and umbilical pid");
        });

    /* With the child process announced, and the umbilical monitor
     * prepared, allow the child process to run the target program.
     *
     * Wait until the child process acknowledges to avoid racing with
     * the child process initialisation. */

    if (closeSocketPairChild(&syncSocket))
        terminate(
            errno,
            "Unable to close child sync socket");

    RACE
    ({
        /* Be aware that the supervisor might have sent a signal to the
         * watchdog which will have propagated it to the child, causing
         * the child to terminate. */

        if (shutdownFileSocketWriter(syncSocket.mParentFile))
            terminate(
                errno,
                "Unable to activate child process");

        char buf[1];

        switch (readFile(syncSocket.mParentFile, buf, 1))
        {
        default:
            errno = 0;
            /* Fall through */

        case -1:
            if (ECONNRESET != errno)
                terminate(
                    errno,
                    "Unable synchronise child process");
            break;

        case 0:
            break;

        }
    });

    if (closeSocketPair(&syncSocket))
        terminate(
            errno,
            "Unable to close sync socket");

    if (gOptions.mIdentify)
    {
        RACE
        ({
            if (-1 == dprintf(STDOUT_FILENO,
                              "%jd\n", (intmax_t) childProcess.mPid))
                terminate(
                    errno,
                    "Unable to print child pid");
        });
    }

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
        switch (ownFdValid(STDOUT_FILENO))
        {
        default:
            break;

        case -1:
            terminate(
                errno,
                "Unable to check validity of stdout");

        case 0:
            discardStdout = true;
            break;
        }
    }

    if (discardStdout)
    {
        if (nullifyFd(STDOUT_FILENO))
            terminate(
                errno,
                "Unable to nullify stdout");
    }

    if (testMode(1))
    {
        if (raise(SIGSTOP))
            terminate(
                errno,
                "Unable to stop process");
    }

    /* Monitor the running child until it has either completed of
     * its own accord, or terminated. Once the child has stopped
     * running, release the pid file if one was allocated. */

    monitorChild(&childProcess,
                 &umbilicalProcess,
                 umbilicalSocket.mParentFile);

    if (unwatchProcessSigCont())
        terminate(
            errno,
            "Unable to remove watch from process continuation");

    if (unwatchProcessSignals())
        terminate(
            errno,
            "Unable to remove watch from signals");

    if (unwatchProcessChildren())
        terminate(
            errno,
            "Unable to remove watch on child process termination");

    if (pidFile)
    {
        if (acquireWriteLockPidFile(pidFile))
            terminate(
                errno,
                "Cannot lock pid file '%s'", pidFile->mPathName.mFileName);

        if (closePidFile(pidFile))
            terminate(
                errno,
                "Cannot close pid file '%s'", pidFile->mPathName.mFileName);

        pidFile = 0;
    }

    /* Attempt to stop the umbilical process cleanly so that the watchdog
     * can exit in an orderly fashion with the exit status of the child
     * process as the last line emitted. */

    debug(0, "stopping umbilical pid %jd", (intmax_t) umbilicalProcess.mPid);

    if (stopUmbilicalProcess(&umbilicalProcess))
        warn(errno, "Unable to stop umbilical process cleanly");

    /* The child process group is cleaned up from both the umbilical process
     * and the watchdog with the expectation that at least one of them
     * will succeed. At this point, the child process has already terminated
     * so killing the child process group will not change its exit
     * status. */

    killChildProcessGroup(&childProcess);

    /* Reap the child only after the pid file is released. This ensures
     * that any competing reader that manages to sucessfully lock and
     * read the pid file will see the terminated process. */

    debug(0, "reaping child pid %jd", (intmax_t) childProcess.mPid);

    pid_t childPid = childProcess.mPid;

    int status = closeChild(&childProcess);

    debug(0, "reaped child pid %jd status %d", (intmax_t) childPid, status);

    if (closeSocketPair(&umbilicalSocket))
        terminate(
            errno,
            "Unable to close umbilical socket");

    if (resetProcessSigPipe())
        terminate(
            errno,
            "Unable to reset SIGPIPE");

    return extractProcessExitStatus(status, childPid);
}

/* -------------------------------------------------------------------------- */
int
main(int argc, char **argv)
{
    if (Timekeeping_init())
        terminate(
            0,
            "Unable to initialise timekeeping module");

    if (Process_init(argv[0]))
        terminate(
            errno,
            "Unable to initialise process state");

    struct ExitCode exitCode;

    {
        char **cmd = processOptions(argc, argv);

        if ( ! cmd && gOptions.mPidFile)
            exitCode = cmdPrintPidFile(gOptions.mPidFile);
        else
            exitCode = cmdRunCommand(cmd);
    }

    if (Process_exit())
        terminate(
            errno,
            "Unable to finalise process state");

    if (Timekeeping_exit())
        terminate(
            0,
            "Unable to finalise timekeeping module");

    return exitCode.mStatus;
}

/* -------------------------------------------------------------------------- */
