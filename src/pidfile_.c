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
#include <sys/un.h>

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
    self->mLock = 0;

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
        closePathName(&self->mPathName);
}

/* -------------------------------------------------------------------------- */
static int
lockPidFile_(struct PidFile *self, int aLock, const char *aLockType)
{
    int rc = -1;

    debug(0, "lock %s '%s'", aLockType, self->mPathName.mFileName);

    ensure(aLock);
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
readPidFile_(char *aBuf, struct sockaddr_un *aPidKeeperAddr)
{
    int rc = -1;

    struct Pid pid = Pid(0);

    char *signature = 0;

    char *endPtr  = strchr(aBuf, 0);
    char *sigPtr  = strchr(aBuf, '\n');
    char *addrPtr = sigPtr ? strchr(sigPtr+1, '\n') : 0;

    do
    {
        if (endPtr == aBuf)
            break;

        if ( ! sigPtr)
            break;

        if (sigPtr + 1 == endPtr)
            break;

        if ( ! addrPtr)
            break;

        if (addrPtr + 1 == endPtr)
            break;

        if ('\n' != endPtr[-1])
            break;

        *sigPtr   = 0;
        *addrPtr  = 0;
        *--endPtr = 0;

        const char *pidSignature     = sigPtr + 1;
        const char *pidKeeperAddr    = addrPtr + 1;
        size_t      pidKeeperAddrLen = endPtr - pidKeeperAddr;

        struct Pid parsedPid;
        if (parsePid(aBuf, &parsedPid))
            break;

        if ( ! parsedPid.mPid)
            break;

        ERROR_IF(
            pidKeeperAddrLen + 2 > sizeof(aPidKeeperAddr->sun_path),
            {
                errno = EADDRNOTAVAIL;
            });

        aPidKeeperAddr->sun_path[0] = 0;
        memcpy(&aPidKeeperAddr->sun_path[1], pidKeeperAddr, pidKeeperAddrLen);
        memset(aPidKeeperAddr->sun_path + pidKeeperAddrLen + 1,
               0,
               sizeof(aPidKeeperAddr->sun_path) - pidKeeperAddrLen - 1);

        debug(0, "pidfile address %s", &aPidKeeperAddr->sun_path[1]);

        int err = 0;
        ERROR_IF(
            (err = fetchProcessSignature(parsedPid, &signature),
             err && ENOENT != errno));

        if ( ! err)
        {
            if ( ! strcmp(pidSignature, signature))
            {
                debug(0, "pidfile signature %s", pidSignature);

                pid = parsedPid;
                break;
            }

            debug(0, "pidfile signature %s vs %s", pidSignature, signature);
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
readPidFile(const struct PidFile *self,
            struct sockaddr_un   *aPidKeeperAddr)
{
    int        rc    = -1;
    struct Pid pid   = Pid(0);
    int        pidfd = -1;

    char *bufcpy = 0;

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
                    (pid = readPidFile_(buf, aPidKeeperAddr),
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

        free(bufcpy);
    });

    return rc ? Pid(-1) : pid;
}

/* -------------------------------------------------------------------------- */
int
releaseLockPidFile(struct PidFile *self)
{
    int rc = -1;

    ensure(self->mLock);
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
static int
acquireWriteLockPidFile_(struct PidFile *self)
{
    return lockPidFile_(self, LOCK_EX, "exclusive");
}

int
acquireWriteLockPidFile(struct PidFile *self)
{
    int rc = -1;

    int flags;
    ERROR_IF(
        (flags = fcntlFileGetFlags(self->mFile),
         -1 == flags));

    ERROR_UNLESS(
        O_RDONLY != (flags & O_ACCMODE));

    ERROR_IF(
        acquireWriteLockPidFile_(self));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
acquireReadLockPidFile(struct PidFile *self)
{
    return lockPidFile_(self, LOCK_SH, "shared");
}

/* -------------------------------------------------------------------------- */
int
initPidFile(struct PidFile *self_, const char *aFileName)
{
    int rc = -1;

    struct PidFile *self = 0;

    ERROR_IF(
        openPidFile_(self_, aFileName));
    self = self_;

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
void
destroyPidFile(struct PidFile *self)
{
    if (self)
    {
        closePidFile(self);
        closePathName(&self->mPathName);
    }
}

/* -------------------------------------------------------------------------- */
static int
unlinkPidFile_(struct PidFile *self)
{
    int rc = -1;

    ensure(LOCK_EX == self->mLock);

    /* The pidfile might already have been unlinked from its enclosing
     * directory by another process, but this code enforces the precondition
     * that the caller must hold an exclusive lock on the pidfile to be
     * unlinked before attempting the operation. */

    int deleted = 0;

    int zombie;
    ERROR_IF(
        (zombie = detectPidFileZombie(self),
         0 > zombie));

    /* If the pidfile is a zombie, it is no longer present in its
     * enclosing directory, in which case it is not necessary
     * to unlink it. */

    if ( ! zombie)
    {
        ERROR_IF(
            unlinkPathName(&self->mPathName, 0) && ENOENT != errno);

        deleted = 1;
    }

    rc = deleted;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
openPidFile(struct PidFile *self, unsigned aFlags)
{
    int rc = -1;

    ERROR_IF(
        self->mLock,
        {
            errno = EALREADY;
        });
    self->mLock = LOCK_UN;

    ERROR_IF(
        aFlags & ~ (O_CLOEXEC | O_CREAT),
        {
            errno = EINVAL;
        });

    unsigned openFlags = aFlags & O_CLOEXEC;

    if ( ! (aFlags & O_CREAT))
    {
        ERROR_IF(
            createFile(
                &self->mFile_,
                openPathName(&self->mPathName,
                             O_RDONLY | O_NOFOLLOW | openFlags, 0)));
        self->mFile = &self->mFile_;
    }
    else
    {
        /* If O_CREAT is specified, a successful return provides the
         * caller with a new, empty pidfile that was created
         * exclusively (O_EXCL) in the enclosing directory, but because
         * the pidfile it empty and unlocked, it can become a zombie
         * at any time.
         *
         * In order to furnish the caller with a new pidfile, any
         * pre-existing pidfile in the directory with the same name
         * must be removed, if possible. */

        int err;
        ERROR_IF(
            (err = createFile(
                &self->mFile_,
                openPathName(&self->mPathName,
                             O_RDONLY | O_NOFOLLOW | openFlags, 0)),
             err && ENOENT != errno));

        if ( ! err)
        {
            self->mFile = &self->mFile_;

            ERROR_IF(
                acquireWriteLockPidFile_(self));

            /* If the pre-existing pidfile names a valid process then give
             * up since it means that the requested name is already taken.
             * Otherwise, the pidfile is either empty, or names a process
             * that non longer exists, and so can be deleted. */

            struct sockaddr_un pidKeeperAddr;
            struct Pid         pid;
            ERROR_IF(
                (pid = readPidFile(self, &pidKeeperAddr),
                 -1 == pid.mPid));

            ERROR_IF(
                pid.mPid,
                {
                    errno = EEXIST;
                });

            int unlinked;
            ERROR_IF(
                (unlinked = unlinkPidFile_(self),
                 -1 == unlinked));

            if (unlinked)
                debug(
                    0,
                    "removing existing pidfile '%s'",
                    self->mPathName.mFileName);

            ERROR_IF(
                releaseLockPidFile(self));

            closeFile(self->mFile);

            self->mFile = 0;
        }

        /* This is a window where another process can also race to
         * create the pidfile. Guard against that by using O_EXCL
         * which will only allow one of the prcesses to succeed.
         *
         * Open the pidfile using lock file semantics for writing, but
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
                    openPathName(
                        &self->mPathName,
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | openFlags,
                        S_IRUSR)));
        });
        self->mFile = &self->mFile_;

        /* Although the pidfile is created, it is unlocked and empty, so
         * any other process will consider it a zombie, remove it and
         * write its own pidfile. If that happens, from the point of view
         * of this caller, this pidfile will have become a zombie. */
    }

    ensure(self->mFile == &self->mFile_);

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            closeFile(self->mFile);
            self->mFile = 0;
            self->mLock = 0;
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

    int err;
    ERROR_IF(
        (err = fstatPathName(
            &self->mPathName, &fileStatus, AT_SYMLINK_NOFOLLOW),
         err && ENOENT != errno));

    bool zombie;

    if (err)
        zombie = true;
    else
    {
        struct stat fdStatus;

        ERROR_IF(
            fstatFile(self->mFile, &fdStatus));

        zombie = ( fdStatus.st_dev != fileStatus.st_dev ||
                   fdStatus.st_ino != fileStatus.st_ino );
    }

    rc = zombie;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closePidFile(struct PidFile *self)
{
    if (self && self->mFile)
    {
        ensure(self->mLock);

        if (LOCK_EX == self->mLock)
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

            int unlinked;
            ABORT_IF(
                (unlinked = unlinkPidFile_(self),
                 -1 == unlinked || (errno = 0, ! unlinked)),
                {
                    terminate(
                        errno,
                        "Unable to unlink pid file '%s'",
                        self->mPathName.mFileName);
                });
        }

        closeFile(self->mFile);

        self->mFile = 0;
        self->mLock = 0;
    }
}

/* -------------------------------------------------------------------------- */
int
writePidFile(struct PidFile           *self,
             struct Pid                aPid,
             const struct sockaddr_un *aPidServerAddr)
{
    int   rc        = -1;
    char *signature = 0;

    ensure(0 < aPid.mPid);

    ensure( ! aPidServerAddr->sun_path[0]);
    ensure( 1 < sizeof(aPidServerAddr->sun_path));
    ensure( ! aPidServerAddr->sun_path[sizeof(aPidServerAddr->sun_path)-1]);
    ensure( aPidServerAddr->sun_path[1]);

    ERROR_IF(
        fetchProcessSignature(aPid, &signature));

    char buf[PIDFILE_SIZE_+1];

    int buflen =
        snprintf(
            buf, sizeof(buf),
            "%" PRId_Pid "\n%s\n%s\n",
            FMTd_Pid(aPid),
            signature,
            &aPidServerAddr->sun_path[1]);

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
