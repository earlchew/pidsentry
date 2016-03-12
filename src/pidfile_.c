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
#include "parse_.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/file.h>

/* -------------------------------------------------------------------------- */
/* Maximum pid file size
 *
 * Bound the size of the pid file so that IO requirements can be kept
 * reasonable. This provides a way to avoid having large files cause
 * the watchdog to fail.
 */

#define PIDFILE_SIZE_ 1024

/* -------------------------------------------------------------------------- */
static int
openPidFile_(struct PidFile *self, const char *aFileName)
{
    int rc = -1;

    self->mFile = 0;
    self->mLock = LOCK_UN;

    ERROR_IF(
        createPathName(&self->mPathName, aFileName));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
closePidFile_(struct PidFile *self)
{
    if (self)
    {
        closeFile(self->mFile);
        closePathName(&self->mPathName);
    }
}

/* -------------------------------------------------------------------------- */
static int
lockPidFile_(struct PidFile *self, int aLock, const char *aLockType)
{
    int rc = -1;

    debug(0, "lock %s '%s'", aLockType, self->mPathName.mFileName);

    ensure(LOCK_UN != aLock);
    ensure(LOCK_UN == self->mLock);

    TEST_RACE
    ({
        ERROR_IF(
            lockFile(self->mFile, aLock));
    });

    self->mLock = aLock;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static struct Pid
readPidFile_(char *aBuf)
{
    int rc = -1;

    struct Pid pid = Pid(0);

    char *signature = 0;

    char *endptr = strchr(aBuf, 0);
    char *nlptr  = strchr(aBuf, '\n');

    do
    {
        if (endptr == aBuf)
            break;

        if ( ! nlptr)
            break;

        if (nlptr + 1 == endptr)
            break;

        if ('\n' != endptr[-1])
            break;

        *nlptr     = 0;
        endptr[-1] = 0;

        const char *pidSignature = nlptr + 1;

        struct Pid parsedPid;
        if (parsePid(aBuf, &parsedPid))
            break;

        if ( ! parsedPid.mPid)
            break;

        int err = 0;
        ERROR_IF(
            (err = fetchProcessSignature(parsedPid, &signature),
             err && ENOENT != errno));

        if ( ! err)
        {
            debug(0, "pidfile signature %s vs %s", pidSignature, signature);

            if ( ! strcmp(pidSignature, signature))
            {
                pid = parsedPid;
                break;
            }
        }

        /* The process either does not exist, or if it does exist the two
         * process signatures do not match. */

    } while (0);

    rc = 0;

Finally:

    FINALLY
    ({
        free(signature);
    });

    return rc ? Pid(-1) : pid;
}

struct Pid
readPidFile(const struct PidFile *self)
{
    int        rc           = -1;
    struct Pid pid          = Pid(0);
    int        pidfd        = -1;
    char      *pidsignature = 0;
    char      *signature    = 0;

    ensure(LOCK_UN != self->mLock);

    do
    {
        char buf[PIDFILE_SIZE_+1];

        ssize_t buflen;
        ERROR_IF(
            (buflen = readFile(self->mFile, buf, sizeof(buf)),
             -1 == buflen));

        if (sizeof(buf) > buflen)
        {
            /* Try to read a little more from the file to be sure that
             * the entire content of the file has been scanned. */

            ssize_t lastlen;
            ERROR_IF(
                (lastlen = readFile(
                    self->mFile, buf + buflen, 1),
                 -1 == lastlen));

            if ( ! lastlen)
            {
                buf[buflen] = 0;
                ERROR_IF(
                    (pid = readPidFile_(buf),
                     -1 == pid.mPid));
                break;
            }
        }

        pid = Pid(0);

    } while (0);

    rc = 0;

Finally:

    FINALLY
    ({
        closeFd(&pidfd);

        free(pidsignature);
        free(signature);
    });

    return rc ? Pid(-1) : pid;
}

/* -------------------------------------------------------------------------- */
int
releaseLockPidFile(struct PidFile *self)
{
    int rc = -1;

    ensure(LOCK_UN != self->mLock);

    ERROR_IF(
        unlockFile(self->mFile));

    self->mLock = LOCK_UN;

    debug(0, "unlock '%s'", self->mPathName.mFileName);

    rc = 0;

Finally:

    FINALLY
    ({
        TEST_RACE({});
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

    struct PidFile *pidFile = 0;
    ERROR_IF(
        openPidFile_(self, aFileName));
    pidFile = self;

    /* Check if the pidfile already exists, and if the process that
     * it names is still running.
     */

    int err;
    ERROR_IF(
        (err = createFile(
            &self->mFile_,
            openPathName(&self->mPathName, O_RDONLY | O_NOFOLLOW, 0)),
         err && ENOENT != errno));

    if ( ! err)
    {
        self->mFile = &self->mFile_;

        ERROR_IF(
            acquireWriteLockPidFile(self));

        /* If the pidfile names a valid process then give up since
         * it means that the pidfile is already owned. Otherwise,
         * the pidfile is not owned, and can be deleted. */

        struct Pid pid;
        ERROR_IF(
            (pid = readPidFile(self),
             -1 == pid.mPid));

        ERROR_IF(
            pid.mPid,
            {
                errno = EEXIST;
            });

        debug(
            0,
            "removing existing pidfile '%s'", self->mPathName.mFileName);

        ERROR_IF(
            unlinkPathName(&self->mPathName, 0) && ENOENT != errno);

        ERROR_IF(
            releaseLockPidFile(self));

        closeFile(self->mFile);

        self->mFile = 0;
    }

    /* Open the pidfile using lock file semantics for writing, but
     * with readonly permissions. Use of lock file semantics ensures
     * that the watchdog will be the owner of the pid file, and
     * readonly permissions dissuades other processes from modifying
     * the content. */

    ensure( ! self->mFile);
    TEST_RACE
    ({
        ERROR_IF(
            createFile(
                &self->mFile_,
                openPathName(&self->mPathName,
                             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                             S_IRUSR)));
    });
    self->mFile = &self->mFile_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            closePidFile_(pidFile);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
openPidFile(struct PidFile *self, const char *aFileName)
{
    int rc = -1;

    ERROR_IF(
        openPidFile_(self, aFileName));

    ERROR_IF(
        createFile(
            &self->mFile_,
            openPathName(&self->mPathName, O_RDONLY | O_NOFOLLOW, 0)));

    self->mFile = &self->mFile_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            closePidFile_(self);
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

    ERROR_IF(
        fstatPathName(
            &self->mPathName, &fileStatus, AT_SYMLINK_NOFOLLOW),
        {
            if (ENOENT == errno)
                rc = 1;
        });

    struct stat fdStatus;

    ERROR_IF(
        fstatFile(self->mFile, &fdStatus));

    rc = ( fdStatus.st_dev != fileStatus.st_dev ||
           fdStatus.st_ino != fileStatus.st_ino );

Finally:

    FINALLY({});

    if ( ! rc && testAction(TestLevelRace))
        rc = 1;

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closePidFile(struct PidFile *self)
{
    if (self)
    {
        ensure(LOCK_UN != self->mLock);

        int flags;
        ABORT_IF(
            (flags = fcntlFileGetFlags(self->mFile), -1 == flags),
            {
                terminate(
                    errno,
                    "Unable to get flags for pid file '%s'",
                    self->mPathName.mFileName);
            });

        if (O_RDONLY != (flags & O_ACCMODE) && LOCK_EX == self->mLock)
        {
            /* The pidfile is still locked at this point. If writable,
             * remove the content from the pidfile first so that any
             * competing reader will see any empty file. Once emptied,
             * remove the pidfile so that no other process will be
             * able to find the file. */

            ABORT_IF(
                ftruncateFile(self->mFile, 0),
                {
                    terminate(
                        errno,
                        "Unable to truncate pid file '%s'",
                        self->mPathName.mFileName);
                });

            /* In theory, ENOENT should not occur since the pidfile
             * is locked, and competing processes need to hold the
             * lock to remove the pidfile. It might be possible
             * that the pidfile is deleted from, say, the command
             * line. */

            ABORT_IF(
                unlinkPathName(&self->mPathName, 0) && ENOENT != errno,
                {
                    terminate(
                        errno,
                        "Unable to unlink pid file '%s'",
                        self->mPathName.mFileName);
                });
        }

        closeFile(self->mFile);
        closePathName(&self->mPathName);

        self->mFile = 0;
        self->mLock = LOCK_UN;
    }
}

/* -------------------------------------------------------------------------- */
int
writePidFile(struct PidFile *self, struct Pid aPid)
{
    int   rc        = -1;
    char *signature = 0;

    ensure(0 < aPid.mPid);

    ERROR_IF(
        fetchProcessSignature(aPid, &signature));

    char buf[PIDFILE_SIZE_+1];

    int buflen =
        snprintf(
            buf, sizeof(buf), "%" PRId_Pid "\n%s\n", FMTd_Pid(aPid), signature);

    ERROR_IF(
        0 > buflen,
        {
            errno = EIO;
        });

    ERROR_IF(
        ! buflen || sizeof(buf) <= buflen,
        {
            errno = ERANGE;
        });

    /* Separate the formatting of the signature from the actual IO
     * so that it is possible to determine if there is a formatting
     * error, or an IO error. */

    ERROR_IF(
        writeFile(self->mFile, buf, buflen) != buflen,
        {
            errno = EIO;
        });

    rc = 0;

Finally:

    FINALLY
    ({
        free(signature);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
