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

#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <assert.h>
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

#define NUMBEROF(x) (sizeof((x))/sizeof((x)[0]))

#define STRINGIFY_(x) #x
#define STRINGIFY(x)  STRINGIFY_(x)

#define FINALLY(...)      \
    do                    \
    {                     \
        int err_ = errno; \
        __VA_ARGS__;      \
        errno = err_;     \
    } while (0)

#define DEFAULT_TIMEOUT 30

/* TODO
 *
 * Push fd to environment, or argument on command line
 * Do not kill self, and exit 255
 * Under test, pause after fork()
 * Use --stdout semantics by default, instead allow --fd [ N ]
 * Kill SIGTERM on timeout, then SIGKILL
 * cmdRunCommand() is too big, break it up
 */

static const char sUsage[] =
"usage : %s [ options ] cmd ...\n"
"        %s { --pidfile file | -p file }\n"
"\n"
"options:\n"
"  --debug | -d\n"
"      Print debug information.\n"
"  --pid N | -P N\n"
"      Specify value to write to pidfile. Set N to 0 to use pid of child,\n"
"      set N to -1 to use pid of watchdog, otherwise use N as the pid\n"
"      of the child. [Default: Use pid of child]\n"
"  --pidfile file | -p file\n"
"      Write the pid of the child to the specified file, and remove the\n"
"      file when the child terminates. [Default: No pidfile]\n"
"  --stdout | -s\n"
"      Tether child using stdout and copy stdout content from child.\n"
"      [Default: Do not use stdout as tether]\n"
"  --timeout N | -t N\n"
"      Specify the timeout N in seconds for activity on tether from\n"
"      the child process. Set N to 0 to avoid imposing any timeout at\n"
"      all. [Default: N = " STRINGIFY(DEFAULT_TIMEOUT) "]\n"
"  --untethered | -u\n"
"      Run child process without a tether and only watch for termination.\n"
"      [Default: Tether child process]\n"
"";

static const char sShortOptions[] =
    "dP:p:sTt:u";

static struct option sLongOptions[] =
{
    { "debug",      0, 0, 'd' },
    { "pid",        1, 0, 'P' },
    { "pidfile",    1, 0, 'p' },
    { "stdout",     0, 0, 's' },
    { "test",       0, 0, 'T' },
    { "timeout",    1, 0, 't' },
    { "untethered", 0, 0, 'u' },
    { 0 },
};

static struct
{
    char           *mPidFile;
    unsigned        mDebug;
    bool            mTest;
    bool            mStdout;
    unsigned short  mTimeout;
    bool            mUntethered;
    pid_t           mPid;
} sOptions;

static char    *sArg0;
static char   **sCmd;
static pid_t    sPid;
static uint64_t sTimeBase;

struct SocketPair
{
    int mParentFd;
    int mChildFd;
};

struct Pipe
{
    int mRdFd;
    int mWrFd;
};

struct StdFdFiller
{
    int mFd[3];
};

struct PidFile
{
    char *mFileName;
    char *mDirName_;
    char *mDirName;
    char *mBaseName_;
    char *mBaseName;
    int   mDirFd;
    int   mFd;
    int   mLock;
};

struct ExitCode
{
    int mStatus;
};

/* -------------------------------------------------------------------------- */
static uint64_t monotonicTime(void);

static void
showUsage()
{
    dprintf(STDERR_FILENO, sUsage, sArg0, sArg0);
    _exit(1);
}

static void
showMessage(
    int aErrCode,
    const char *aFile, unsigned aLine,
    const char *aFmt, va_list aArgs)
{
    FINALLY
    ({
        uint64_t elapsed    = monotonicTime() - sTimeBase;
        uint64_t elapsed_s = elapsed / (1000 * 1000 * 1000);
        uint64_t elapsed_m;
        uint64_t elapsed_h;

        elapsed_h = elapsed_s / (60 * 60);
        elapsed_m = elapsed_s % (60 * 60) / 60;
        elapsed_s = elapsed_s % (60 * 60) % 60;

        dprintf(
            STDERR_FILENO,
            "%s: [%03" PRIu64 ":%02" PRIu64 ":%02" PRIu64" %jd %s:%u] ",
            sArg0,
            elapsed_h, elapsed_m, elapsed_s,
            (intmax_t) sPid,
            aFile, aLine);
        vdprintf(STDERR_FILENO, aFmt, aArgs);
        if (aErrCode)
            dprintf(STDERR_FILENO, " - errno %d\n", aErrCode);
        else
            dprintf(STDERR_FILENO, "\n");
    });
}

#define breadcrumb() \
    debug_(__FILE__, __LINE__, ".")

#define debug(aLevel, ...)        \
    if (aLevel < sOptions.mDebug) \
        debug_(__FILE__, __LINE__, ## __VA_ARGS__)

static void
debug_(
    const char *aFile, unsigned aLine,
    const char *aFmt, ...)
{
    FINALLY
    ({
        va_list args;

        va_start(args, aFmt);
        showMessage(0, aFile, aLine, aFmt, args);
        va_end(args);
    });
}

#define warn(aErrCode, ...) \
    warn_((aErrCode), __FILE__, __LINE__, ## __VA_ARGS__)

static void
warn_(
    int aErrCode,
    const char *aFile, unsigned aLine,
    const char *aFmt, ...)
{
    FINALLY
    ({
        va_list args;

        va_start(args, aFmt);
        showMessage(aErrCode, aFile, aLine, aFmt, args);
        va_end(args);
    });
}

#define terminate(aErrCode, ...) \
    terminate_((aErrCode), __FILE__, __LINE__, ## __VA_ARGS__)

static void
terminate_(
    int aErrCode,
    const char *aFile, unsigned aLine,
    const char *aFmt, ...)
{
    FINALLY
    ({
        va_list args;

        va_start(args, aFmt);
        showMessage(aErrCode, aFile, aLine, aFmt, args);
        va_end(args);
        _exit(1);
    });
}

/* -------------------------------------------------------------------------- */
static uint64_t
monotonicTime(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        terminate(
            errno,
            "Unable to fetch monotonic time");

    uint64_t ns = ts.tv_sec;

    return ns * 1000 * 1000 * 1000 + ts.tv_nsec;
}

/* -------------------------------------------------------------------------- */
static bool
testAction(void)
{
    /* If test mode has been enabled, choose to activate a test action
     * a small percentage of the time. */

    return sOptions.mTest && 3 > random() % 10;
}

/* -------------------------------------------------------------------------- */
static struct timespec
earliestTime(const struct timespec *aLhs, const struct timespec *aRhs)
{
    if (aLhs->tv_sec < aRhs->tv_sec)
        return *aLhs;

    if (aLhs->tv_sec  == aRhs->tv_sec &&
        aLhs->tv_nsec <  aRhs->tv_nsec)
    {
        return *aLhs;
    }

    return *aRhs;
}

/* -------------------------------------------------------------------------- */
static unsigned long long
parseUnsigned(const char *aArg)
{
    unsigned long long arg;

    do
    {
        if (isdigit((unsigned char) *aArg))
        {
            char *endptr = 0;

            errno = 0;
            arg   = strtoull(aArg, &endptr, 10);

            if (!*endptr && (ULLONG_MAX != arg || ERANGE != errno))
            {
                errno = 0;
                break;
            }
        }

        errno = ERANGE;
        arg   = ULLONG_MAX;

    } while (0);

    return arg;
}

/* -------------------------------------------------------------------------- */
static void
parseOptions(int argc, char **argv)
{
    int pidFileOnly = 0;

    sArg0 = argv[0];

    sOptions.mTimeout = DEFAULT_TIMEOUT;

    while (1)
    {
        int longOptIndex = 0;

        int opt = getopt_long(
                argc, argv, sShortOptions, sLongOptions, &longOptIndex);

        if (-1 == opt)
            break;

        switch (opt)
        {
        default:
            terminate(0, "Unrecognised option %d ('%c')", opt, opt);
            break;

        case '?':
            showUsage();
            break;

        case 'd':
            ++sOptions.mDebug;
            break;

        case 'P':
            pidFileOnly = -1;
            if ( ! strcmp(optarg, "-1"))
                sOptions.mPid = -1;
            else
            {
                do
                {
                    unsigned long long pid = parseUnsigned(optarg);

                    if ( ! errno)
                    {
                        sOptions.mPid = pid;

                        if ( !   (sOptions.mPid - pid) &&
                             0 <= sOptions.mPid)
                        {
                            break;
                        }
                    }
                    terminate(0, "Badly formed pid - '%s'", optarg);
                } while (0);
            }
            break;

        case 'p':
            pidFileOnly = pidFileOnly ? pidFileOnly : 1;
            sOptions.mPidFile = optarg;
            break;

        case 's':
            pidFileOnly = -1;
            sOptions.mStdout = true;
            break;

        case 'T':
            sOptions.mTest = true;
            break;

        case 't':
            pidFileOnly = -1;
            do
            {
                unsigned long long timeout = parseUnsigned(optarg);

                if ( ! errno)
                {
                    sOptions.mTimeout = timeout;

                    if ( !   (sOptions.mTimeout - timeout) &&
                         0 <= sOptions.mTimeout)
                    {
                        break;
                    }
                }
                terminate(0, "Badly formed timeout - '%s'", optarg);
            } while (0);
            break;

        case 'u':
            pidFileOnly = -1;
            sOptions.mUntethered = true;
            break;
        }
    }

    if (0 >= pidFileOnly)
    {
        if (optind >= argc)
            terminate(0, "Missing command for execution");
    }

    if (optind < argc)
        sCmd = argv + optind;
}

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
static int
closeFdPair(int *aFd1, int *aFd2)
{
    return closeFd(aFd1) || closeFd(aFd2) ? -1 : 0;
}

/* -------------------------------------------------------------------------- */
static bool
stdFd(int aFd)
{
    return STDIN_FILENO == aFd || STDOUT_FILENO == aFd || STDERR_FILENO == aFd;
}

/* -------------------------------------------------------------------------- */
static int
rankFd_(const void *aLhs, const void *aRhs)
{
    int lhs = * (const int *) aLhs;
    int rhs = * (const int *) aRhs;

    if (lhs < rhs) return -1;
    if (lhs > rhs) return +1;
    return 0;
}

static int
purgeFds(const int *aWhiteList, unsigned aWhiteListLen)
{
    int rc = -1;

    int whiteList[aWhiteListLen + 1];

    for (unsigned ix = 0; ix < aWhiteListLen; ++ix)
        whiteList[ix] = aWhiteList[ix];

    struct rlimit noFile;

    if (getrlimit(RLIMIT_NOFILE, &noFile))
        goto Finally;

    whiteList[aWhiteListLen] = noFile.rlim_cur;

    debug(0, "purging %d fds", whiteList[aWhiteListLen]);

    qsort(whiteList, NUMBEROF(whiteList), sizeof(whiteList[0]), rankFd_);

    for (unsigned fd = 0, wx = 0; ; ++fd)
    {
        while (0 > whiteList[wx])
            ++wx;

        if (fd != whiteList[wx])
        {
            if (close(fd) && EBADF != errno)
                goto Finally;
        }
        else
        {
            debug(0, "not closing fd %d", fd);
            if (NUMBEROF(whiteList) == ++wx)
                break;
        }
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
nonblockingFd(int aFd)
{
    long flags = fcntl(aFd, F_GETFL, 0);

    return -1 == flags ? -1 : fcntl(aFd, F_SETFL, flags | O_NONBLOCK);
}

/* -------------------------------------------------------------------------- */
static struct timespec
processStartTime(pid_t aPid)
{
    static const char procFileFmt[] = "/proc/%jd";

    char procFileName[sizeof(procFileFmt) + sizeof(aPid) * CHAR_BIT];

    sprintf(procFileName, procFileFmt, (intmax_t) aPid);

    struct timespec startTime = { 0 };

    struct stat procStatus;

    if (stat(procFileName, &procStatus))
        startTime.tv_nsec = ENOENT == errno ?  UTIME_NOW : UTIME_OMIT;
    else
        startTime = earliestTime(&procStatus.st_mtim, &procStatus.st_ctim);

    return startTime;
}

/* -------------------------------------------------------------------------- */
static int
createSocketPair(struct SocketPair *self)
{
    int rc = -1;

    int fds[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
        goto Finally;

    if (-1 == fds[0] || -1 == fds[1])
        goto Finally;

    self->mParentFd = fds[0];
    self->mChildFd  = fds[1];

    assert( ! stdFd(self->mParentFd));
    assert( ! stdFd(self->mChildFd));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
closeSocketPair(struct SocketPair *self)
{
    return closeFdPair(&self->mParentFd, &self->mChildFd);
}

/* -------------------------------------------------------------------------- */
static int
createPipe(struct Pipe *self)
{
    int fds[2];

    if (pipe(fds))
        return -1;

    if (-1 == fds[0] || -1 == fds[1])
        return -1;

    self->mRdFd = fds[0];
    self->mWrFd = fds[1];

    assert( ! stdFd(self->mRdFd));
    assert( ! stdFd(self->mWrFd));

    return 0;
}

/* -------------------------------------------------------------------------- */
static int
closePipe(struct Pipe *self)
{
    return closeFdPair(&self->mRdFd, &self->mWrFd);
}

/* -------------------------------------------------------------------------- */
static int
createStdFdFiller(struct StdFdFiller *self)
{
    int rc = -1;

    for (unsigned ix = 0; NUMBEROF(self->mFd) > ix; ++ix)
        self->mFd[ix] = -1;

    if (pipe(self->mFd))
        goto Finally;

    if (-1 == self->mFd[0] || -1 == self->mFd[1])
        goto Finally;

    self->mFd[2] = dup(self->mFd[0]);

    if (-1 == self->mFd[2])
        goto Finally;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            for (unsigned ix = 0; NUMBEROF(self->mFd) > ix; ++ix)
            {
                if (-1 != self->mFd[ix])
                    close(self->mFd[ix]);
                self->mFd[ix] = -1;
            }
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
closeStdFdFiller(struct StdFdFiller *self)
{
    int rc  = 0;
    int err = 0;

    for (unsigned ix = 0; NUMBEROF(self->mFd) > ix; ++ix)
    {
        if (close(self->mFd[ix]) && ! rc)
        {
            rc = -1;
            err = errno;
        }
        self->mFd[ix] = -1;
    }

    if (rc)
        errno = err;

    return rc;
}

/* -------------------------------------------------------------------------- */
static int sDeadChildFd_ = -1;
static int sSignalFd_    = -1;

static const int sWatchedSignals_[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };

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
        {
            warn(errno, "Dropped signal %d", aSigNum);
            goto Finally;
        }

        terminate(errno, "Unable to queue signal %d", aSigNum);
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static void
caughtSignal_(int aSigNum)
{
    if (writeSignal_(sSignalFd_, aSigNum))
    {
        if (EWOULDBLOCK != errno)
            terminate(
                errno,
                "Unable to queue signal %d", aSigNum);
    }
}

static int
watchSignals(const struct Pipe *aSigPipe)
{
    int rc = -1;

    sSignalFd_ = aSigPipe->mWrFd;

    if (nonblockingFd(sSignalFd_))
        goto Finally;

    for (unsigned ix = 0; NUMBEROF(sWatchedSignals_) > ix; ++ix)
    {
        int sigNum = sWatchedSignals_[ix];

        if (SIG_ERR == signal(sigNum, caughtSignal_))
            goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static void
deadChild_(int aSigNum)
{
    (void) writeSignal_(sDeadChildFd_, aSigNum);

    /* Ignore any errors here because unwatchChildren() will deregister
     * the file descriptor immediately before unsubscribing the
     * signal handler. */
}

static int
watchChildren(const struct Pipe *aTermPipe)
{
    int rc = -1;

    sDeadChildFd_ = aTermPipe->mWrFd;

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

    self->mFileName  = 0;
    self->mDirName_  = 0;
    self->mBaseName_ = 0;
    self->mDirFd     = -1;
    self->mFd        = -1;
    self->mLock      = LOCK_UN;

    self->mFileName = strdup(aFileName);
    if ( ! self->mFileName)
        goto Finally;

    self->mDirName_ = strdup(self->mFileName);
    if ( ! self->mDirName_)
        goto Finally;

    self->mBaseName_ = strdup(self->mFileName);
    if ( ! self->mBaseName_)
        goto Finally;

    self->mDirName  = strdup(dirname(self->mDirName_));
    if ( ! self->mDirName)
        goto Finally;

    self->mBaseName = strdup(basename(self->mBaseName_));
    if ( ! self->mBaseName)
        goto Finally;

    breadcrumb();

    self->mDirFd = open(self->mDirName, O_RDONLY);
    if (-1 == self->mDirFd)
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
closePidFile_(struct PidFile *self)
{
    FINALLY
    ({
        free(self->mDirName_);
        free(self->mBaseName_);

        free(self->mDirName);
        free(self->mBaseName);
        free(self->mFileName);

        if (-1 != self->mDirFd)
            close(self->mDirFd);
        if (-1 != self->mFd)
            close(self->mFd);
    });
}

/* -------------------------------------------------------------------------- */
static pid_t
readPidFile(const struct PidFile *self)
{
    pid_t pid;
    char  buf[sizeof(pid) * CHAR_BIT + sizeof("\n")];
    char *bufptr = buf;
    char *bufend = bufptr + sizeof(buf);

    assert(LOCK_UN != self->mLock);

    while (true)
    {
        if (bufptr == bufend)
            return 0;

        ssize_t len;

        len = read(self->mFd, bufptr, bufend - bufptr);

        if (-1 == len)
        {
            if (EINTR == errno)
                continue;

            return -1;
        }

        if ( ! len)
        {
            *bufptr++ = '\n';
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

                unsigned long long pid_ = parseUnsigned(buf);

                pid = pid_;

                if ((pid_ - pid) || 0 >= pid)
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

                if (fstat(self->mFd, &fdStatus))
                    return -1;

                struct timespec fdTime   = earliestTime(&fdStatus.st_mtim,
                                                        &fdStatus.st_ctim);
                struct timespec procTime = processStartTime(pid);

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
    assert(LOCK_UN != self->mLock);

    int locked = self->mLock;

    self->mLock = LOCK_UN;

    int rc = flock(self->mFd, LOCK_UN);

    FINALLY
    ({
        if (rc)
            self->mLock = locked;
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
lockPidFile_(struct PidFile *self, int aLock, const char *aLockType)
{
    debug(0, "lock %s '%s'", aLockType, self->mFileName);

    assert(LOCK_UN != aLock);
    assert(LOCK_UN == self->mLock);

    int rc = flock(self->mFd, aLock);

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

    self->mFd = openat(self->mDirFd, self->mBaseName, O_RDONLY | O_NOFOLLOW);
    if (-1 == self->mFd)
    {
        if (ENOENT != errno)
            goto Finally;
    }
    else
    {
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

        debug(0, "removing existing pidfile '%s'", self->mFileName);

        if (unlinkat(self->mDirFd, self->mBaseName, 0) && ENOENT != errno)
            goto Finally;

        if (releaseLockPidFile(self))
            goto Finally;

        if (close(self->mFd))
            goto Finally;

        self->mFd = -1;
    }

    /* Open the pidfile using lock file semantics for writing, but
     * with readonly permissions. Use of lock file semantics ensures
     * that the watchdog will be the owner of the pid file, and
     * readonly permissions dissuades other processes from modifying
     * the content. */

    breadcrumb();

    self->mFd = openat(self->mDirFd,
                       self->mBaseName,
                       O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                       S_IRUSR | S_IRGRP | S_IROTH);
    if (-1 == self->mFd)
        goto Finally;

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
static int
openPidFile(struct PidFile *self, const char *aFileName)
{
    int rc = -1;

    if (openPidFile_(self, aFileName))
        goto Finally;

    breadcrumb();

    self->mFd = openat(self->mDirFd, self->mBaseName, O_RDONLY | O_NOFOLLOW);
    if (-1 == self->mFd)
        goto Finally;

    breadcrumb();

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
static int
zombiePidFile(const struct PidFile *self)
{
    int rc = -1;

    /* The pidfile has become a zombie if it was deleted, and no longer
     * exists, or replaced by a different file different file in the
     * same directory. */

    struct stat fileStatus;

    if (fstatat(
            self->mDirFd, self->mBaseName, &fileStatus, AT_SYMLINK_NOFOLLOW))
    {
        if (ENOENT == errno)
            rc = 1;
        goto Finally;
    }

    struct stat fdStatus;

    if (fstat(self->mFd, &fdStatus))
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
    assert(LOCK_UN != self->mLock);

    int rc = -1;

    int flags = fcntl(self->mFd, F_GETFL, 0);

    if (-1 == flags)
        goto Finally;

    if (O_RDONLY != (flags & O_ACCMODE))
    {
        if (ftruncate(self->mFd, 0))
            goto Finally;
    }

    if (close(self->mFd))
        goto Finally;

    if (close(self->mDirFd))
        goto Finally;

    free(self->mBaseName_);
    free(self->mDirName_);

    free(self->mBaseName);
    free(self->mDirName);
    free(self->mFileName);

    self->mBaseName_ = 0;
    self->mDirName_  = 0;

    self->mFd     = -1;
    self->mDirFd  = -1;
    self->mLock   = LOCK_UN;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
writePidFile(struct PidFile *self, pid_t aPid)
{
    assert(0 < aPid);

    return 0 > dprintf(self->mFd, "%jd\n", (intmax_t) aPid) ? -1 : 0;
}

/* -------------------------------------------------------------------------- */
static pid_t
runChild(
    const struct StdFdFiller *aStdFdFiller,
    const struct SocketPair  *aTetherPipe,
    const struct Pipe        *aTermPipe,
    const struct Pipe        *aSigPipe)
{
    /* Note that the fork() will complete and launch the child process
     * before the child pid is recorded in the local variable. This
     * is an important consideration for propagating signals to
     * the child process. */

    pid_t childPid = fork();

    if ( ! childPid)
    {
        sPid = getpid();

        struct StdFdFiller stdFdFiller = *aStdFdFiller;
        struct SocketPair  tetherPipe  = *aTetherPipe;
        struct Pipe        termPipe    = *aTermPipe;
        struct Pipe        sigPipe     = *aSigPipe;

        /* Wait until the parent has created the pidfile. This
         * invariant can be used to determine if the pidfile
         * is really associated with the process possessing
         * the specified pid. */

        debug(0, "synchronising child process");

        if (closeFd(&tetherPipe.mParentFd))
            terminate(
                errno,
                "Unable to close tether pipe fd %d", tetherPipe.mParentFd);

        while (true)
        {
            char buf[1];

            switch (read(tetherPipe.mChildFd, buf, 1))
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

        if (sOptions.mUntethered)
        {
            if (closeFd(&tetherPipe.mChildFd))
                terminate(
                    errno,
                    "Unable to close tether pipe fd %d", tetherPipe.mChildFd);
        }
        else if (sOptions.mStdout)
        {
            if (STDOUT_FILENO != dup2(tetherPipe.mChildFd, STDOUT_FILENO))
                terminate(
                    errno,
                    "Unable to dup tether pipe fd %d", tetherPipe.mChildFd);
            if (STDOUT_FILENO != tetherPipe.mChildFd || sOptions.mUntethered)
            {
                if (closeFd(&tetherPipe.mChildFd))
                    terminate(
                        errno,
                        "Unable to close tether pipe fd %d",
                        tetherPipe.mChildFd);
            }
        }

        debug(0, "child process synchronised");

        /* Close the StdFdFiller in case this will free up stdin, stdout or
         * stderr. The remaining operations will close the remaining
         * unwanted file descriptors. */

        breadcrumb();
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

        execvp(sCmd[0], sCmd);
        terminate(
            errno,
            "Unable to execute '%s'", sCmd[0]);
    }

    return childPid;
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
exitProcess(int aStatus)
{
    struct ExitCode exitCode = { 127 };

    if (WIFEXITED(aStatus))
        exitCode.mStatus = WEXITSTATUS(aStatus);

    if (WIFSIGNALED(aStatus))
    {
        int sigNum = WTERMSIG(aStatus);

        if (kill(sPid, sigNum))
            warn(errno, "Unable to deliver signal %d'", sigNum);
    }

    return exitCode;
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
        breadcrumb();
        if (closePidFile(&pidFile))
            terminate(
                errno,
                "Unable to close pid file '%s'", aFileName);
    });

    return exitCode;
}

/* -------------------------------------------------------------------------- */
static struct ExitCode
cmdRunCommand()
{
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

    pid_t childPid = runChild(&stdFdFiller,
                              &tetherPipe, &termPipe, &sigPipe);
    if (-1 == childPid)
        terminate(
            errno,
            "Unable to fork child");

    struct PidFile  pidFile_;
    struct PidFile *pidFile = 0;

    if (sOptions.mPidFile)
    {
        const char *pidFileName = sOptions.mPidFile;

        pid_t pid = sOptions.mPid;

        switch (pid)
        {
        default:
            break;
        case -1:
            pid = sPid; break;
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

            breadcrumb();

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

        struct timespec childStartTime = processStartTime(pid);

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
                if (fstat(pidFile->mFd, &pidFileStat))
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

                        assert(usResolution);
                        assert(1000 * 1000 >= usResolution);

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

                if (1 != write(pidFile->mFd, "\n", 1))
                    terminate(
                        errno,
                        "Unable to write to pid file '%s'", pidFileName);

                if (ftruncate(pidFile->mFd, 0))
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

        breadcrumb();
        if (releaseLockPidFile(pidFile))
            terminate(
                errno,
                "Cannot unlock pid file '%s'", pidFileName);
        breadcrumb();
    }

    /* The creation time of the child process is earlier than
     * the creation time of the pidfile. With the pidfile created,
     * release the waiting child process. */

    if (1 != write(tetherPipe.mParentFd, "", 1))
        terminate(
            errno,
            "Unable to synchronise child process");

    /* With the child process launched, close the instance of StdFdFiller
     * so that stdin, stdout and stderr become available for manipulation
     * and will not be closed multiple times. */

    breadcrumb();
    if (closeStdFdFiller(&stdFdFiller))
        terminate(
            errno,
            "Unable to close stdin, stdout and stderr fillers");

    if (STDIN_FILENO != dup2(tetherPipe.mParentFd, STDIN_FILENO))
        terminate(
            errno,
            "Unable to dup tether pipe to stdin");

    if (closeSocketPair(&tetherPipe))
        warn(
            errno,
            "Unable to close tether pipe");

    const int whiteListFds[] =
    {
        pidFile ? pidFile->mFd : -1,
        pidFile ? pidFile->mDirFd : -1,
        termPipe.mRdFd,
        termPipe.mWrFd,
        sigPipe.mRdFd,
        sigPipe.mWrFd,
        STDIN_FILENO,
        STDOUT_FILENO,
        STDERR_FILENO,
    };

    purgeFds(whiteListFds, NUMBEROF(whiteListFds));

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
        [POLL_FD_STDIN] = {
            .fd = STDIN_FILENO,   .events = 0 },
        [POLL_FD_STDOUT] = {
            .fd = STDOUT_FILENO,  .events = 0 },
        [POLL_FD_CHILD] = {
            .fd = termPipe.mRdFd, .events = pollInputEvents },
        [POLL_FD_SIGNAL] = {
            .fd = sigPipe.mRdFd,  .events = pollInputEvents },
    };

    char buffer[8192];

    char *bufend = buffer;
    char *bufptr = buffer;

    bool deadChild  = false;
    int  childSig   = SIGTERM;

    if ( ! sOptions.mUntethered)
    {
        pollfds[POLL_FD_STDIN].events = pollInputEvents;

        /* Non-blocking IO on stdout is required so that the event loop
         * remains responsive. If stdout is not open, disable sOptions.mStdout
         * so that all input will be discarded. */

        if (nonblockingFd(STDOUT_FILENO))
        {
            if (errno == EBADF)
                sOptions.mStdout = false;
            else
                terminate(
                    errno,
                    "Unable to set stdout to non-blocking");
        }
    }

    int timeout = sOptions.mTimeout;

    timeout = timeout ? timeout * 1000 : -1;

    do
    {
        int rc = poll(pollfds, NUMBEROF(pollfds), timeout);

        if (-1 == rc)
            terminate(
                errno,
                "Unable to poll for activity");

        /* The poll(2) call will mark POLLNVAL, POLLERR or POLLHUP
         * no matter what the caller has subscribed for. Only pay
         * attention to what was subscribed. */

        for (unsigned ix = 0; NUMBEROF(pollfds) > ix; ++ix)
            pollfds[ix].revents &= pollfds[ix].events;

        if (pollfds[POLL_FD_STDIN].revents)
        {
            debug(1, "poll stdin 0x%x", pollfds[POLL_FD_STDIN].revents);

            assert(pollfds[POLL_FD_STDIN].events);

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
                terminate(
                    errno,
                    "Unable to read from tether pipe");

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

                if (sOptions.mStdout)
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

            warn(
                0,
                "Killing unresponsive child pid %jd with signal %d",
                (intmax_t) childPid,
                childSig);

            if (kill(childPid, childSig) && ESRCH != errno)
                terminate(
                    errno,
                    "Unable to kill child pid %jd with signal %d",
                    (intmax_t) childPid,
                    childSig);

            childSig = SIGKILL;
        }

        if (pollfds[POLL_FD_STDOUT].revents)
        {
            debug(1, "poll stdout 0x%x", pollfds[POLL_FD_STDOUT].revents);

            assert(pollfds[POLL_FD_STDOUT].events);

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
                len = read(sigPipe.mRdFd, &sigNum, 1);
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

            assert(pollfds[POLL_FD_CHILD].events);

            pollfds[POLL_FD_CHILD].events = 0;
            deadChild = true;
        }

    } while ( ! deadChild ||
                pollfds[POLL_FD_STDOUT].events ||
                pollfds[POLL_FD_STDIN].events);

    if (pidFile)
    {
        breadcrumb();
        if (acquireWriteLockPidFile(pidFile))
            terminate(
                errno,
                "Cannot lock pid file '%s'", pidFile->mFileName);

        breadcrumb();
        if (closePidFile(pidFile))
            terminate(
                errno,
                "Cannot close pid file '%s'", pidFile->mFileName);

        pidFile = 0;
    }

    debug(0, "reaping child pid %jd", (intmax_t) childPid);

    if (unwatchChildren())
        terminate(
            errno,
            "Unable to remove watch on child process termination");

    int status = reapChild(childPid);

    debug(0, "reaped child pid %jd status %d", (intmax_t) childPid, status);

    return exitProcess(status);
}

/* -------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    sPid      = getpid();
    sTimeBase = monotonicTime();

    srandom(sPid);

    parseOptions(argc, argv);

    struct ExitCode exitCode;

    if ( ! sCmd && sOptions.mPidFile)
        exitCode = cmdPrintPidFile(sOptions.mPidFile);
    else
        exitCode = cmdRunCommand();

    return exitCode.mStatus;
}

/* -------------------------------------------------------------------------- */
