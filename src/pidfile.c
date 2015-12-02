/* -*- c-basic-offset:4; indent-tabs-mode:nil -*- vi: set sw=4 et: */
/*
// Copyright (c) 2015, Earl Chew
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

#include "pidfile.h"
#include "macros.h"
#include "error.h"
#include "test.h"
#include "process.h"
#include "timekeeping.h"

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
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
    debug(0, "lock %s '%s'", aLockType, self->mPathName.mFileName);

    ensure(LOCK_UN != aLock);
    ensure(LOCK_UN == self->mLock);

    testSleep();

    int rc = flock(self->mFile->mFd, aLock);

    if ( ! rc)
        self->mLock = aLock;

    return rc;
}

/* -------------------------------------------------------------------------- */
pid_t
readPidFile(const struct PidFile *self)
{
    pid_t pid;
    char  buf[sizeof(pid) * CHAR_BIT + sizeof("\n")];
    char *bufptr = buf;
    char *bufend = bufptr + sizeof(buf);

    ensure(LOCK_UN != self->mLock);

    while (true)
    {
        if (bufptr == bufend)
            return 0;

        ssize_t len;

        len = read(self->mFile->mFd, bufptr, bufend - bufptr);

        if (-1 == len)
        {
            if (EINTR == errno)
                continue;

            return -1;
        }

        if ( ! len)
        {
            bufptr[len] = '\n';
            ++len;
        }

        for (unsigned ix = 0; ix < len; ++ix)
        {
            if ('\n' == bufptr[ix])
            {
                /* Parse the value read from the pidfile, but take
                 * care that it is a valid number that can fit in the
                 * representation. */

                bufptr[ix] = 0;

                debug(0, "examining candidate pid '%s'", buf);

                if (parsePid(buf, &pid) || 0 >= pid)
                {
                    debug(0, "invalid pid representation");
                    return 0;
                }

                /* Find the name of the proc entry that corresponds
                 * to this pid, and use that to determine when
                 * the process was started. Compare that with
                 * the mtime of the pidfile to determine if
                 * the pid is viable. */

                struct stat fdStatus;

                if (fstat(self->mFile->mFd, &fdStatus))
                    return -1;

                struct timespec fdTime   = earliestTime(&fdStatus.st_mtim,
                                                        &fdStatus.st_ctim);
                struct timespec procTime = findProcessStartTime(pid);

                if (UTIME_OMIT == procTime.tv_nsec)
                {
                    return -1;
                }
                else if (UTIME_NOW == procTime.tv_nsec)
                {
                    debug(0, "process no longer exists");
                    return 0;
                }

                debug(0,
                    "pidfile mtime %jd.%09ld",
                    (intmax_t) fdTime.tv_sec, fdTime.tv_nsec);

                debug(0,
                    "process mtime %jd.%09ld",
                    (intmax_t) procTime.tv_sec, procTime.tv_nsec);

                if (procTime.tv_sec < fdTime.tv_sec)
                    return pid;

                if (procTime.tv_sec  == fdTime.tv_sec &&
                    procTime.tv_nsec <  fdTime.tv_nsec)
                {
                    return pid;
                }

                debug(0, "process was restarted");

                return 0;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
int
releaseLockPidFile(struct PidFile *self)
{
    ensure(LOCK_UN != self->mLock);

    int locked = self->mLock;

    self->mLock = LOCK_UN;

    int rc = flock(self->mFile->mFd, LOCK_UN);

    FINALLY
    ({
        if (rc)
            self->mLock = locked;
    });

    testSleep();

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
zombiePidFile(const struct PidFile *self)
{
    int rc = -1;

    /* The pidfile has become a zombie if it was deleted, and no longer
     * exists, or replaced by a different file different file in the
     * same directory. */

    struct stat fileStatus;

    if (fstatPathName(&self->mPathName, &fileStatus, AT_SYMLINK_NOFOLLOW))
    {
        if (ENOENT == errno)
            rc = 1;
        goto Finally;
    }

    struct stat fdStatus;

    if (fstat(self->mFile->mFd, &fdStatus))
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
    ensure(LOCK_UN != self->mLock);

    int rc = -1;

    int flags = fcntl(self->mFile->mFd, F_GETFL, 0);

    if (-1 == flags)
        goto Finally;

    if (O_RDONLY != (flags & O_ACCMODE))
    {
        /* The pidfile is still locked at this point. If writable,
         * remove the content from the pidfile first so that any
         * competing reader will see any empty file. Once emptied,
         * remove the pidfile so that no other process will be
         * able to find the file. */

        if (ftruncate(self->mFile->mFd, 0))
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

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
writePidFile(struct PidFile *self, pid_t aPid)
{
    ensure(0 < aPid);

    return 0 > dprintf(self->mFile->mFd, "%jd\n", (intmax_t) aPid) ? -1 : 0;
}

/* -------------------------------------------------------------------------- */
