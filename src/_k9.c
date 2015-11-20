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

#define RACE(...) \
    do                    \
    {                     \
        testSleep();      \
        __VA_ARGS__;      \
        testSleep();      \
    } while (0)

/* TODO
 *
 * Push tether fd to environment, or argument on command line
 * Under test, pause after fork()
 * Use --stdout semantics by default,
 *        instead allow --tether [ { D | D,S | - | -,S } ]
 *        Verify that D is actually open for writing
 *        Verify that S is not open
 * Kill SIGTERM on timeout, then SIGKILL
 * cmdRunCommand() is too big, break it up
 * Use flock to serialise messages
 * Remove sCmd
 */

static const char sUsage[] =
"usage : %s [ options ] cmd ...\n"
"        %s { --pidfile file | -p file }\n"
"\n"
"options:\n"
"  --debug | -d\n"
"      Print debug information.\n"
"  --fd N | -f N\n"
"      Tether child using file descriptor N in the child process, and\n"
"      copy received data to stdout of the watchdog. Specify N as - to\n"
"      allocate a new file descriptor. [Default: N = 1 (stdout) ].\n"
"  --identify | -i\n"
"      Print the pid of the child process on stdout before starting\n"
"      the child program. [Default: Do not print the pid of the child]\n"
"  --pid N | -P N\n"
"      Specify value to write to pidfile. Set N to 0 to use pid of child,\n"
"      set N to -1 to use the pid of the watchdog, otherwise use N as the\n"
"      pid of the child. [Default: Use the pid of child]\n"
"  --pidfile file | -p file\n"
"      Write the pid of the child to the specified file, and remove the\n"
"      file when the child terminates. [Default: No pidfile]\n"
"  --quiet | -q\n"
"      Do not copy received data from tether to stdout. This is an\n"
"      alternative to closing stdout. [Default: Copy data from tether]\n"
"  --timeout N | -t N\n"
"      Specify the timeout N in seconds for activity on tether from\n"
"      the child process. Set N to 0 to avoid imposing any timeout at\n"
"      all. [Default: N = " STRINGIFY(DEFAULT_TIMEOUT) "]\n"
"  --untethered | -u\n"
"      Run child process without a tether and only watch for termination.\n"
"      [Default: Tether child process]\n"
"";

static const char sShortOptions[] =
    "df:iP:p:qTt:u";

static struct option sLongOptions[] =
{
    { "debug",      0, 0, 'd' },
    { "fd",         1, 0, 'f' },
    { "identify",   0, 0, 'i' },
    { "pid",        1, 0, 'P' },
    { "pidfile",    1, 0, 'p' },
    { "quiet",      0, 0, 'q' },
    { "test",       0, 0, 'T' },
    { "timeout",    1, 0, 't' },
    { "untethered", 0, 0, 'u' },
    { 0 },
};

static struct
{
    char      *mPidFile;
    unsigned   mDebug;
    bool       mTest;
    bool       mQuiet;
    bool       mIdentify;
    int        mTimeout;
    int        mTetherFd;
    const int *mTether;
    pid_t      mPid;
} sOptions;

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

struct PathName
{
    char *mFileName;
    char *mDirName_;
    char *mDirName;
    char *mBaseName_;
    char *mBaseName;
    int   mDirFd;
};

struct PidFile
{
    struct PathName mPathName;
    int             mFd;
    int             mLock;
};

struct ProcessLock
{
    struct PathName mPathName;
    int             mFd;
    int             mLock;
};

struct ExitCode
{
    int mStatus;
};

static const char sProcessDirNameFmt[] = "/proc/%jd";

struct ProcessDirName
{
    char mDirName[sizeof(sProcessDirNameFmt) + sizeof(pid_t) * CHAR_BIT];
};

static char               *sArg0;
static char              **sCmd;
static pid_t               sPid;
static uint64_t            sTimeBase;
static struct ProcessLock *sProcessLock;

static int lockProcessLock(struct ProcessLock *);
static int unlockProcessLock(struct ProcessLock *);

/* -------------------------------------------------------------------------- */
static pid_t
getProcessId()
{
    /* Lazily cache the pid of this process. This is safe because the pid
     * returns the same value for all threads in the same process. */

    pid_t pid = sPid;

    if ( ! pid)
    {
        pid  = getpid();
        sPid = pid;
    }

    return pid;
}

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
        lockProcessLock(sProcessLock);

        uint64_t elapsed   = monotonicTime() - sTimeBase;
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
            (intmax_t) getProcessId(),
            aFile, aLine);
        vdprintf(STDERR_FILENO, aFmt, aArgs);
        if (aErrCode)
            dprintf(STDERR_FILENO, " - errno %d\n", aErrCode);
        else
            dprintf(STDERR_FILENO, "\n");

        unlockProcessLock(sProcessLock);
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
static void
testSleep(void)
{
    /* If test mode has been enabled, choose to sleep a short time
     * a small percentage of the time. */

    if (testAction())
        usleep(random() % (500 * 1000));
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
parseUnsignedLongLong(const char *aArg)
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
static int
parseInt(const char *aArg, int *aValue)
{
    int rc = -1;

    unsigned long long value = parseUnsignedLongLong(aArg);

    if ( ! errno)
    {
        *aValue = value;

        if ( ! (*aValue - value))
            rc = 0;
    }

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
parsePid(const char *aArg, pid_t *aValue)
{
    int rc = -1;

    unsigned long long value = parseUnsignedLongLong(aArg);

    if ( ! errno)
    {
        *aValue = value;

        if ( !   (*aValue - value) &&
             0 <= *aValue)
        {
            rc = 0;
        }
    }

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
parseOptions(int argc, char **argv)
{
    int pidFileOnly = 0;

    sOptions.mTimeout   = DEFAULT_TIMEOUT;
    sOptions.mTetherFd  = STDOUT_FILENO;
    sOptions.mTether    = &sOptions.mTetherFd;

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

        case 'f':
            pidFileOnly = -1;
            sOptions.mTether = &sOptions.mTetherFd;
            if ( ! strcmp(optarg, "-"))
            {
                sOptions.mTetherFd = -1;
            }
            else
            {
                if (parseInt(
                        optarg,
                        &sOptions.mTetherFd) || 0 > sOptions.mTetherFd)
                {
                    terminate(0, "Badly formed fd - '%s'", optarg);
                }
            }
            break;

        case 'i':
            pidFileOnly = -1;
            sOptions.mIdentify = true;
            break;

        case 'P':
            pidFileOnly = -1;
            if ( ! strcmp(optarg, "-1"))
                sOptions.mPid = -1;
            else if (parsePid(optarg, &sOptions.mPid))
                terminate(0, "Badly formed pid - '%s'", optarg);
            break;

        case 'p':
            pidFileOnly = pidFileOnly ? pidFileOnly : 1;
            sOptions.mPidFile = optarg;
            break;

        case 'q':
            pidFileOnly = -1;
            sOptions.mQuiet = true;
            break;

        case 'T':
            sOptions.mTest = true;
            break;

        case 't':
            pidFileOnly = -1;
            if (parseInt(optarg, &sOptions.mTimeout) || 0 > sOptions.mTimeout)
                terminate(0, "Badly formed timeout - '%s'", optarg);
            break;

        case 'u':
            pidFileOnly = -1;
            sOptions.mTether = 0;
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
static void
initProcessDirName(struct ProcessDirName *self, pid_t aPid)
{
    sprintf(self->mDirName, sProcessDirNameFmt, (intmax_t) aPid);
}

/* -------------------------------------------------------------------------- */
static int
createPathName(struct PathName *self, const char *aFileName)
{
    int rc = -1;

    self->mFileName  = 0;
    self->mBaseName_ = 0;
    self->mBaseName  = 0;
    self->mDirName_  = 0;
    self->mDirName   = 0;
    self->mDirFd     = -1;

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

    self->mDirFd = open(self->mDirName, O_RDONLY | O_CLOEXEC);
    if (-1 == self->mDirFd)
        goto Finally;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            free(self->mFileName);
            free(self->mBaseName_);
            free(self->mBaseName);
            free(self->mDirName_);
            free(self->mDirName);

            close(self->mDirFd);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
closePathName(struct PathName *self)
{
    int rc = -1;

    if (close(self->mDirFd))
        goto Finally;

    free(self->mFileName);
    free(self->mBaseName_);
    free(self->mBaseName);
    free(self->mDirName_);
    free(self->mDirName);

    self->mFileName  = 0;
    self->mBaseName_ = 0;
    self->mBaseName  = 0;
    self->mDirName_  = 0;
    self->mDirName   = 0;
    self->mDirFd     = -1;

    rc = 0;

Finally:

    FINALLY({});

    return 0;
}

/* -------------------------------------------------------------------------- */
static int
openPathName(struct PathName *self, int aFlags, mode_t aMode)
{
    return openat(self->mDirFd, self->mBaseName, aFlags, aMode);
}

/* -------------------------------------------------------------------------- */
static int
unlinkPathName(struct PathName *self, int aFlags)
{
    return unlinkat(self->mDirFd, self->mBaseName, aFlags);
}

/* -------------------------------------------------------------------------- */
static int
fstatPathName(const struct PathName *self, struct stat *aStat, int aFlags)
{
    return fstatat(self->mDirFd, self->mBaseName, aStat, aFlags);
}

/* -------------------------------------------------------------------------- */
static struct timespec
processStartTime(pid_t aPid)
{
    struct ProcessDirName processDirName;

    initProcessDirName(&processDirName, aPid);

    struct timespec startTime = { 0 };

    struct stat procStatus;

    if (stat(processDirName.mDirName, &procStatus))
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

    /* Close the writing end of the pipe, leaving only the reading
     * end of the pipe. Any attempt to write will fail, and any
     * attempt to read will yield EOF. */

    if (close(self->mFd[1]))
        goto Finally;

    self->mFd[1] = dup(self->mFd[0]);
    self->mFd[2] = dup(self->mFd[0]);

    if (-1 == self->mFd[1] || -1 == self->mFd[2])
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
    int signalFd = sSignalFd_;

    debug(0, "queued signal %d", aSigNum);

    if (writeSignal_(signalFd, aSigNum))
    {
        if (EWOULDBLOCK != errno)
            terminate(
                errno,
                "Unable to queue signal %d on fd %d", aSigNum, signalFd);
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
    if (writeSignal_(sDeadChildFd_, aSigNum))
    {
        if (EBADF != errno && EWOULDBLOCK != errno)
            terminate(
                errno,
                "Unable to indicate dead child");
    }
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

    self->mFd   = -1;
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

    if (-1 != self->mFd)
        if (close(self->mFd))
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

                unsigned long long pid_ = parseUnsignedLongLong(buf);

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

    testSleep();

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
lockPidFile_(struct PidFile *self, int aLock, const char *aLockType)
{
    debug(0, "lock %s '%s'", aLockType, self->mPathName.mFileName);

    assert(LOCK_UN != aLock);
    assert(LOCK_UN == self->mLock);

    testSleep();

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

    self->mFd = openPathName(&self->mPathName, O_RDONLY | O_NOFOLLOW, 0);
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

    RACE
    ({
        self->mFd = openPathName(&self->mPathName,
                                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                                 S_IRUSR | S_IRGRP | S_IROTH);
    });
    if (-1 == self->mFd)
        goto Finally;

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

    breadcrumb();

    self->mFd = openPathName(&self->mPathName, O_RDONLY | O_NOFOLLOW, 0);
    if (-1 == self->mFd)
        goto Finally;

    breadcrumb();

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

    if (closePathName(&self->mPathName))
        goto Finally;

    self->mFd   = -1;
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
    assert(0 < aPid);

    return 0 > dprintf(self->mFd, "%jd\n", (intmax_t) aPid) ? -1 : 0;
}

/* -------------------------------------------------------------------------- */
static int
createProcessLock(struct ProcessLock *self)
{
    int rc = -1;

    self->mFd   = -1;
    self->mLock = LOCK_UN;

    bool havePathName = false;
    if (createPathName(&self->mPathName, "/proc/self"))
        goto Finally;
    havePathName = true;

    self->mFd = openPathName(&self->mPathName, O_RDONLY | O_CLOEXEC, 0);
    if (-1 == self->mFd)
        goto Finally;
    breadcrumb();

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            close(self->mFd);

            if (havePathName)
                closePathName(&self->mPathName);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
closeProcessLock(struct ProcessLock *self)
{
    int rc = -1;

    if (close(self->mFd))
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
lockProcessLock(struct ProcessLock *self)
{
    int rc = -1;

    if (self)
    {
        assert(self->mLock == LOCK_UN);

        if (flock(self->mFd, LOCK_EX))
            goto Finally;

        self->mLock = LOCK_EX;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
unlockProcessLock(struct ProcessLock *self)
{
    int rc = -1;

    if (self)
    {
        assert(self->mLock != LOCK_UN);

        if (flock(self->mFd, LOCK_UN))
            goto Finally;

        self->mLock = LOCK_UN;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static pid_t
forkProcess(void)
{
    /* Note that the fork() will complete and launch the child process
     * before the child pid is recorded in the local variable. This
     * is an important consideration for propagating signals to
     * the child process.
     *
     * Reset the cached pid before the fork() so that there will not
     * be a race immediately after the fork() to correct the cached value.
     * */

    pid_t childPid;

    RACE
    ({
        sPid = 0;

        childPid = fork();

        testSleep();
    });

    return childPid;
}

/* -------------------------------------------------------------------------- */
static pid_t
runChild(
    const struct StdFdFiller *aStdFdFiller,
    const struct SocketPair  *aTetherPipe,
    const struct Pipe        *aTermPipe,
    const struct Pipe        *aSigPipe)
{
    pid_t rc = -1;

    /* The child process needs separate process lock. It cannot share
     * the process lock with the parent because flock(2) distinguishes
     * locks by file descriptor table entry. Create the process lock
     * in the parent first so that the child process is guaranteed to
     * be able to synchronise its messages. */

    struct ProcessLock processLock;

    if (createProcessLock(&processLock))
        goto Finally;

    pid_t childPid = forkProcess();

    if ( ! childPid)
    {
        struct StdFdFiller stdFdFiller = *aStdFdFiller;
        struct SocketPair  tetherPipe  = *aTetherPipe;
        struct Pipe        termPipe    = *aTermPipe;
        struct Pipe        sigPipe     = *aSigPipe;

        /* Switch the process lock first in case the child process
         * needs to emit diagnostic messages so that the messages
         * will not be garbled. */

        struct ProcessLock *prevProcessLock = sProcessLock;

        sProcessLock = &processLock;

        if (closeProcessLock(prevProcessLock))
            terminate(
                errno,
                "Unable to close process lock");

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

        /* Wait until the parent has created the pidfile. This
         * invariant can be used to determine if the pidfile
         * is really associated with the process possessing
         * the specified pid. */

        debug(0, "synchronising child process");

        if (closeFd(&tetherPipe.mParentFd))
            terminate(
                errno,
                "Unable to close tether pipe fd %d", tetherPipe.mParentFd);

        RACE
        ({
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
        });

        breadcrumb();
        do
        {
            if (sOptions.mTether)
            {
                int tetherFd = *sOptions.mTether;

                if (0 > tetherFd || tetherFd == tetherPipe.mChildFd)
                    break;

                breadcrumb();
                if (dup2(tetherPipe.mChildFd, tetherFd) != tetherFd)
                    terminate(
                        errno,
                        "Unable to dup tether pipe fd %d to fd %d",
                        tetherPipe.mChildFd,
                        tetherFd);
            }

            breadcrumb();
            if (closeFd(&tetherPipe.mChildFd))
                terminate(
                    errno,
                    "Unable to close tether pipe fd %d", tetherPipe.mChildFd);
        } while (0);

        debug(0, "child process synchronised");

        /* The child process does not close the process lock because it
         * might need to emit a diagnostic if execvp() fails. Rely on
         * O_CLOEXEC to close the underlying file descriptors. */

        execvp(sCmd[0], sCmd);
        terminate(
            errno,
            "Unable to execute '%s'", sCmd[0]);
    }

    if (closeProcessLock(&processLock))
        goto Finally;

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
exitProcess(int aStatus)
{
    /* Taking guidance from OpenGroup:
     *
     * http://pubs.opengroup.org/onlinepubs/009695399/
     *      utilities/xcu_chap02.html#tag_02_08_02
     *
     * Use exit codes above 128 to indicate signals, and codes below
     * 128 to indicate exit status. */

    struct ExitCode exitCode = { 255 };

    if (WIFEXITED(aStatus))
    {
        exitCode.mStatus = WEXITSTATUS(aStatus);
        if (128 < exitCode.mStatus)
            exitCode.mStatus = 128;
    }
    else if (WIFSIGNALED(aStatus))
    {
        exitCode.mStatus = 128 + WTERMSIG(aStatus);
        if (255 < exitCode.mStatus)
            exitCode.mStatus = 255;
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
            pid = getProcessId(); break;
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

    if (sOptions.mIdentify)
        dprintf(STDOUT_FILENO,
                "%jd\n%jd\n",
                (intmax_t) getProcessId(),
                (intmax_t) childPid);

    RACE
    ({
        if (1 != write(tetherPipe.mParentFd, "", 1))
            terminate(
                errno,
                "Unable to synchronise child process");
    });

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

    if (sOptions.mTether)
    {
        /* Non-blocking IO on stdout is required so that the event loop
         * remains responsive, otherwise the event loop will likely block
         * when writing each buffer in its entirety. */

        if (nonblockingFd(STDOUT_FILENO))
        {
            if (EBADF == errno)
                sOptions.mQuiet = true;
            else
                terminate(
                    errno,
                    "Unable to enable non-blocking writes to stdout");
        }
    }

    const int whiteListFds[] =
    {
        pidFile ? pidFile->mFd : -1,
        pidFile ? pidFile->mPathName.mDirFd : -1,
        sProcessLock ? sProcessLock->mFd : -1,
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
        [POLL_FD_CHILD] = {
            .fd = termPipe.mRdFd, .events = pollInputEvents },
        [POLL_FD_SIGNAL] = {
            .fd = sigPipe.mRdFd,  .events = pollInputEvents },
        [POLL_FD_STDOUT] = {
            .fd = STDOUT_FILENO,  .events = 0 },
        [POLL_FD_STDIN] = {
            .fd = STDIN_FILENO,   .events = ! sOptions.mTether
                                            ? 0 : pollInputEvents },
    };

    char buffer[8192];

    char *bufend = buffer;
    char *bufptr = buffer;

    bool deadChild  = false;
    int  childSig   = SIGTERM;

    int timeout = sOptions.mTimeout;

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

                if ( ! sOptions.mQuiet)
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
                "Cannot lock pid file '%s'", pidFile->mPathName.mFileName);

        breadcrumb();
        if (closePidFile(pidFile))
            terminate(
                errno,
                "Cannot close pid file '%s'", pidFile->mPathName.mFileName);

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
    sArg0     = argv[0];
    sTimeBase = monotonicTime();

    srandom(getProcessId());

    struct ProcessLock processLock;

    if (createProcessLock(&processLock))
        terminate(
            errno,
            "Unable to create process lock");

    sProcessLock = &processLock;

    parseOptions(argc, argv);

    struct ExitCode exitCode;

    if ( ! sCmd && sOptions.mPidFile)
        exitCode = cmdPrintPidFile(sOptions.mPidFile);
    else
        exitCode = cmdRunCommand();

    return exitCode.mStatus;
}

/* -------------------------------------------------------------------------- */
