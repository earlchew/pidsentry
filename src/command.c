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

#include "command.h"
#include "shellcommand.h"

#include "error_.h"
#include "pipe_.h"
#include "env_.h"
#include "pidfile_.h"
#include "pidsignature_.h"
#include "timescale_.h"
#include "process_.h"

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <ctype.h>

#include <sys/un.h>

/* -------------------------------------------------------------------------- */
enum CommandStatus
createCommand(struct Command *self,
              const char     *aPidFileName)
{
    int rc = -1;

    enum CommandStatus status = CommandStatusOk;

    self->mPid          = Pid(0);
    self->mChildPid     = Pid(0);
    self->mKeeperTether = 0;

    struct PidSignature *pidSignature = 0;

    struct PidFile  pidFile_;
    struct PidFile *pidFile = 0;

    do
    {
        enum PathNameStatus pathNameStatus;
        ERROR_IF(
            (pathNameStatus = initPidFile(&pidFile_, aPidFileName),
             PathNameStatusError == pathNameStatus));

        if (PathNameStatusOk != pathNameStatus)
        {
            status = CommandStatusUnreachablePidFile;
            break;
        }
        pidFile = &pidFile_;

        struct Pid pid;
        ERROR_IF(
            (pid = openPidFile(pidFile, O_CLOEXEC),
             pid.mPid && ENOENT != errno && EACCES != errno));

        /* Run the command if the child process is running, or if
         * the force to run the command regardless of the state of the
         * child process. */

        if (pid.mPid)
        {
            status = CommandStatusInaccessiblePidFile;
            break;
        }

        ERROR_IF(
            acquirePidFileReadLock(pidFile));

        struct sockaddr_un pidKeeperAddr;
        ERROR_UNLESS(
            pidSignature = readPidFile(pidFile, &pidKeeperAddr));

        if ( ! pidSignature->mPid.mPid)
        {
            status = CommandStatusZombiePidFile;
            break;
        }

        /* If the pid file can be read and an authentic pid extracted,
         * that pid will remain viable because the sentry will not
         * reap the child process unless it can acquire a lock on
         * the same pid file.
         *
         * Obtain a reference to the child process group, and do not proceed
         * until a positive acknowledgement is received to indicate that
         * the remote keeper has provided a stable reference.
         *
         * Note that there is a window here between checking the content
         * of the pid file, and connecting to the name pid server, that
         * allows for a race where the pid server is replaced by another
         * program servicing the same connection address. */

        int err;
        ERROR_IF(
            (err = connectUnixSocket(&self->mKeeperTether_,
                                     pidKeeperAddr.sun_path,
                                     sizeof(pidKeeperAddr.sun_path)),
             -1 == err && EINPROGRESS != errno));
        self->mKeeperTether = &self->mKeeperTether_;

        ERROR_IF(
            (err = waitUnixSocketWriteReady(self->mKeeperTether, 0),
             -1 == err || (errno = 0, ! err)));

        /* In case a connection race occurs, send the pid signature
         * to allow the pid server to verify that it is serving a valid
         * client. */

        ERROR_IF(
            sendPidSignature(self->mKeeperTether->mFile, pidSignature, 0));

        ERROR_IF(
            (err = waitUnixSocketReadReady(self->mKeeperTether, 0),
             -1 == err));

        char buf[1];
        ERROR_IF(
            (err = readFile(self->mKeeperTether->mFile, buf, sizeof(buf), 0),
             -1 == err || (errno = 0, 1 != err)));

        self->mChildPid = pidSignature->mPid;

    } while (0);

    if (CommandStatusOk != status && gOptions.mForce)
    {
        self->mChildPid = Pid(0);
        status          = CommandStatusOk;
    }

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            status = CommandStatusError;

        if (CommandStatusOk != status || ! self->mChildPid.mPid)
            self->mKeeperTether = closeUnixSocket(self->mKeeperTether);

        /* There is no further need to hold a lock on the pidfile because
         * acquisition of a reference to the child process group is the
         * sole requirement. */

        pidFile = destroyPidFile(pidFile);

        pidSignature = destroyPidSignature(pidSignature);
    });

    return status;
}

/* -------------------------------------------------------------------------- */
struct RunCommandProcess_
{
    struct Command     *mCommand;
    struct Pipe        *mSyncPipe;
    const char * const *mCmd;
};

static CHECKED int
runCommandChildProcess_(struct RunCommandProcess_ *self)
{
    int rc = -1;

    struct ShellCommand  shellCommand_;
    struct ShellCommand *shellCommand = 0;

    self->mCommand->mPid = ownProcessId();

    debug(0,
          "starting command process pid %" PRId_Pid,
          FMTd_Pid(self->mCommand->mPid));

    /* Populate the environment of the command process to
     * provide the attributes of the monitored process. */

    const char *pidSentryPidEnv = "PIDSENTRY_PID";

    if ( ! self->mCommand->mChildPid.mPid)
        ERROR_IF(
            deleteEnv(pidSentryPidEnv) && ENOENT != errno);
    else
    {
        const char *watchdogChildPid;
        ERROR_UNLESS(
            (watchdogChildPid = setEnvPid(
                pidSentryPidEnv, self->mCommand->mChildPid)));

        debug(0, "%s=%s", pidSentryPidEnv, watchdogChildPid);
    }

    /* Wait here until the parent process has completed its
     * initialisation, and sends a positive acknowledgement. */

    closePipeWriter(self->mSyncPipe);

    char buf[1];

    ssize_t rdlen;
    ERROR_IF(
        (rdlen = readFile(self->mSyncPipe->mRdFile, buf, sizeof(buf), 0),
         -1 == rdlen));

    self->mSyncPipe = closePipe(self->mSyncPipe);

    debug(0, "command process synchronised");

    /* Exit in sympathy if the parent process did not indicate
     * that it intialised successfully. Otherwise attempt to
     * execute the specified program. */

    if (1 == rdlen)
    {
        ERROR_IF(
            createShellCommand(&shellCommand_, self->mCmd));
        shellCommand = &shellCommand_;

        execShellCommand(shellCommand);

        warn(errno,
             "Unable to execute '%s'", ownShellCommandText(shellCommand));
    }

    rc = EXIT_FAILURE;

Finally:

    FINALLY
    ({
        shellCommand = closeShellCommand(shellCommand);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
runCommand(struct Command     *self,
           const char * const *aCmd)

{
    int rc = -1;

    struct Pid pid = Pid(-1);

    struct Pipe  syncPipe_;
    struct Pipe *syncPipe = 0;

    ERROR_IF(
        createPipe(&syncPipe_, O_CLOEXEC));
    syncPipe = &syncPipe_;

    struct RunCommandProcess_ commandProcess =
    {
        .mCommand  = self,
        .mSyncPipe = syncPipe,
        .mCmd      = aCmd,
    };

    ERROR_IF(
        (pid = forkProcessChild(
            ForkProcessInheritProcessGroup,
            Pgid(0),
            ForkProcessMethod(runCommandChildProcess_, &commandProcess)),
         -1 == pid.mPid));

    self->mPid = pid;

    debug(0,
          "running command pid %" PRId_Pid,
          FMTd_Pid(self->mPid));

    ERROR_IF(
        purgeProcessOrphanedFds());

    /* Only send a positive acknowledgement to the command process
     * after initialisation is successful. If initialisation fails,
     * the command process will terminate sympathetically. */

    closePipeReader(syncPipe);

    {
        char buf[1] = { 0 };

        ssize_t wrlen;
        ERROR_IF(
            (wrlen = writeFile(syncPipe->mWrFile, buf, sizeof(buf), 0),
             -1 == wrlen || (errno = EIO, 1 != wrlen)));
    }

    rc = 0;

Finally:

    FINALLY
    ({
        syncPipe = closePipe(syncPipe);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
reapCommand(struct Command  *self,
            struct ExitCode *aExitCode)
{
    int rc = -1;

    int status;
    ERROR_IF(
        reapProcessChild(self->mPid, &status));

    struct ExitCode exitCode = extractProcessExitStatus(status, self->mPid);

    if (EXIT_SUCCESS == exitCode.mStatus)
    {
        /* Do not allow a positive result to mask the loss of the
         * reference to the child process group. */

        if (self->mKeeperTether)
        {
            int rdReady;
            ERROR_IF(
                (rdReady = waitFileReadReady(
                    self->mKeeperTether->mFile, &ZeroDuration),
                 -1 == rdReady));

            if (rdReady)
                exitCode.mStatus = 255;
        }
    }

    *aExitCode = exitCode;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
struct Command *
closeCommand(struct Command *self)
{
    if (self)
        self->mKeeperTether = closeUnixSocket(self->mKeeperTether);

    return 0;
}

/* -------------------------------------------------------------------------- */
