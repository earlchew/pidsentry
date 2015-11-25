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

#include "macros.h"
#include "file.h"
#include "pathname.h"
#include "error.h"
#include "options.h"
#include "timespec.h"
#include "process.h"
#include "test.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <time.h>

#include <getopt.h>
#include <unistd.h>
#include <libgen.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/resource.h>

/* TODO
 *
 * cmdRunCommand() is too big, break it up
 * EINTR everywhere, especially flock
 * SIGALARM for occasional EINTR
 * Stuck flock
 * cloneProcessLock -- and don't use /proc/self
 * struct ProcessLock using mPathName and mPathName_
 * struct PidFile using mPathName and mPathName_
 * Use splice()
 * Correctly close socket/pipe on read and write EOF
 * Check correct operation if child closes tether first, vs stdout close first
 * Shutdown pipes
 * Parent and child process groups
 */

struct SocketPair
{
    struct FileDescriptor  mParentFile_;
    struct FileDescriptor *mParentFile;
    struct FileDescriptor  mChildFile_;
    struct FileDescriptor *mChildFile;
};

struct Pipe
{
    struct FileDescriptor  mRdFile_;
    struct FileDescriptor *mRdFile;
    struct FileDescriptor  mWrFile_;
    struct FileDescriptor *mWrFile;
};

struct StdFdFiller
{
    struct FileDescriptor  mFile_[3];
    struct FileDescriptor *mFile[3];
};

struct PidFile
{
    struct PathName        mPathName;
    struct FileDescriptor  mFile_;
    struct FileDescriptor *mFile;
    int                    mLock;
};

/* -------------------------------------------------------------------------- */
static int
closeFd(int *aFd)
{
    int rc = close(*aFd);

    *aFd = -1;

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static bool
stdFd(int aFd)
{
    return STDIN_FILENO == aFd || STDOUT_FILENO == aFd || STDERR_FILENO == aFd;
}

/* -------------------------------------------------------------------------- */
static int
nonblockingFd(int aFd)
{
    long flags = fcntl(aFd, F_GETFL, 0);

    return -1 == flags ? -1 : fcntl(aFd, F_SETFL, flags | O_NONBLOCK);
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */
static int
createSocketPair(struct SocketPair *self)
{
    int rc = -1;

    self->mParentFile = 0;
    self->mChildFile  = 0;

    int fd[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd))
        goto Finally;

    if (-1 == fd[0] || -1 == fd[1])
        goto Finally;

    ensure( ! stdFd(fd[0]));
    ensure( ! stdFd(fd[1]));

    if (createFileDescriptor(&self->mParentFile_, fd[0]))
        goto Finally;
    self->mParentFile = &self->mParentFile_;

    fd[0] = -1;

    if (createFileDescriptor(&self->mChildFile_, fd[1]))
        goto Finally;
    self->mChildFile = &self->mChildFile_;

    fd[1] = -1;

    rc = 0;

Finally:

    FINALLY
    ({
        if (-1 != fd[0])
            close(fd[0]);
        if (-1 != fd[1])
            close(fd[1]);

        if (rc)
        {
            closeFileDescriptor(self->mParentFile);
            closeFileDescriptor(self->mChildFile);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
closeSocketPair(struct SocketPair *self)
{
    return closeFileDescriptorPair(&self->mParentFile, &self->mChildFile);
}

/* -------------------------------------------------------------------------- */
static int
createPipe(struct Pipe *self)
{
    int rc = -1;

    self->mRdFile = 0;
    self->mWrFile = 0;

    int fd[2];

    if (pipe(fd))
        goto Finally;

    if (-1 == fd[0] || -1 == fd[1])
        goto Finally;

    ensure( ! stdFd(fd[0]));
    ensure( ! stdFd(fd[1]));

    if (createFileDescriptor(&self->mRdFile_, fd[0]))
        goto Finally;
    self->mRdFile = &self->mRdFile_;

    fd[0] = -1;

    if (createFileDescriptor(&self->mWrFile_, fd[1]))
        goto Finally;
    self->mWrFile = &self->mWrFile_;

    fd[1] = -1;

    rc = 0;

Finally:

    FINALLY
    ({
        if (-1 != fd[0])
            close(fd[0]);
        if (-1 != fd[1])
            close(fd[1]);

        if (rc)
        {
            closeFileDescriptor(self->mRdFile);
            closeFileDescriptor(self->mWrFile);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
closePipe(struct Pipe *self)
{
    return closeFileDescriptorPair(&self->mRdFile, &self->mWrFile);
}

/* -------------------------------------------------------------------------- */
static int
createStdFdFiller(struct StdFdFiller *self)
{
    int rc = -1;

    for (unsigned ix = 0; NUMBEROF(self->mFile) > ix; ++ix)
        self->mFile[ix] = 0;

    int fd[2];

    if (pipe(fd))
        goto Finally;

    if (-1 == fd[0] || -1 == fd[1])
    {
        errno = EBADF;
        goto Finally;
    }

    /* Close the writing end of the pipe, leaving only the reading
     * end of the pipe. Any attempt to write will fail, and any
     * attempt to read will yield EOF. */

    if (close(fd[1]))
        goto Finally;

    fd[1] = -1;

    if (createFileDescriptor(&self->mFile_[0], fd[0]))
        goto Finally;
    self->mFile[0] = &self->mFile_[0];

    fd[0] = -1;

    if (dupFileDescriptor(&self->mFile_[1], &self->mFile_[0]))
        goto Finally;
    self->mFile[1] = &self->mFile_[1];

    if (dupFileDescriptor(&self->mFile_[2], &self->mFile_[0]))
        goto Finally;
    self->mFile[2] = &self->mFile_[2];

    rc = 0;

Finally:

    FINALLY
    ({
        if (-1 != fd[0])
            close(fd[0]);
        if (-1 != fd[1])
            close(fd[1]);

        if (rc)
        {
            for (unsigned ix = 0; NUMBEROF(self->mFile) > ix; ++ix)
            {
                closeFileDescriptor(self->mFile[ix]);
                self->mFile[ix] = 0;
            }
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
closeStdFdFiller(struct StdFdFiller *self)
{
    int rc = -1;

    for (unsigned ix = 0; NUMBEROF(self->mFile) > ix; ++ix)
    {
        if (closeFileDescriptor(self->mFile[ix]))
            goto Finally;
        self->mFile[ix] = 0;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int sDeadChildFd_ = -1;
static int sSignalFd_    = -1;

static struct SignalWatch {
    int          mSigNum;
    sighandler_t mSigHandler;
    bool         mWatched;
} sWatchedSignals_[] =
{
    { SIGHUP },
    { SIGINT },
    { SIGQUIT },
    { SIGTERM },
};

static int
writeSignal_(int aFd, char aSigNum)
{
    int rc = -1;

    if (-1 == aFd)
    {
        errno = EBADF;
        goto Finally;
    }

    ssize_t len = write(aFd, &aSigNum, 1);

    if (-1 == len)
    {
        if (EWOULDBLOCK == errno)
            warn(errno, "Dropped signal %d", aSigNum);
        goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static void
caughtSignal_(int aSigNum)
{
    SIGNAL_CONTEXT
    ({
        int signalFd = sSignalFd_;

        debug(1, "queued signal %d", aSigNum);

        if (writeSignal_(signalFd, aSigNum))
        {
            if (EWOULDBLOCK != errno)
                terminate(
                    errno,
                    "Unable to queue signal %d on fd %d", aSigNum, signalFd);
        }
    });
}

static int
watchSignals(const struct Pipe *aSigPipe)
{
    int rc = -1;

    sSignalFd_ = aSigPipe->mWrFile->mFd;

    if (nonblockingFd(sSignalFd_))
        goto Finally;

    for (unsigned ix = 0; NUMBEROF(sWatchedSignals_) > ix; ++ix)
    {
        struct SignalWatch *watchedSig = sWatchedSignals_ + ix;

        int          sigNum     = watchedSig->mSigNum;
        sighandler_t sigHandler = signal(sigNum, caughtSignal_);

        if (SIG_ERR == sigHandler)
            goto Finally;

        watchedSig->mSigHandler = sigHandler;
        watchedSig->mWatched    = true;
    }

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            for (unsigned ix = 0; NUMBEROF(sWatchedSignals_) > ix; ++ix)
            {
                struct SignalWatch *watchedSig = sWatchedSignals_ + ix;

                if (watchedSig->mWatched)
                {
                    int          sigNum     = watchedSig->mSigNum;
                    sighandler_t sigHandler = watchedSig->mSigHandler;

                    signal(sigNum, sigHandler);

                    watchedSig->mWatched = false;
                }
            }
        }
    });

    return rc;
}

static int
unwatchSignals(void)
{
    int rc  = 0;
    int err = 0;

    for (unsigned ix = 0; NUMBEROF(sWatchedSignals_) > ix; ++ix)
    {
        struct SignalWatch *watchedSig = sWatchedSignals_ + ix;

        int          sigNum     = watchedSig->mSigNum;
        sighandler_t sigHandler = watchedSig->mSigHandler;

        if (SIG_ERR == signal(sigNum, sigHandler) && ! rc)
        {
            rc  = -1;
            err = errno;
        }

        watchedSig->mWatched = false;
    }

    sSignalFd_ = -1;

    if (rc)
        errno = err;

    return rc;
}

static void
deadChild_(int aSigNum)
{
    SIGNAL_CONTEXT
    ({
        debug(1, "queued dead child");

        if (writeSignal_(sDeadChildFd_, aSigNum))
        {
            if (EBADF != errno && EWOULDBLOCK != errno)
                terminate(
                    errno,
                    "Unable to indicate dead child");
        }
    });
}

static int
watchChildren(const struct Pipe *aTermPipe)
{
    int rc = -1;

    sDeadChildFd_ = aTermPipe->mWrFile->mFd;

    if (nonblockingFd(sDeadChildFd_))
        goto Finally;

    if (SIG_ERR == signal(SIGCHLD, deadChild_))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static int
unwatchChildren()
{
    sDeadChildFd_ = -1;

    return SIG_ERR == signal(SIGCHLD, SIG_DFL) ? -1 : 0;
}

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

    if (closeFileDescriptor(self->mFile))
        goto Finally;

    if (closePathName(&self->mPathName))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static pid_t
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
static int
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
static int
acquireWriteLockPidFile(struct PidFile *self)
{
    return lockPidFile_(self, LOCK_EX, "exclusive");
}

/* -------------------------------------------------------------------------- */
static int
acquireReadLockPidFile(struct PidFile *self)
{
    return lockPidFile_(self, LOCK_SH, "shared");
}

/* -------------------------------------------------------------------------- */
static int
createPidFile(struct PidFile *self, const char *aFileName)
{
    int rc = -1;

    if (openPidFile_(self, aFileName))
        goto Finally;

    /* Check if the pidfile already exists, and if the process that
     * it names is still running.
     */

    if (createFileDescriptor(
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

        if (closeFileDescriptor(self->mFile))
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
        if (createFileDescriptor(
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
static int
openPidFile(struct PidFile *self, const char *aFileName)
{
    int rc = -1;

    if (openPidFile_(self, aFileName))
        goto Finally;

    if (createFileDescriptor(
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
static int
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
static int
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

    if (closeFileDescriptor(self->mFile))
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
static int
writePidFile(struct PidFile *self, pid_t aPid)
{
    ensure(0 < aPid);

    return 0 > dprintf(self->mFile->mFd, "%jd\n", (intmax_t) aPid) ? -1 : 0;
}

/* -------------------------------------------------------------------------- */
static pid_t
runChild(
    char                    **aCmd,
    const struct StdFdFiller *aStdFdFiller,
    const struct SocketPair  *aTetherPipe,
    const struct Pipe        *aTermPipe,
    const struct Pipe        *aSigPipe)
{
    pid_t rc = -1;

    /* Both the parent and child share the same signal handler configuration.
     * In particular, no custom signal handlers are configured, so
     * signals delivered to either will likely caused them to terminate.
     *
     * This is safe because that would cause one of end the termPipe
     * to close, and the other end will eventually notice. */

    pid_t childPid = forkProcess(
        gOptions.mSetPgid
        ? ForkProcessSetProcessGroup
        : ForkProcessShareProcessGroup);

    if (-1 == childPid)
        goto Finally;

    if (childPid)
    {
        debug(0, "running child process %jd", (intmax_t) childPid);
    }
    else
    {
        debug(0, "starting child process");

        struct StdFdFiller stdFdFiller = *aStdFdFiller;
        struct SocketPair  tetherPipe  = *aTetherPipe;
        struct Pipe        termPipe    = *aTermPipe;
        struct Pipe        sigPipe     = *aSigPipe;

        /* Unwatch the signals so that the child process will be
         * responsive to signals from the parent. Note that the parent
         * will wait for the child to synchronise before sending it
         * signals, so that there is no race here. */

        if (unwatchSignals())
            terminate(
                errno,
                "Unable to remove watch from signals");

        /* Close the StdFdFiller in case this will free up stdin, stdout or
         * stderr. The remaining operations will close the remaining
         * unwanted file descriptors. */

        if (closeStdFdFiller(&stdFdFiller))
            terminate(
                errno,
                "Unable to close stdin, stdout and stderr fillers");

        if (closePipe(&termPipe))
            terminate(
                errno,
                "Unable to close termination pipe");

        if (closePipe(&sigPipe))
            terminate(
                errno,
                "Unable to close signal pipe");

        /* Wait until the parent has created the pidfile. This
         * invariant can be used to determine if the pidfile
         * is really associated with the process possessing
         * the specified pid. */

        debug(0, "synchronising child process");

        if (closeFd(&tetherPipe.mParentFile->mFd))
            terminate(
                errno,
                "Unable to close tether pipe fd %d",
                tetherPipe.mParentFile->mFd);

        RACE
        ({
            while (true)
            {
                char buf[1];

                switch (read(tetherPipe.mChildFile->mFd, buf, 1))
                {
                default:
                        break;
                case -1:
                    if (EINTR == errno)
                        continue;
                    terminate(
                        errno,
                        "Unable to read tether to synchronise child");
                    break;

                case 0:
                    _exit(1);
                    break;
                }

                break;
            }
        });

        char tetherArg[sizeof(int) * CHAR_BIT + 1];

        do
        {
            if (gOptions.mTether)
            {
                int tetherFd = *gOptions.mTether;

                if (0 > tetherFd)
                    tetherFd = tetherPipe.mChildFile->mFd;

                sprintf(tetherArg, "%d", tetherFd);

                if (gOptions.mName)
                {
                    bool useEnv = isupper(gOptions.mName[0]);

                    for (unsigned ix = 1; useEnv && gOptions.mName[ix]; ++ix)
                    {
                        unsigned char ch = gOptions.mName[ix];

                        if ( ! isupper(ch) && ! isdigit(ch) && ch != '_')
                            useEnv = false;
                    }

                    if (useEnv)
                    {
                        if (setenv(gOptions.mName, tetherArg, 1))
                            terminate(
                                errno,
                                "Unable to set environment variable '%s'",
                                gOptions.mName);
                    }
                    else
                    {
                        /* Start scanning from the first argument, leaving
                         * the command name intact. */

                        char *matchArg = 0;

                        for (unsigned ix = 1; aCmd[ix]; ++ix)
                        {
                            matchArg = strstr(aCmd[ix], gOptions.mName);

                            if (matchArg)
                            {
                                char replacedArg[
                                    strlen(aCmd[ix])       -
                                    strlen(gOptions.mName) +
                                    strlen(tetherArg)      + 1];

                                sprintf(replacedArg,
                                        "%.*s%s%s",
                                        matchArg - aCmd[ix],
                                        aCmd[ix],
                                        tetherArg,
                                        matchArg + strlen(gOptions.mName));

                                aCmd[ix] = strdup(replacedArg);

                                if ( ! aCmd[ix])
                                    terminate(
                                        errno,
                                        "Unable to duplicate '%s'",
                                        replacedArg);
                                break;
                            }
                        }

                        if ( ! matchArg)
                            terminate(
                                0,
                                "Unable to find matching argument '%s'",
                                gOptions.mName);
                    }
                }

                if (tetherFd == tetherPipe.mChildFile->mFd)
                    break;

                if (dup2(tetherPipe.mChildFile->mFd, tetherFd) != tetherFd)
                    terminate(
                        errno,
                        "Unable to dup tether pipe fd %d to fd %d",
                        tetherPipe.mChildFile->mFd,
                        tetherFd);
            }

            if (closeFd(&tetherPipe.mChildFile->mFd))
                terminate(
                    errno,
                    "Unable to close tether pipe fd %d",
                    tetherPipe.mChildFile->mFd);
        } while (0);

        debug(0, "child process synchronised");

        /* The child process does not close the process lock because it
         * might need to emit a diagnostic if execvp() fails. Rely on
         * O_CLOEXEC to close the underlying file descriptors. */

        execvp(aCmd[0], aCmd);
        terminate(
            errno,
            "Unable to execute '%s'", aCmd[0]);
    }

    rc = childPid;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
reapChild(pid_t aChildPid)
{
    int status;

    if (wait(&status) != aChildPid)
        terminate(
            errno,
            "Unable to reap child pid '%jd'",
            (intmax_t) aChildPid);

    return status;
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
static struct ExitCode
cmdRunCommand(char **aCmd)
{
    ensure(aCmd);

    /* The instance of the StdFdFiller guarantees that any further file
     * descriptors that are opened will not be mistaken for stdin,
     * stdout or stderr. */

    struct StdFdFiller stdFdFiller;

    if (createStdFdFiller(&stdFdFiller))
        terminate(
            errno,
            "Unable to create stdin, stdout, stderr filler");

    struct SocketPair tetherPipe;
    if (createSocketPair(&tetherPipe))
        terminate(
            errno,
            "Unable to create tether pipe");

    struct Pipe termPipe;
    if (createPipe(&termPipe))
        terminate(
            errno,
            "Unable to create termination pipe");

    struct Pipe sigPipe;
    if (createPipe(&sigPipe))
        terminate(
            errno,
            "Unable to create signal pipe");

    if (watchSignals(&sigPipe))
        terminate(
            errno,
            "Unable to add watch on signals");

    if (watchChildren(&termPipe))
        terminate(
            errno,
            "Unable to add watch on child process termination");

    /* Only identify the watchdog process after all the signal
     * handlers have been installed. The functional tests can
     * use this as an indicator that the watchdog is ready to
     * run the child process. */

    if (gOptions.mIdentify)
        RACE
        ({
            if (-1 == dprintf(STDOUT_FILENO, "%jd\n", (intmax_t) getpid()))
                terminate(
                    errno,
                    "Unable to print parent pid");
        });

    pid_t childPid = runChild(aCmd,
                              &stdFdFiller,
                              &tetherPipe, &termPipe, &sigPipe);
    if (-1 == childPid)
        terminate(
            errno,
            "Unable to fork child");

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
            pid = childPid; break;
        }

        pidFile = &pidFile_;

        for (int zombie = -1; zombie; )
        {
            if (0 < zombie)
            {
                if (closePidFile(pidFile))
                    terminate(
                        errno,
                        "Cannot close pid file '%s'", pidFileName);
            }

            if (createPidFile(pidFile, pidFileName))
                terminate(
                    errno,
                    "Cannot create pid file '%s'", pidFileName);

            /* It is not possible to create the pidfile and acquire a flock
             * as an atomic operation. The flock can only be acquired after
             * the pidfile exists. Since this newly created pidfile is empty,
             * it resembles an closed pidfile, and in the intervening time,
             * another process might have removed it and replaced it with
             * another. */

            if (acquireWriteLockPidFile(pidFile))
                terminate(
                    errno,
                    "Cannot acquire write lock on pid file '%s'", pidFileName);

            zombie = zombiePidFile(pidFile);

            if (0 > zombie)
                terminate(
                    errno,
                    "Unable to obtain status of pid file '%s'", pidFileName);

            debug(0, "discarding zombie pid file '%s'", pidFileName);
        }

        /* Ensure that the mtime of the pidfile is later than the
         * start time of the child process, if that process exists. */

        struct timespec childStartTime = findProcessStartTime(pid);

        if (UTIME_OMIT == childStartTime.tv_nsec)
        {
            terminate(
                errno,
                "Unable to obtain status of pid %jd", (intmax_t) pid);
        }
        else if (UTIME_NOW != childStartTime.tv_nsec)
        {
            debug(0,
                "child process mtime %jd.%09ld",
                (intmax_t) childStartTime.tv_sec, childStartTime.tv_nsec);

            struct stat pidFileStat;

            while (true)
            {
                if (fstat(pidFile->mFile->mFd, &pidFileStat))
                    terminate(
                        errno,
                        "Cannot obtain status of pid file '%s'", pidFileName);

                struct timespec pidFileTime = pidFileStat.st_mtim;

                debug(0,
                    "pid file mtime %jd.%09ld",
                    (intmax_t) pidFileTime.tv_sec, pidFileTime.tv_nsec);

                if (pidFileTime.tv_sec > childStartTime.tv_sec)
                    break;

                if (pidFileTime.tv_sec  == childStartTime.tv_sec &&
                    pidFileTime.tv_nsec >  childStartTime.tv_nsec)
                    break;

                if ( ! pidFileTime.tv_nsec)
                    pidFileTime.tv_nsec = 900 * 1000 * 1000;

                for (long usResolution = 1; ; usResolution *= 10)
                {
                    if (pidFileTime.tv_nsec % (1000 * usResolution))
                    {
                        /* OpenGroup says that the argument to usleep(3)
                         * must be less than 1e6. */

                        ensure(usResolution);
                        ensure(1000 * 1000 >= usResolution);

                        --usResolution;

                        debug(0, "delay for %ldus", usResolution);

                        if (usleep(usResolution) && EINTR != errno)
                            terminate(
                                errno,
                                "Unable to sleep for %ldus", usResolution);

                        break;
                    }
                }

                /* Mutate the data in the pidfile so that the mtime
                 * and ctime will be updated. */

                if (1 != write(pidFile->mFile->mFd, "\n", 1))
                    terminate(
                        errno,
                        "Unable to write to pid file '%s'", pidFileName);

                if (ftruncate(pidFile->mFile->mFd, 0))
                    terminate(
                        errno,
                        "Unable to truncate pid file '%s'", pidFileName);
            }
        }

        if (writePidFile(pidFile, pid))
            terminate(
                errno,
                "Cannot write to pid file '%s'", pidFileName);

        /* The pidfile was locked on creation, and now that it
         * is completely initialised, it is ok to release
         * the flock. */

        if (releaseLockPidFile(pidFile))
            terminate(
                errno,
                "Cannot unlock pid file '%s'", pidFileName);
    }

    /* The creation time of the child process is earlier than
     * the creation time of the pidfile. With the pidfile created,
     * release the waiting child process. */

    if (gOptions.mIdentify)
        RACE
        ({
            if (-1 == dprintf(STDOUT_FILENO, "%jd\n", (intmax_t) childPid))
                terminate(
                    errno,
                    "Unable to print child pid");
        });

    RACE
    ({
        if (1 != write(tetherPipe.mParentFile->mFd, "", 1))
            terminate(
                errno,
                "Unable to synchronise child process");
    });

    /* With the child process launched, close the instance of StdFdFiller
     * so that stdin, stdout and stderr become available for manipulation
     * and will not be closed multiple times. */

    if (closeStdFdFiller(&stdFdFiller))
        terminate(
            errno,
            "Unable to close stdin, stdout and stderr fillers");

    if (STDIN_FILENO != dup2(tetherPipe.mParentFile->mFd, STDIN_FILENO))
        terminate(
            errno,
            "Unable to dup tether pipe to stdin");

    if (closeSocketPair(&tetherPipe))
        warn(
            errno,
            "Unable to close tether pipe");

    if (gOptions.mTether)
    {
        /* Non-blocking IO on stdout is required so that the event loop
         * remains responsive, otherwise the event loop will likely block
         * when writing each buffer in its entirety. */

        if (nonblockingFd(STDOUT_FILENO))
        {
            if (EBADF == errno)
                gOptions.mQuiet = true;
            else
                terminate(
                    errno,
                    "Unable to enable non-blocking writes to stdout");
        }
    }

#if 1
    if (cleanseFileDescriptors())
        terminate(
            errno,
            "Unable to cleanse file descriptors");
#else
    const int whiteListFds[] =
    {
        pidFile ? pidFile->mFile->mFd : -1,
        pidFile ? pidFile->mPathName.mDirFile->mFd : -1,
        sProcessLock ? sProcessLock->mFile->mFd : -1,
        sProcessLock ? sProcessLock->mPathName->mDirFile->mFd : -1,
        termPipe.mRdFile->mFd,
        termPipe.mWrFile->mFd,
        sigPipe.mRdFile->mFd,
        sigPipe.mWrFile->mFd,
        STDIN_FILENO,
        STDOUT_FILENO,
        STDERR_FILENO,
    };

    purgeFds(whiteListFds, NUMBEROF(whiteListFds));
#endif

    enum PollFdKind
    {
        POLL_FD_STDIN,
        POLL_FD_STDOUT,
        POLL_FD_CHILD,
        POLL_FD_SIGNAL,
        POLL_FD_COUNT
    };

    unsigned pollInputEvents  = POLLPRI | POLLRDHUP | POLLIN;
    unsigned pollOutputEvents = POLLOUT | POLLHUP   | POLLNVAL;

    struct pollfd pollfds[POLL_FD_COUNT] =
    {
        [POLL_FD_CHILD] = {
            .fd = termPipe.mRdFile->mFd, .events = pollInputEvents },
        [POLL_FD_SIGNAL] = {
            .fd = sigPipe.mRdFile->mFd,  .events = pollInputEvents },
        [POLL_FD_STDOUT] = {
            .fd = STDOUT_FILENO,  .events = 0 },
        [POLL_FD_STDIN] = {
            .fd = STDIN_FILENO,   .events = ! gOptions.mTether
                                            ? 0 : pollInputEvents },
    };

    char buffer[8192];

    char *bufend = buffer;
    char *bufptr = buffer;

    bool deadChild = false;

    struct ChildSignalPlan
    {
        pid_t mPid;
        int   mSig;
    };

    struct ChildSignalPlan sharedPgrpPlan[] =
    {
        { childPid, SIGTERM },
        { childPid, SIGKILL },
        { 0 }
    };

    struct ChildSignalPlan ownPgrpPlan[] =
    {
        {  childPid, SIGTERM },
        { -childPid, SIGTERM },
        { -childPid, SIGKILL },
        { 0 }
    };

    const struct ChildSignalPlan *childSignalPlan =
        gOptions.mSetPgid ? ownPgrpPlan : sharedPgrpPlan;

    int timeout = gOptions.mTimeout;

    timeout = timeout ? timeout * 1000 : -1;

    do
    {
        int rc = poll(pollfds, NUMBEROF(pollfds), timeout);

        if (-1 == rc)
        {
            if (EINTR == errno)
                continue;

            terminate(
                errno,
                "Unable to poll for activity");
        }

        /* The poll(2) call will mark POLLNVAL, POLLERR or POLLHUP
         * no matter what the caller has subscribed for. Only pay
         * attention to what was subscribed. */

        for (unsigned ix = 0; NUMBEROF(pollfds) > ix; ++ix)
            pollfds[ix].revents &= pollfds[ix].events;

        if (pollfds[POLL_FD_STDIN].revents)
        {
            debug(1, "poll stdin 0x%x", pollfds[POLL_FD_STDIN].revents);

            ensure(pollfds[POLL_FD_STDIN].events);

            /* The poll(2) call should return positive non-zero if
             * any events are returned. This is a defensive measure
             * against buggy implementations since the next if-clause
             * depends on this being right. */

            rc = 1;

            ssize_t bytes;

            do
                bytes = read(STDIN_FILENO, buffer, sizeof(buffer));
            while (-1 == bytes && EINTR == errno);

            debug(1, "read stdin %zd", bytes);

            if (-1 == bytes)
            {
                if (ECONNRESET == errno)
                    bytes = 0;
                else
                    terminate(
                        errno,
                        "Unable to read from tether pipe");
            }

            /* If the child has closed its end of the tether, the watchdog
             * will read EOF on the tether. Continue running the event
             * loop until the child terminates. */

            if ( ! bytes)
            {
                pollfds[POLL_FD_STDOUT].events = 0;
                pollfds[POLL_FD_STDIN].events  = 0;
            }
            else
            {
                if (sizeof(buffer) < bytes)
                    terminate(
                        errno,
                        "Read returned value %zd which exceeds buffer size",
                        bytes);

                if ( ! gOptions.mQuiet)
                {
                    bufptr = buffer;
                    bufend = bufptr + bytes;

                    pollfds[POLL_FD_STDOUT].events = pollOutputEvents;
                    pollfds[POLL_FD_STDIN].events  = 0;
                }
            }
        }

        /* If a timeout is expected and a timeout occurred, and the
         * event loop was waiting for data from the child process,
         * then declare the child terminated. */

        if ( ! rc && -1 == timeout)
        {
            debug(0, "timeout after %d", timeout);

            pid_t pidNum = childSignalPlan->mPid;
            int   sigNum = childSignalPlan->mSig;

            if (childSignalPlan[1].mPid)
                ++childSignalPlan;

            warn(
                0,
                "Killing unresponsive child pid %jd with signal %d",
                (intmax_t) pidNum,
                sigNum);

            if (kill(pidNum, sigNum) && ESRCH != errno)
                terminate(
                    errno,
                    "Unable to kill child pid %jd with signal %d",
                    (intmax_t) pidNum,
                    sigNum);
        }

        if (pollfds[POLL_FD_STDOUT].revents)
        {
            debug(1, "poll stdout 0x%x", pollfds[POLL_FD_STDOUT].revents);

            ensure(pollfds[POLL_FD_STDOUT].events);

            ssize_t bytes;

            do
                bytes = write(STDOUT_FILENO, bufptr, bufend - bufptr);
            while (-1 == bytes && EINTR == errno);

            debug(1, "wrote stdout %zd", bytes);

            if (-1 == bytes)
            {
                if (EWOULDBLOCK != errno)
                    terminate(
                        errno,
                        "Unable to write to stdout");
                bytes = 0;
            }

            /* Once all the data that was previously read has been
             * transferred, switch the event loop to waiting for
             * more input. */

            bufptr += bytes;

            if (bufptr == bufend)
            {
                pollfds[POLL_FD_STDOUT].events = 0;
                pollfds[POLL_FD_STDIN].events  = pollInputEvents;
            }
        }

        /* Propagate signals to the child process. Signals are queued
         * by the local signal handler to the inherent race in the
         * fork() idiom:
         *
         *     pid_t childPid = fork();
         *
         * The fork() completes before childPid can be assigned. This
         * event loop only runs after the fork() is complete and
         * any signals received before the fork() will be queued for
         * delivery. */

        if (pollfds[POLL_FD_SIGNAL].revents)
        {
            debug(1, "poll signal 0x%x", pollfds[POLL_FD_CHILD].revents);

            ssize_t       len;
            unsigned char sigNum;

            do
                len = read(sigPipe.mRdFile->mFd, &sigNum, 1);
            while (-1 == len && EINTR == errno);

            if (-1 == len)
                terminate(
                    errno,
                    "Unable to read signal from queue");

            if ( ! len)
                terminate(
                    0,
                    "Signal queue closed unexpectedly");

            if (kill(childPid, sigNum))
            {
                if (ESRCH != childPid)
                    warn(
                        errno,
                        "Unable to deliver signal %d to child pid %jd",
                        sigNum,
                        (intmax_t) childPid);
            }
        }

        /* Record when the child has terminated, but do not exit
         * the event loop until all the IO has been flushed. */

        if (pollfds[POLL_FD_CHILD].revents)
        {
            debug(1, "poll child 0x%x", pollfds[POLL_FD_CHILD].revents);

            ensure(pollfds[POLL_FD_CHILD].events);

            pollfds[POLL_FD_CHILD].events = 0;
            deadChild = true;
        }

    } while ( ! deadChild ||
                pollfds[POLL_FD_STDOUT].events ||
                pollfds[POLL_FD_STDIN].events);

    if (unwatchSignals())
        terminate(
            errno,
            "Unable to remove watch from signals");

    if (unwatchChildren())
        terminate(
            errno,
            "Unable to remove watch on child process termination");

    if (closePipe(&sigPipe))
        terminate(
            errno,
            "Unable to close signal pipe");

    if (closePipe(&termPipe))
        terminate(
            errno,
            "Unable to close termination pipe");

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

    debug(0, "reaping child pid %jd", (intmax_t) childPid);

    int status = reapChild(childPid);

    debug(0, "reaped child pid %jd status %d", (intmax_t) childPid, status);

    return extractProcessExitStatus(status);
}

/* -------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    if (initProcess(argv[0]))
        terminate(
            errno,
            "Unable to initialise process state");

#if 0
    sArg0     = argv[0];
    sTimeBase = monotonicTime();

    srandom(getpid());

    struct ProcessLock processLock;

    if (createProcessLock(&processLock))
        terminate(
            errno,
            "Unable to create process lock");

    sProcessLock = &processLock;
#endif

    struct ExitCode exitCode;

    {
        char **cmd = parseOptions(argc, argv);

        if ( ! cmd && gOptions.mPidFile)
            exitCode = cmdPrintPidFile(gOptions.mPidFile);
        else
            exitCode = cmdRunCommand(cmd);
    }

    if (exitProcess())
        terminate(
            errno,
            "Unable to finalise process state");

#if 0
    sProcessLock = 0;

    if (closeProcessLock(&processLock))
        terminate(
            errno,
            "Unable to close process lock");
#endif

    return exitCode.mStatus;
}

/* -------------------------------------------------------------------------- */
