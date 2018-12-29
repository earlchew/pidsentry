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
#include "pidsignature_.h"

#include "ert/error.h"
#include "ert/parse.h"
#include "ert/mode.h"

#include <stdlib.h>

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

static const struct Ert_LockType * const lockTypeRead_  = &Ert_LockTypeRead;
static const struct Ert_LockType * const lockTypeWrite_ = &Ert_LockTypeWrite;

/* -------------------------------------------------------------------------- */
int
printPidFile(const struct PidFile *self, FILE *aFile)
{
    return fprintf(aFile, "<pidfile %p %s>", self, self->mPathName->mFileName);
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
lockPidFile_(
    struct PidFile *self,
    const struct Ert_LockType *aLockType, const char *aLockName)
{
    int rc = -1;

    ert_debug(
        0,
        "locking %s %" PRIs_Ert_Method,
        aLockName,
        FMTs_Ert_Method(self, printPidFile));

    ert_ensure(aLockType);
    ert_ensure( ! self->mLock);

    ERT_TEST_RACE
    ({
        ERT_ERROR_IF(
            ert_lockFile(self->mFile, *aLockType));
    });

    ert_debug(
        0,
        "locked %s %" PRIs_Ert_Method,
        aLockName,
        FMTs_Ert_Method(self, printPidFile));

    self->mLock = aLockType;

    rc = 0;

Ert_Finally:

    ERT_FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED struct PidSignature *
readPidFile_(char *aBuf, struct sockaddr_un *aPidKeeperAddr)
{
    int rc = -1;

    struct PidSignature *signature = 0;

    char *endPtr  = strchr(aBuf, 0);
    char *sepPtr  = strchr(aBuf, '\n');
    char *sigPtr  = sepPtr ? strchr(sepPtr+1, '\n') : 0;
    char *addrPtr = sigPtr ? strchr(sigPtr+1, '\n') : 0;

    do
    {
        if (endPtr == aBuf)
            break;

        if ( ! sepPtr || sepPtr + 1 == endPtr)
            break;

        if ( ! sigPtr || sigPtr + 1 == endPtr)
            break;

        if ( ! addrPtr || addrPtr + 1 == endPtr)
            break;

        if ('\n' != endPtr[-1])
            break;

        *sepPtr   = 0;
        *sigPtr   = 0;
        *addrPtr  = 0;
        *--endPtr = 0;

        const char *pidSignature     = sigPtr + 1;
        const char *pidKeeperAddr    = addrPtr + 1;
        size_t      pidKeeperAddrLen = endPtr - pidKeeperAddr;

        struct Ert_Pid parsedPid;
        if (ert_parsePid(aBuf, &parsedPid))
            break;

        if ( ! parsedPid.mPid)
            break;

        ERT_ERROR_IF(
            pidKeeperAddrLen + 2 > sizeof(aPidKeeperAddr->sun_path),
            {
                errno = EADDRNOTAVAIL;
            });

        aPidKeeperAddr->sun_path[0] = 0;
        memcpy(&aPidKeeperAddr->sun_path[1], pidKeeperAddr, pidKeeperAddrLen);
        memset(aPidKeeperAddr->sun_path + pidKeeperAddrLen + 1,
               0,
               sizeof(aPidKeeperAddr->sun_path) - pidKeeperAddrLen - 1);

        ert_debug(0, "pidfile address %s", &aPidKeeperAddr->sun_path[1]);

        ERT_ERROR_IF(
            (signature = createPidSignature(parsedPid, 0),
             ! signature && ENOENT != errno));

        if (signature)
        {
            if ( ! strcmp(pidSignature, signature->mSignature))
            {
                ert_debug(0, "pidfile signature %s", pidSignature);
                break;
            }

            ert_debug(
                0,
                "pidfile signature %s vs %s",
                pidSignature,
                signature->mSignature);
        }

        /* The process either does not exist, or if it does exist the two
         * process signatures do not match. Use Pid.mPid == 0 to
         * distinguish this case. */

        signature = destroyPidSignature(signature);

        ERT_ERROR_UNLESS(
            signature = createPidSignature(Ert_Pid(0), 0));

    } while (0);

     /* Use Pid.mPid == -1 to indicate that there was a problem parsing or
     * otherwise interpreting the pid. */

    if ( ! signature)
        ERT_ERROR_UNLESS(
            signature = createPidSignature(Ert_Pid(-1), 0));

    rc = 0;

Ert_Finally:

    ERT_FINALLY
    ({
        if (rc)
            signature = destroyPidSignature(signature);
    });

    return signature;
}

struct PidSignature *
readPidFile(const struct PidFile *self,
            struct sockaddr_un   *aPidKeeperAddr)
{
    int rc  = -1;

    struct PidSignature *signature = 0;

    char *bufcpy = 0;

    ert_ensure(self->mLock);

    do
    {
        char buf[PIDFILE_SIZE_+1];

        ssize_t buflen;
        ERT_ERROR_IF(
            (buflen = ert_readFile(self->mFile, buf, sizeof(buf), 0),
             -1 == buflen));

        if (sizeof(buf) > buflen)
        {
            /* Try to read a little more from the file to be sure that
             * the entire content of the file has been scanned. */

            ssize_t lastlen;
            ERT_ERROR_IF(
                (lastlen = ert_readFile(self->mFile, buf + buflen, 1, 0),
                 -1 == lastlen));

            if ( ! lastlen)
            {
                buf[buflen] = 0;
                ERT_ERROR_UNLESS(
                    signature = readPidFile_(buf, aPidKeeperAddr));
                break;
            }
        }

        /* Since the size of the pidfile seems to be larger than expected
         * treat it as a problem parsing the pid. */

        ert_ensure( ! signature);

        char emptyPid[] = "";

        ERT_ERROR_UNLESS(
            signature = readPidFile_(emptyPid, 0));

    } while (0);

    ert_ensure(signature);

    rc = 0;

Ert_Finally:

    ERT_FINALLY
    ({
        free(bufcpy);

        if (rc)
            signature = destroyPidSignature(signature);
    });

    return signature;
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
releasePidFileLock_(struct PidFile *self)
{
    int rc = -1;

    ert_ensure(self->mLock);

    ERT_ERROR_IF(
        ert_unlockFile(self->mFile));

    self->mLock = 0;

    ert_debug(
        0,
        "unlocked %" PRIs_Ert_Method,
        FMTs_Ert_Method(self, printPidFile));

    rc = 0;

Ert_Finally:

    ERT_FINALLY
    ({
        ERT_TEST_RACE({});
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
acquirePidFileWriteLock_(struct PidFile *self)
{
    return lockPidFile_(self, lockTypeWrite_, "exclusive");
}

int
acquirePidFileWriteLock(struct PidFile *self)
{
    int rc = -1;

    int flags;
    ERT_ERROR_IF(
        (flags = ert_fcntlFileGetFlags(self->mFile),
         -1 == flags));

    ERT_ERROR_UNLESS(
        O_RDONLY != (flags & O_ACCMODE));

    ERT_ERROR_IF(
        acquirePidFileWriteLock_(self));

    rc = 0;

Ert_Finally:

    ERT_FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
acquirePidFileReadLock(struct PidFile *self)
{
    return lockPidFile_(self, lockTypeRead_, "shared");
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
detectPidFileZombie_(const struct PidFile *self)
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
    ERT_ERROR_IF(
        (err = ert_fstatPathName(
            self->mPathName, &fileStatus, AT_SYMLINK_NOFOLLOW),
         err && ENOENT != errno));

    bool zombie;

    if (err)
        zombie = true;
    else
    {
        struct stat fdStatus;

        ERT_ERROR_IF(
            ert_fstatFile(self->mFile, &fdStatus));

        zombie = ( fdStatus.st_dev != fileStatus.st_dev ||
                   fdStatus.st_ino != fileStatus.st_ino );
    }

    rc = zombie;

Ert_Finally:

    ERT_FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
unlinkPidFile_(struct PidFile *self)
{
    int rc = -1;

    ert_ensure(lockTypeWrite_ == self->mLock);

    /* The pidfile might already have been unlinked from its enclosing
     * directory by another process, but this code enforces the precondition
     * that the caller must hold an exclusive lock on the pidfile to be
     * unlinked before attempting the operation. */

    int deleted = 0;

    int zombie;
    ERT_ERROR_IF(
        (zombie = detectPidFileZombie_(self),
         0 > zombie));

    /* If the pidfile is a zombie, it is no longer present in its
     * enclosing directory, in which case it is not necessary
     * to unlink it. */

    if ( ! zombie)
    {
        ERT_ERROR_IF(
            ert_unlinkPathName(self->mPathName, 0) && ENOENT != errno);

        deleted = 1;
    }

    rc = deleted;

Ert_Finally:

    ERT_FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED struct Ert_Pid
generatePidFile_(struct PidFile *self, unsigned aFlags, struct Ert_Mode aMode)
{
    int rc = -1;

    struct PidSignature *pidSignature = 0;

    struct Ert_Pid pid = Ert_Pid(-1);

    ert_ensure(aMode.mMode & S_IRUSR);
    ert_ensure( ! (aFlags & ~ O_CLOEXEC));
    ert_ensure( ! self->mFile);

    int err;

    do
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

        ERT_ERROR_IF(
            (err = ert_createFile(
                &self->mFile_,
                ert_openPathName(self->mPathName,
                                 O_RDONLY | O_NOFOLLOW | aFlags, Ert_Mode(0))),
             err && ENOENT != errno));

        if ( ! err)
        {
            self->mFile = &self->mFile_;

            ERT_ERROR_IF(
                acquirePidFileWriteLock_(self));

            /* If the pre-existing pidfile names a valid process then give
             * up since it means that the requested name is already taken.
             * Otherwise, the pidfile is either empty, or names a process
             * that non longer exists, and so can be deleted. */

            struct sockaddr_un pidKeeperAddr;
            ERT_ERROR_UNLESS(
                pidSignature = readPidFile(self, &pidKeeperAddr));

            ERT_ERROR_IF(
                pidSignature->mPid.mPid && -1 != pidSignature->mPid.mPid,
                {
                    errno = EEXIST;
                    pid   = pidSignature->mPid;
                });

            int unlinked;
            ERT_ERROR_IF(
                (unlinked = unlinkPidFile_(self),
                 -1 == unlinked));

            if (unlinked)
                ert_debug(
                    0,
                    "removing existing file %" PRIs_Ert_Method,
                    FMTs_Ert_Method(self, printPidFile));

            ERT_ERROR_IF(
                releasePidFileLock_(self));

            self->mFile = ert_closeFile(self->mFile);

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

        ert_ensure( ! self->mFile);
        ERT_TEST_RACE
        ({
            ERT_ERROR_IF(
                (err = ert_createFile(
                    &self->mFile_,
                    ert_openPathName(
                        self->mPathName,
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | aFlags,
                        aMode)),
                 err && EEXIST != errno));
        });

    } while (err);

    self->mFile = &self->mFile_;

    rc = 0;

Ert_Finally:

    ERT_FINALLY
    ({
        if (rc)
            self->mFile = ert_closeFile(self->mFile);

        pidSignature = destroyPidSignature(pidSignature);
    });

    return rc ? pid : Ert_Pid(0);
}

static ERT_CHECKED struct Ert_Pid
openPidFile_(struct PidFile *self, unsigned aFlags, struct Ert_Mode aMode)
{
    int rc = -1;

    struct Ert_Pid pid = Ert_Pid(-1);

    int already = -1;
    {
        ERT_ERROR_IF(
            self->mFile || self->mLock,
            {
                errno = EALREADY;
            });
        self->mLock = 0;
    }
    already = 0;

    ERT_ERROR_IF(
        aFlags & ~ (O_CLOEXEC | O_CREAT),
        {
            errno = EINVAL;
        });

    unsigned openFlags = aFlags & O_CLOEXEC;

    if (aFlags & O_CREAT)
    {
        ERT_ERROR_UNLESS(
            aMode.mMode & S_IRUSR,
            {
                errno = EINVAL;
            });
        ERT_ERROR_IF(
            (pid = generatePidFile_(self, openFlags, aMode),
             pid.mPid));
    }
    else
    {
        ERT_ERROR_IF(
            aMode.mMode,
            {
                errno = EINVAL;
            });
        ERT_ERROR_IF(
            ert_createFile(
                &self->mFile_,
                ert_openPathName(self->mPathName,
                                 O_RDONLY | O_NOFOLLOW | openFlags,
                                 Ert_Mode(0))));
        self->mFile = &self->mFile_;
    }

    ert_ensure(self->mFile == &self->mFile_);

    rc = 0;

Ert_Finally:

    ERT_FINALLY
    ({
        if (rc)
        {
            if ( ! already)
            {
                self->mFile = ert_closeFile(self->mFile);
                self->mLock = 0;
            }
        }
    });

    return rc ? pid : Ert_Pid(0);
}

struct Ert_Pid
openPidFile(struct PidFile *self, unsigned aFlags)
{
    return openPidFile_(self, aFlags, Ert_Mode(0));
}

/* -------------------------------------------------------------------------- */
int
closePidFile(struct PidFile *self)
{
    int rc = -1;

    if (self && self->mFile)
    {
        if (lockTypeWrite_ == self->mLock)
        {
            /* The pidfile is still locked at this point. If writable,
             * remove the content from the pidfile first so that any
             * competing reader will see any empty file. Once emptied,
             * remove the pidfile so that no other process will be
             * able to find the file. */

            ERT_ABORT_IF(
                ert_ftruncateFile(self->mFile, 0));

            /* In theory, ENOENT should not occur since the pidfile
             * is locked, and competing processes need to hold the
             * lock to remove the pidfile. It might be possible
             * that the pidfile is deleted from, say, the command
             * line. */

            int unlinked;
            ERT_ABORT_IF(
                (unlinked = unlinkPidFile_(self),
                 -1 == unlinked || (errno = 0, ! unlinked)));
        }

        self->mFile = ert_closeFile(self->mFile);
        self->mLock = 0;
    }

    rc = 0;

Ert_Finally:

    ERT_FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
enum Ert_PathNameStatus
initPidFile(struct PidFile *self, const char *aFileName)
{
    int rc = -1;

    enum Ert_PathNameStatus status = Ert_PathNameStatusError;

    self->mFile     = 0;
    self->mLock     = 0;
    self->mPathName = 0;

    ERT_ERROR_IF(
        (status = ert_createPathName(&self->mPathName_, aFileName),
         Ert_PathNameStatusOk != status));
    self->mPathName = &self->mPathName_;

    rc = 0;

Ert_Finally:

    ERT_FINALLY
    ({
        if (rc)
            self = destroyPidFile(self);
    });

    return status;
}

/* -------------------------------------------------------------------------- */
struct PidFile *
destroyPidFile(struct PidFile *self)
{
    if (self)
    {
        ERT_ABORT_IF(
            closePidFile(self));

        self->mPathName = ert_closePathName(self->mPathName);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
createPidFile_(struct PidFile           *self,
               struct Ert_Pid            aPid,
               const struct sockaddr_un *aPidServerAddr)
{
    int rc = -1;

    struct PidSignature *signature = 0;

    ert_ensure(0 < aPid.mPid);

    ert_ensure( ! aPidServerAddr->sun_path[0]);
    ert_ensure( 1 < sizeof(aPidServerAddr->sun_path));
    ert_ensure( ! aPidServerAddr->sun_path[sizeof(aPidServerAddr->sun_path)-1]);
    ert_ensure( aPidServerAddr->sun_path[1]);

    ERT_ERROR_UNLESS(
        signature = createPidSignature(aPid, 0));

    char buf[PIDFILE_SIZE_+1];

    /* The Linux Standard Base Core says:
     *
     *   If the -p pidfile option is specified, and the named pidfile exists,
     *   a single line at the start of the pidfile shall be read. If this line
     *   contains one or more numeric values, separated by spaces, these values
     *   shall be used. If the -p pidfile option is specified and the named
     *   pidfile does not exist, the functions shall assume that the daemon is
     *   not running.
     *
     * The Fedora implementation (FC12) reads all lines in the specified
     * pidfile, stopping on the first blank line. */

    int buflen =
        snprintf(
            buf, sizeof(buf),
            "%" PRId_Ert_Pid "\n\n%s\n%s\n",
            FMTd_Ert_Pid(aPid),
            signature->mSignature,
            &aPidServerAddr->sun_path[1]);

    ERT_ERROR_IF(
        0 > buflen,
        {
            errno = EIO;
        });

    ERT_ERROR_IF(
        ! buflen || sizeof(buf) <= buflen,
        {
            errno = ERANGE;
        });

    /* Separate the formatting of the signature from the actual IO
     * so that it is possible to determine if there is a formatting
     * error, or an IO error. */

    ERT_ERROR_IF(
        ert_writeFile(self->mFile, buf, buflen, 0) != buflen,
        {
            errno = EIO;
        });

    rc = 0;

Ert_Finally:

    ERT_FINALLY
    ({
        signature = destroyPidSignature(signature);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct Ert_Pid
createPidFile(struct PidFile           *self,
              struct Ert_Pid            aPid,
              const struct sockaddr_un *aPidServerAddr,
              struct Ert_Mode           aMode)
{
    int rc = -1;

    struct Ert_Pid pid = Ert_Pid(-1);

    ERT_ERROR_IF(
        self->mFile || self->mLock,
        {
            errno = EALREADY;
        });

    for (int zombie = -1; zombie; )
    {
        if (0 < zombie)
        {
            /* If the pidfile has become a zombie, it is possible to
             * delete it here, but do not attempt to do so, and instead
             * rely on the correct deletion semantics to be used when
             * a new attempt is made to open the pidfile. */

            ERT_ERROR_IF(
                releasePidFileLock_(self));

            ert_debug(
                0,
                "disregarding zombie %" PRIs_Ert_Method,
                FMTs_Ert_Method(self, printPidFile));

            ERT_ERROR_IF(
                closePidFile(self));
        }

        struct Ert_Pid openedPid = Ert_Pid(-1);
        ERT_ERROR_IF(
            (openedPid = openPidFile_(self, O_CLOEXEC | O_CREAT, aMode),
             openedPid.mPid),
            {
                if (EEXIST == errno)
                    pid = openedPid;
            });

        /* It is not possible to create the pidfile and acquire a flock
         * as an atomic operation. The flock can only be acquired after
         * the pidfile exists. Since this newly created pidfile is empty,
         * it resembles an closed pidfile, and in the intervening time,
         * another process might have removed it and replaced it with
         * another, turning the pidfile held by this process into a zombie. */

        ERT_ERROR_IF(
            acquirePidFileWriteLock(self));

        ERT_ERROR_IF(
            (zombie = detectPidFileZombie_(self),
             0 > zombie));
    }

    /* At this point, this process has a newly created, empty and locked
     * pidfile. The pidfile cannot be deleted because a write lock must
     * be held for deletion to occur. */

    ert_debug(
        0,
        "initialised %" PRIs_Ert_Method " mode %" PRIs_Ert_Mode,
        FMTs_Ert_Method(self, printPidFile),
        FMTs_Ert_Mode(aMode));

    ERT_ERROR_IF(
        createPidFile_(self, aPid, aPidServerAddr));

    /* The pidfile was locked on creation, and now that it is completely
     * initialised, it is ok to release the flock. Any other process will
     * check and see that the pidfile refers to a live process, and refrain
     * from deleting it. */

    ERT_ERROR_IF(
        releasePidFileLock_(self));

    rc = 0;

Ert_Finally:

    ERT_FINALLY
    ({
        ert_finally_warn_if(rc, self, printPidFile);
    });

    return rc ? pid : Ert_Pid(0);;
}

/* -------------------------------------------------------------------------- */
const char *
ownPidFileName(const struct PidFile *self)
{
    return self->mPathName->mFileName;
}

/* -------------------------------------------------------------------------- */
