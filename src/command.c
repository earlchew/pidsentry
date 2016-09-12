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
#include "fdset_.h"

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <ctype.h>

#include <sys/un.h>

/* -------------------------------------------------------------------------- */
struct Command *
closeCommand(struct Command *self)
{
    if (self)
        self->mKeeperTether = closeUnixSocket(self->mKeeperTether);

    return 0;
}

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

        if (pid.mPid)
        {
            switch (errno)
            {
            default:
                ensure(false);

            case ENOENT:
                status = CommandStatusNonexistentPidFile;
                break;

            case EACCES:
                status = CommandStatusInaccessiblePidFile;
                break;
            }
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

        if (-1 == pidSignature->mPid.mPid)
        {
            status = CommandStatusMalformedPidFile;
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
            sendPidSignature(
                self->mKeeperTether->mSocket->mFile, pidSignature, 0));

        ERROR_IF(
            (err = waitUnixSocketReadReady(self->mKeeperTether, 0),
             -1 == err));

        char buf[1];
        ERROR_IF(
            (err = readSocket(
                self->mKeeperTether->mSocket, buf, sizeof(buf), 0),
             -1 == err || (errno = 0, 1 != err)));

        self->mChildPid = pidSignature->mPid;

    } while (0);

    /* Run the command if the child process is running, or if
     * the force to run the command regardless of the state of the
     * child process. */

    if (CommandStatusNonexistentPidFile == status ||
        CommandStatusZombiePidFile      == status)
    {
        if (gOptions.mClient.mRelaxed)
        {
            self->mChildPid = Pid(0);
            status          = CommandStatusOk;
        }
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
struct CommandProcess_
{
    struct Command *mCommand;

    struct ShellCommand  mShellCommand_;
    struct ShellCommand *mShellCommand;
};

static CHECKED struct CommandProcess_ *
closeCommandProcess_(struct CommandProcess_ *self)
{
    if (self)
    {
        self->mShellCommand = closeShellCommand(self->mShellCommand);
    }

    return 0;
}

static CHECKED int
createCommandProcess_(struct CommandProcess_ *self,
                      struct Command         *aCommand,
                      const char * const     *aCmd)
{
    int rc = -1;

    self->mCommand      = aCommand;
    self->mShellCommand = 0;

    ERROR_IF(
        createShellCommand(&self->mShellCommand_, aCmd));
    self->mShellCommand = &self->mShellCommand_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            self = closeCommandProcess_(self);
    });

    return rc;
}

static CHECKED int
runCommandProcess_(struct CommandProcess_ *self)
{
    int rc = -1;

    execShellCommand(self->mShellCommand);

    warn(errno,
         "Unable to execute '%s'", ownShellCommandText(self->mShellCommand));

    rc = EXIT_FAILURE;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static CHECKED int
prepareCommandProcess_(struct CommandProcess_      *self,
                       const struct PreForkProcess *aPreFork)
{
    int rc = -1;

    ERROR_IF(
        insertFdSetRange(
            aPreFork->mWhitelistFds,
            FdRange(0, INT_MAX)));

    ERROR_IF(
        insertFdSetRange(
            aPreFork->mBlacklistFds,
            FdRange(0, INT_MAX)));

    if (self->mCommand->mKeeperTether)
        ERROR_IF(
            removeFdSetFile(
                aPreFork->mBlacklistFds,
                self->mCommand->mKeeperTether->mSocket->mFile));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static CHECKED int
postCommandProcessChild_(struct CommandProcess_ *self)
{
    int rc = -1;

    self->mCommand->mPid = ownProcessId();

    /* Do not allow the child process to retain a reference to the tether
     * to avoid giving it a chance to scribble into it. */

    self->mCommand->mKeeperTether = closeUnixSocket(
        self->mCommand->mKeeperTether);

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

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static CHECKED int
postCommandProcessParent_(struct CommandProcess_ *self,
                          struct Pid              aPid)
{
    int rc = -1;

    self->mCommand->mPid = aPid;

    debug(0,
          "running command pid %" PRId_Pid,
          FMTd_Pid(self->mCommand->mPid));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
runCommand(struct Command     *self,
           const char * const *aCmd)

{
    int rc = -1;

    struct CommandProcess_  commandProcess_;
    struct CommandProcess_ *commandProcess = 0;

    ERROR_IF(
        createCommandProcess_(&commandProcess_, self, aCmd));
    commandProcess = &commandProcess_;

    struct Pid pid;
    ERROR_IF(
        (pid = forkProcessChildX(
            ForkProcessInheritProcessGroup,
            Pgid(0),
            PreForkProcessMethod(
                commandProcess, prepareCommandProcess_),
            PostForkChildProcessMethod(
                commandProcess, postCommandProcessChild_),
            PostForkParentProcessMethod(
                commandProcess, postCommandProcessParent_),
            ForkProcessMethod(
                commandProcess, runCommandProcess_)),
         -1 == pid.mPid));

    rc = 0;

Finally:

    FINALLY
    ({
        commandProcess = closeCommandProcess_(commandProcess);
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
                (rdReady = waitSocketReadReady(
                    self->mKeeperTether->mSocket, &ZeroDuration),
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
