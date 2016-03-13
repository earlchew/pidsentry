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

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

/* -------------------------------------------------------------------------- */
int
createCommand(struct Command *self,
              const char     *aPidFileName)
{
    int rc = -1;

    self->mChildPid = Pid(0);
    self->mPid      = Pid(0);
    self->mPgid     = Pgid(0);
    self->mPidFile  = 0;

    int err;
    ERROR_IF(
        (err = openPidFile(&self->mPidFile_, aPidFileName, O_CLOEXEC),
         err && ENOENT != errno),
        {
            warn(errno,
                "Unable to open pid file '%s'", aPidFileName);
        });
    self->mPidFile = &self->mPidFile_;

    if ( ! err)
    {
        ERROR_IF(
            acquireReadLockPidFile(self->mPidFile),
            {
                warn(errno,
                     "Unable to acquire read lock on pid file '%s'",
                     aPidFileName);
            });

        struct Pid pid;
        ERROR_IF(
            (pid = readPidFile(self->mPidFile),
             -1 == pid.mPid),
            {
                warn(errno,
                     "Unable to read pid file '%s'",
                     aPidFileName);
            });

        if (pid.mPid)
        {
            self->mChildPid = pid;
            self->mPgid     = Pgid(pid.mPid);

            rc = 0;
        }
    }

    if (rc)
        errno = ECHILD;

Finally:

    FINALLY({});

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
        (pid = forkProcess(ForkProcessSetProcessGroup, self->mPgid),
         -1 == pid.mPid));

    if ( ! pid.mPid)
    {
        self->mPid = ownProcessId();

        debug(0,
              "starting command process pid %" PRId_Pid,
              FMTd_Pid(pid));

        /* Populate the environment of the command process to
         * provide the attributes of the monitored process. */

        ABORT_UNLESS(
            setEnvPid("BLACKDOG_CHILD_PID", self->mChildPid),
            {
                terminate(
                    errno,
                    "Unable to set BLACKDOG_CHILD_PID %" PRId_Pid,
                    FMTd_Pid(self->mChildPid));
            });

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

        debug(0, "command process synchronised");

        if (1 == rdlen)
            ABORT_IF(
                execProcess(aCmd[0], aCmd) || (errno = 0, true),
                {
                    terminate(
                        errno,
                        "Unable to execute '%s'", aCmd[0]);
                });

        quitProcess(EXIT_FAILURE);
    }

    debug(0,
          "running command pid %" PRId_Pid " in pgid %" PRId_Pgid,
          FMTd_Pid(self->mPid),
          FMTd_Pgid(self->mPgid));

    ERROR_IF(
        purgeProcessOrphanedFds());

    /* Only send a positive acknowledgement to the command process
     * after initialisation is complete. */

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
        reapProcess(self->mPid, &status));

    *aExitCode = extractProcessExitStatus(status, self->mPid);

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
        closePidFile(self->mPidFile);
    }
}

/* -------------------------------------------------------------------------- */
