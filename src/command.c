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

#include "error_.h"
#include "pipe_.h"
#include "env_.h"
#include "pidfile_.h"
#include "timescale_.h"
#include "process_.h"

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

#include <sys/un.h>

/* -------------------------------------------------------------------------- */
int
createCommand(struct Command *self,
              const char     *aPidFileName)
{
    int rc = -1;

    self->mPid          = Pid(0);
    self->mKeeperTether = 0;

    struct PidFile  pidFile_;
    struct PidFile *pidFile = 0;

    ERROR_IF(
        initPidFile(&pidFile_, aPidFileName),
        {
            warn(errno,
                 "Unable to find pid file '%s'", aPidFileName);
        });
    pidFile = &pidFile_;

    int err;
    ERROR_IF(
        (err = openPidFile(&pidFile_, O_CLOEXEC),
         err && ENOENT != errno),
        {
            warn(errno,
                "Unable to open pid file '%s'", aPidFileName);
        });

    ERROR_IF(
        err,
        {
            errno = ECHILD;
        });

    ERROR_IF(
        acquireReadLockPidFile(pidFile),
        {
            warn(errno,
                 "Unable to acquire read lock on pid file '%s'",
                 aPidFileName);
        });

    struct sockaddr_un pidKeeperAddr;
    struct Pid         pid;
    ERROR_IF(
        (pid = readPidFile(pidFile, &pidKeeperAddr),
         -1 == pid.mPid),
        {
            warn(errno,
                 "Unable to read pid file '%s'",
                 aPidFileName);
        });

    ERROR_UNLESS(
        pid.mPid,
        {
            errno = ECHILD;
        });

    /* Obtain a reference to the child process group, and do not proceed
     * until a positive acknowledgement is received to indicate that
     * the remote keeper has provided a stable reference. */

    ERROR_IF(
        (err = connectUnixSocket(&self->mKeeperTether_,
                                 pidKeeperAddr.sun_path,
                                 sizeof(pidKeeperAddr.sun_path)),
         -1 == err && EINPROGRESS != errno));
    self->mKeeperTether = &self->mKeeperTether_;

    ERROR_IF(
        (err = waitUnixSocketWriteReady(self->mKeeperTether, 0),
         -1 == err));

    ERROR_UNLESS(
        err,
        {
            errno = ENOTCONN;
        });

    ERROR_IF(
        (err = waitUnixSocketReadReady(self->mKeeperTether, 0),
         -1 == err));

    char buf[1];
    ERROR_IF(
        (err = readFile(self->mKeeperTether->mFile, buf, 1),
         -1 == err || (errno = EIO, 1 != err)));

    self->mChildPid = pid;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            closeUnixSocket(self->mKeeperTether);

        /* There is no further need to hold a lock on the pidfile because
         * acquisition of a reference to the child process group is the
         * sole requirement. */

        destroyPidFile(pidFile);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
runCommand(struct Command *self,
           char           **aCmd)

{
    int rc = -1;

    struct Pid pid = Pid(-1);

    struct Pipe  syncPipe_;
    struct Pipe *syncPipe = 0;

    ERROR_IF(
        createPipe(&syncPipe_, O_CLOEXEC));
    syncPipe = &syncPipe_;

    ERROR_IF(
        (pid = forkProcessChild(ForkProcessShareProcessGroup, Pgid(0)),
         -1 == pid.mPid));

    if (pid.mPid)
        self->mPid = pid;
    else
    {
        self->mPid = ownProcessId();

        debug(0,
              "starting command process pid %" PRId_Pid,
              FMTd_Pid(self->mPid));

        /* Populate the environment of the command process to
         * provide the attributes of the monitored process. */

        const char *watchdogChildPid;
        ABORT_UNLESS(
            (watchdogChildPid = setEnvPid(
                "PIDSENTRY_CHILD_PID", self->mChildPid)),
            {
                terminate(
                    errno,
                    "Unable to set PIDSENTRY_CHILD_PID %" PRId_Pid,
                    FMTd_Pid(self->mChildPid));
            });
        debug(0, "PIDSENTRY_CHILD_PID=%s", watchdogChildPid);

        /* Wait here until the parent process has completed its
         * initialisation, and sends a positive acknowledgement. */

        closePipeWriter(syncPipe);

        char buf[1];

        ssize_t rdlen;
        ABORT_IF(
            (rdlen = readFile(syncPipe->mRdFile, buf, 1),
             -1 == rdlen),
            {
                terminate(
                    errno,
                    "Unable to synchronise");
            });

        closePipe(syncPipe);
        syncPipe = 0;

        debug(0, "command process synchronised");

        /* Exit in sympathy if the parent process did not indicate
         * that it intialised successfully. Otherwise attempt to
         * execute the specified program. */

        if (1 == rdlen)
            ABORT_IF(
                execProcess(aCmd[0], aCmd) || (errno = 0, true),
                {
                    terminate(
                        errno,
                        "Unable to execute '%s'", aCmd[0]);
                });

        exitProcess(EXIT_FAILURE);
    }

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
            (wrlen = writeFile(syncPipe->mWrFile, buf, 1),
             -1 == wrlen || (errno = EIO, 1 != wrlen)));
    }

    self->mPid = pid;

    rc = 0;

Finally:

    FINALLY
    ({
        closePipe(syncPipe);
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

        struct Duration zeroDuration = Duration(NanoSeconds(0));

        int rdReady;
        ERROR_IF(
            (rdReady = waitFileReadReady(
                self->mKeeperTether->mFile, &zeroDuration),
             -1 == rdReady));

        if (rdReady)
            exitCode.mStatus = 255;
    }

    *aExitCode = exitCode;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closeCommand(struct Command *self)
{
    if (self)
    {
        closeUnixSocket(self->mKeeperTether);
    }
}

/* -------------------------------------------------------------------------- */
