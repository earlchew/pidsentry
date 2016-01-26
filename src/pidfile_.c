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

#include "pidfile_.h"
#include "fd_.h"
#include "macros_.h"
#include "error_.h"
#include "test_.h"
#include "timekeeping_.h"
#include "system_.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/file.h>

/* -------------------------------------------------------------------------- */
static int
openPidFile_(struct PidFile *self, const char *aFileName)
{
    int rc = -1;

    self->mFile = 0;
    self->mLock = LOCK_UN;

    if (createPathName(&self->mPathName, aFileName))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
closePidFile_(struct PidFile *self)
{
    int rc = -1;

    if (closeFile(self->mFile))
        goto Finally;

    if (closePathName(&self->mPathName))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
lockPidFile_(struct PidFile *self, int aLock, const char *aLockType)
{
    int rc = -1;

    debug(0, "lock %s '%s'", aLockType, self->mPathName.mFileName);

    ensure(LOCK_UN != aLock);
    ensure(LOCK_UN == self->mLock);

    testSleep();

    if (lockFile(self->mFile, aLock))
        goto Finally;

    self->mLock = aLock;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
pid_t
readPidFile(const struct PidFile *self)
{
    int   rc     = -1;
    pid_t pid    = 0;
    FILE *pidfp  = 0;
    int   pidfd  = -1;
    char *bootid = 0;

    ensure(LOCK_UN != self->mLock);

    pidfd = dup(self->mFile->mFd);
    if (-1 == pidfd)
        goto Finally;

    pidfp = fdopen(pidfd, "r");
    if ( ! pidfp)
        goto Finally;
    pidfd = -1;

    uintmax_t pidvalue;
    if (2 != fscanf(pidfp, "%ju\n", &pidvalue))
    {
        errno = EIO;
        goto Finally;
    }

    pid = pidvalue;
    if (pid != pidvalue || 0 >= pid)
    {
        errno = ERANGE;
        goto Finally;
    }

    uint64_t pidfiletime_ns;
    if (2 != fscanf(pidfp, "%" SCNu64 "\n", &pidfiletime_ns))
    {
        errno = EIO;
        goto Finally;
    }

    if (1 != fscanf(pidfp, "%m[^\n]", &bootid))
    {
        errno = EIO;
        goto Finally;
    }

    if (ferror(pidfp))
    {
        errno = EIO;
        goto Finally;
    }

    do
    {
        const char *incarnation = fetchSystemIncarnation();

        debug(0, "pidfile boot id %s vs %s", bootid, incarnation);

        if ( ! strcmp(bootid, incarnation))
        {
            struct BootClockTime starttime;
            if (fetchProcessStartTime_(pid, &starttime))
            {
                if (ENOENT != errno)
                    goto Finally;
            }
            else
            {
                debug(0, "pidfile time %" PRIu64 " vs %" PRIu_NanoSeconds,
                      pidfiletime_ns,
                      starttime.bootclock.ns);

                if (starttime.bootclock.ns == pidfiletime_ns)
                    break;
            }
        }

        pid = 0;

    } while (0);

    rc = 0;

Finally:

    FINALLY
    ({
        if (pidfp)
            fclose(pidfp);

        closeFd(&pidfd);
        free(bootid);
    });

    return rc ? -1 : pid;
}

/* -------------------------------------------------------------------------- */
int
releaseLockPidFile(struct PidFile *self)
{
    int rc = -1;

    ensure(LOCK_UN != self->mLock);

    if (unlockFile(self->mFile))
        goto Finally;

    self->mLock = LOCK_UN;

    rc = 0;

Finally:

    FINALLY
    ({
        testSleep();
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
acquireWriteLockPidFile(struct PidFile *self)
{
    return lockPidFile_(self, LOCK_EX, "exclusive");
}

/* -------------------------------------------------------------------------- */
int
acquireReadLockPidFile(struct PidFile *self)
{
    return lockPidFile_(self, LOCK_SH, "shared");
}

/* -------------------------------------------------------------------------- */
int
createPidFile(struct PidFile *self, const char *aFileName)
{
    int rc = -1;

    if (openPidFile_(self, aFileName))
        goto Finally;

    /* Check if the pidfile already exists, and if the process that
     * it names is still running.
     */

    if (createFile(
            &self->mFile_,
            openPathName(&self->mPathName, O_RDONLY | O_NOFOLLOW, 0)))
    {
        if (ENOENT != errno)
            goto Finally;
    }
    else
    {
        self->mFile = &self->mFile_;

        if (acquireWriteLockPidFile(self))
            goto Finally;

        /* If the pidfile names a valid process then give up since
         * it means that the pidfile is already owned. Otherwise,
         * the pidfile is not owned, and can be deleted. */

        switch (readPidFile(self))
        {
        default:
            errno = EEXIST;
            goto Finally;
        case -1:
            goto Finally;
        case 0:
            break;
        }

        debug(
            0,
            "removing existing pidfile '%s'", self->mPathName.mFileName);

        if (unlinkPathName(&self->mPathName, 0))
        {
            if (ENOENT != errno)
                goto Finally;
        }

        if (releaseLockPidFile(self))
            goto Finally;

        if (closeFile(self->mFile))
            goto Finally;

        self->mFile = 0;
    }

    /* Open the pidfile using lock file semantics for writing, but
     * with readonly permissions. Use of lock file semantics ensures
     * that the watchdog will be the owner of the pid file, and
     * readonly permissions dissuades other processes from modifying
     * the content. */

    ensure( ! self->mFile);
    RACE
    ({
        if (createFile(
                &self->mFile_,
                openPathName(&self->mPathName,
                             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                             S_IRUSR | S_IRGRP | S_IROTH)))
            goto Finally;
    });
    self->mFile = &self->mFile_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            if (closePidFile_(self))
                warn(
                    errno,
                    "Unable to close pidfile '%s'", aFileName);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
openPidFile(struct PidFile *self, const char *aFileName)
{
    int rc = -1;

    if (openPidFile_(self, aFileName))
        goto Finally;

    if (createFile(
            &self->mFile_,
            openPathName(&self->mPathName, O_RDONLY | O_NOFOLLOW, 0)))
        goto Finally;
    self->mFile = &self->mFile_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            if (closePidFile_(self))
                warn(
                    errno,
                    "Unable to close pidfile '%s'", aFileName);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
detectPidFileZombie(const struct PidFile *self)
{
    int rc = -1;

    /* The pidfile has become a zombie if it was deleted, and either
     * no longer exists, or replaced by a different file in the
     * same directory.
     *
     * Return -1 on error, 0 if the pidfile is not a zombie, or 1 if
     * the pidfile is zombie.
     */

    struct stat fileStatus;

    if (fstatPathName(&self->mPathName, &fileStatus, AT_SYMLINK_NOFOLLOW))
    {
        if (ENOENT == errno)
            rc = 1;
        goto Finally;
    }

    struct stat fdStatus;

    if (fstatFile(self->mFile, &fdStatus))
        goto Finally;

    rc = ( fdStatus.st_dev != fileStatus.st_dev ||
           fdStatus.st_ino != fileStatus.st_ino );

Finally:

    FINALLY({});

    if ( ! rc && testAction())
        rc = 1;

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closePidFile(struct PidFile *self)
{
    int rc = -1;

    if (self)
    {
        ensure(LOCK_UN != self->mLock);

        int flags = fcntlFileGetFlags(self->mFile);

        if (-1 == flags)
            goto Finally;

        if (O_RDONLY != (flags & O_ACCMODE))
        {
            /* The pidfile is still locked at this point. If writable,
             * remove the content from the pidfile first so that any
             * competing reader will see any empty file. Once emptied,
             * remove the pidfile so that no other process will be
             * able to find the file. */

            if (ftruncateFile(self->mFile, 0))
                goto Finally;

            if (unlinkPathName(&self->mPathName, 0))
            {
                /* In theory, ENOENT should not occur since the pidfile
                 * is locked, and competing processes need to hold the
                 * lock to remove the pidfile. It might be possible
                 * that the pidfile is deleted from, say, the command
                 * line. */

                if (ENOENT != errno)
                    goto Finally;
            }
        }

        if (closeFile(self->mFile))
            goto Finally;

        if (closePathName(&self->mPathName))
            goto Finally;

        self->mFile = 0;
        self->mLock = LOCK_UN;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
writePidFile(struct PidFile *self, pid_t aPid)
{
    int rc = -1;

    ensure(0 < aPid);

    struct BootClockTime starttime;
    if (fetchProcessStartTime_(aPid, &starttime))
        goto Finally;

    if (0 > dprintf(self->mFile->mFd,
                    "%jd\n%s\n%" PRIu_NanoSeconds "\n",
                    (intmax_t) aPid,
                    fetchSystemIncarnation(),
                    starttime.bootclock.ns))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
