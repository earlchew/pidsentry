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

#include "process_.h"
#include "macros_.h"
#include "pathname_.h"
#include "fd_.h"
#include "file_.h"
#include "pipe_.h"
#include "test_.h"
#include "error_.h"
#include "timekeeping_.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#include <sys/file.h>
#include <sys/wait.h>

#include <signal.h>
#include <sys/time.h>

struct ProcessLock
{
    struct PathName  mPathName_;
    struct PathName *mPathName;
    struct File      mFile_;
    struct File     *mFile;
    int              mLock;
};

static struct ProcessLock  sProcessLock_[2];
static struct ProcessLock *sProcessLock[2];
static unsigned            sActiveProcessLock;

static unsigned              sInit;
static unsigned              sSigContext;
static sigset_t              sSigSet;
static const char           *sArg0;
static const char           *sProgramName;
static struct MonotonicTime  sTimeBase;

/* -------------------------------------------------------------------------- */
static struct sigaction sSigPipeAction =
{
    .sa_handler = SIG_ERR,
};

int
ignoreProcessSigPipe(void)
{
    int rc = -1;

    struct sigaction prevAction;
    struct sigaction pipeAction =
    {
        .sa_handler = SIG_IGN,
    };

    if (sigaction(SIGPIPE, &pipeAction, &prevAction))
        goto Finally;

    sSigPipeAction = prevAction;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static int
resetProcessSigPipe_(void)
{
    int rc = -1;

    if (SIG_ERR != sSigPipeAction.sa_handler ||
        (sSigPipeAction.sa_flags & SA_SIGINFO))
    {
        if (sigaction(SIGPIPE, &sSigPipeAction, 0))
            goto Finally;

        sSigPipeAction.sa_handler = SIG_ERR;
        sSigPipeAction.sa_flags = 0;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
resetProcessSigPipe(void)
{
    return resetProcessSigPipe_();
}

/* -------------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------------- */
static int sDeadChildRdFd_ = -1;
static int sDeadChildWrFd_ = -1;

static void
deadChild_(int aSigNum)
{
    ++sSigContext;
    {
        int deadChildRdFd = sDeadChildRdFd_;
        int deadChildWrFd = sDeadChildWrFd_;

        debug(1,
              "queued dead child to fd %d from fd %d",
              deadChildRdFd, deadChildWrFd);

        if (writeSignal_(deadChildWrFd, aSigNum))
        {
            if (EBADF != errno && EWOULDBLOCK != errno)
                terminate(
                    errno,
                    "Unable to indicate dead child to fd %d", deadChildWrFd);
        }
    }
    --sSigContext;
}

static int
resetProcessChildrenWatch_(void)
{
    sDeadChildWrFd_ = -1;
    sDeadChildRdFd_ = -1;

    struct sigaction childAction =
    {
        .sa_handler = SIG_DFL,
    };

    return sigaction(SIGCHLD, &childAction, 0);
}

int
watchProcessChildren(const struct Pipe *aTermPipe)
{
    int rc = -1;

    /* It is ok to mark the termination pipe non-blocking because this
     * file descriptor is not shared with any other process. */

    sDeadChildRdFd_ = aTermPipe->mRdFile->mFd;
    sDeadChildWrFd_ = aTermPipe->mWrFile->mFd;

    if (nonblockingFd(sDeadChildRdFd_))
        goto Finally;

    if (nonblockingFd(sDeadChildWrFd_))
        goto Finally;

    struct sigaction childAction =
    {
        .sa_handler = deadChild_,
    };

    if (sigaction(SIGCHLD, &childAction, 0))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
unwatchProcessChildren(void)
{
    return resetProcessChildrenWatch_();
}

/* -------------------------------------------------------------------------- */
static int sClockTickRdFd_ = -1;
static int sClockTickWrFd_ = -1;

static void
clockTick_(int aSigNum)
{
    ++sSigContext;
    {
        int clockTickRdFd = sClockTickRdFd_;
        int clockTickWrFd = sClockTickWrFd_;

        debug(1,
              "queued clock tick to fd %d from fd %d",
              clockTickRdFd, clockTickWrFd);

        if (writeSignal_(clockTickWrFd, aSigNum))
        {
            if (EBADF != errno && EWOULDBLOCK != errno)
                terminate(
                    errno,
                    "Unable to indicate clock tick to fd %d", clockTickWrFd);
        }
    }
    --sSigContext;
}

static int
resetProcessClockWatch_(void)
{
    int rc = -1;

    if (-1 != sClockTickWrFd_)
    {
        sClockTickWrFd_ = -1;
        sClockTickRdFd_ = -1;

        struct itimerval disableClock =
            {
                .it_value    = { .tv_sec = 0 },
                .it_interval = { .tv_sec = 0 },
            };

        if (setitimer(ITIMER_REAL, &disableClock, 0))
            goto Finally;

        struct sigaction clockAction =
            {
                .sa_handler = SIG_DFL,
            };

        if (sigaction(SIGALRM, &clockAction, 0))
            goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
watchProcessClock(const struct Pipe    *aClockPipe,
                  const struct timeval *aClockPeriod)
{
    int rc = -1;

    /* It is ok to mark the clock pipe non-blocking because this
     * file descriptor is not shared with any other process. */

    sClockTickRdFd_ = aClockPipe->mRdFile->mFd;
    sClockTickWrFd_ = aClockPipe->mWrFile->mFd;

    if (nonblockingFd(sClockTickRdFd_))
        goto Finally;

    if (nonblockingFd(sClockTickWrFd_))
        goto Finally;

    struct sigaction clockAction =
    {
        .sa_handler = clockTick_,
    };

    if (sigaction(SIGALRM, &clockAction, 0))
        goto Finally;

    /* Make sure that there are not timers already running. The
     * interface only supports one clock instance. */

    struct itimerval clockTimer;

    if (getitimer(ITIMER_REAL, &clockTimer))
        goto Finally;

    if (clockTimer.it_value.tv_sec || clockTimer.it_value.tv_usec)
    {
        errno = EPERM;
        goto Finally;
    }

    /* Ensure that the selected clock period is non-zero. A zero
     * clock period would mean that clock is disabled. */

    if ( ! aClockPeriod->tv_sec && ! aClockPeriod->tv_usec)
    {
        errno = EINVAL;
        goto Finally;
    }

    clockTimer.it_value    = *aClockPeriod;
    clockTimer.it_interval = *aClockPeriod;

    if (setitimer(ITIMER_REAL, &clockTimer, 0))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
unwatchProcessClock(void)
{
    return resetProcessClockWatch_();
}

/* -------------------------------------------------------------------------- */
static int sSignalRdFd_ = -1;
static int sSignalWrFd_ = -1;

static struct SignalWatch {
    int              mSigNum;
    struct sigaction mSigAction;
    bool             mWatched;
} sWatchedSignals_[] =
{
    { SIGHUP },
    { SIGINT },
    { SIGQUIT },
    { SIGTERM },
};

static void
caughtSignal_(int aSigNum)
{
    ++sSigContext;
    {
        int signalRdFd = sSignalRdFd_;
        int signalWrFd = sSignalWrFd_;

        debug(1,
              "queued signal %d from fd %d to fd %d",
              aSigNum,
              signalWrFd,
              signalRdFd);

        if (writeSignal_(signalWrFd, aSigNum))
        {
            if (EWOULDBLOCK != errno)
                terminate(
                    errno,
                    "Unable to queue signal %d on fd %d", aSigNum, signalWrFd);
        }
    }
    --sSigContext;
}

int
watchProcessSignals(const struct Pipe *aSigPipe)
{
    int rc = -1;

    /* It is ok to mark the signal pipe non-blocking because this
     * file descriptor is not shared with any other process. */

    sSignalRdFd_ = aSigPipe->mRdFile->mFd;
    sSignalWrFd_ = aSigPipe->mWrFile->mFd;

    if (nonblockingFd(sSignalRdFd_))
        goto Finally;

    if (nonblockingFd(sSignalWrFd_))
        goto Finally;

    for (unsigned ix = 0; NUMBEROF(sWatchedSignals_) > ix; ++ix)
    {
        struct SignalWatch *watchedSig = sWatchedSignals_ + ix;

        struct sigaction watchAction =
        {
            .sa_handler = caughtSignal_,
        };

        if (sigaction(watchedSig->mSigNum,
                      &watchAction,
                      &watchedSig->mSigAction))
            goto Finally;

        watchedSig->mWatched = true;
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
                    sigaction(watchedSig->mSigNum,
                              &watchedSig->mSigAction,
                              0);

                    watchedSig->mWatched = false;
                }
            }
        }
    });

    return rc;
}

static int
resetProcessSignalsWatch_(void)
{
    int rc  = 0;
    int err = 0;

    for (unsigned ix = 0; NUMBEROF(sWatchedSignals_) > ix; ++ix)
    {
        struct SignalWatch *watchedSig = sWatchedSignals_ + ix;

        if (watchedSig->mWatched)
        {
            if (sigaction(watchedSig->mSigNum,
                          &watchedSig->mSigAction,
                          0))
            {
                if ( ! rc)
                {
                    rc  = -1;
                    err = errno;
                }

                watchedSig->mWatched = false;
            }
        }
    }

    sSignalWrFd_ = -1;
    sSignalRdFd_ = -1;

    if (rc)
        errno = err;

    return rc;
}

int
unwatchProcessSignals(void)
{
    return resetProcessSignalsWatch_();
}

/* -------------------------------------------------------------------------- */
static int
resetSignals_(void)
{
    int rc = -1;

    if (resetProcessClockWatch_())
        goto Finally;

    if (resetProcessSignalsWatch_())
        goto Finally;

    if (resetProcessChildrenWatch_())
        goto Finally;

    if (resetProcessSigPipe_())
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
void
initProcessDirName(struct ProcessDirName *self, pid_t aPid)
{
    sprintf(self->mDirName, PROCESS_DIRNAME_FMT_, (intmax_t) aPid);
}

/* -------------------------------------------------------------------------- */
struct timespec
findProcessStartTime(pid_t aPid)
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
createProcessLock_(struct ProcessLock *self)
{
    int rc = -1;

    self->mFile     = 0;
    self->mLock     = LOCK_UN;
    self->mPathName = 0;

    static const char pathNameFmt[] = "/proc/%jd/.";

    char pathName[sizeof(pathNameFmt) + sizeof(pid_t) * CHAR_BIT];

    if (-1 == sprintf(pathName, pathNameFmt, (intmax_t) getpid()))
        goto Finally;

    if (createPathName(&self->mPathName_, pathName))
        goto Finally;
    self->mPathName = &self->mPathName_;

    if (createFile(
            &self->mFile_,
            openPathName(self->mPathName, O_RDONLY | O_CLOEXEC, 0)))
        goto Finally;
    self->mFile = &self->mFile_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            closeFile(self->mFile);
            closePathName(self->mPathName);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
closeProcessLock_(struct ProcessLock *self)
{
    int rc = -1;

    if (self)
    {
        if (closeFile(self->mFile))
            goto Finally;

        if (closePathName(self->mPathName))
            goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
lockProcessLock_(struct ProcessLock *self)
{
    int rc = -1;

    if (self)
    {
        ensure(LOCK_UN == self->mLock);

        if (lockFile(self->mFile, LOCK_EX, 0))
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
unlockProcessLock_(struct ProcessLock *self)
{
    int rc = -1;

    if (self)
    {
        ensure(LOCK_UN != self->mLock);

        if (unlockFile(self->mFile))
            goto Finally;

        self->mLock = LOCK_UN;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
Process_init(const char *aArg0)
{
    int rc = -1;

    if (1 == ++sInit)
    {
        ensure( ! sProcessLock[sActiveProcessLock]);

        sArg0     = aArg0;
        sTimeBase = monotonicTime();

        sProgramName = strrchr(sArg0, '/');
        sProgramName = sProgramName ? sProgramName + 1 : sArg0;

        srandom(getpid());

        if (sigprocmask(SIG_SETMASK, 0, &sSigSet))
            goto Finally;

        if (createProcessLock_(&sProcessLock_[sActiveProcessLock]))
            goto Finally;
        sProcessLock[sActiveProcessLock] = &sProcessLock_[sActiveProcessLock];

        if (Error_init())
            goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
Process_exit(void)
{
    int rc = -1;

    if (0 == --sInit)
    {
        struct ProcessLock *processLock = sProcessLock[sActiveProcessLock];

        ensure(processLock);

        if (Error_exit())
            goto Finally;

        if (closeProcessLock_(processLock))
            goto Finally;
        sProcessLock[sActiveProcessLock] = 0;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
lockProcessLock(void)
{
    int rc = -1;

    if (sSigContext)
    {
        errno = EWOULDBLOCK;
        goto Finally;
    }

    struct ProcessLock *processLock = sProcessLock[sActiveProcessLock];

    if (processLock && lockProcessLock_(processLock))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
unlockProcessLock(void)
{
    int rc = -1;

    if (sSigContext)
    {
        errno = EWOULDBLOCK;
        goto Finally;
    }

    struct ProcessLock *processLock = sProcessLock[sActiveProcessLock];

    if (processLock && unlockProcessLock_(processLock))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
ownProcessLockPath(void)
{
    struct ProcessLock *processLock = sProcessLock[sActiveProcessLock];

    return processLock ? processLock->mPathName->mFileName : 0;
}

/* -------------------------------------------------------------------------- */
int
reapProcess(pid_t aPid, int *aStatus)
{
    int rc = -1;

    if (-1 == aPid || ! aPid)
        goto Finally;

    pid_t pid;

    do
    {
        pid = waitpid(aPid, aStatus, __WALL);

        if (-1 == pid && EINTR != errno)
            goto Finally;

    } while (pid != aPid);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
pid_t
forkProcess(enum ForkProcessOption aOption)
{
    ensure(
        sProcessLock[sActiveProcessLock] == &sProcessLock_[sActiveProcessLock]);

    pid_t rc = -1;

    /* The child process needs separate process lock. It cannot share
     * the process lock with the parent because flock(2) distinguishes
     * locks by file descriptor table entry. Create the process lock
     * in the parent first so that the child process is guaranteed to
     * be able to synchronise its messages. */

    unsigned activeProcessLock   = 0 + sActiveProcessLock;
    unsigned inactiveProcessLock = 1 - activeProcessLock;

    ensure(NUMBEROF(sProcessLock_) > activeProcessLock);
    ensure(NUMBEROF(sProcessLock_) > inactiveProcessLock);

    ensure( ! sProcessLock[inactiveProcessLock]);

    if (createProcessLock_(&sProcessLock_[inactiveProcessLock]))
        goto Finally;
    sProcessLock[inactiveProcessLock] = &sProcessLock_[inactiveProcessLock];

    /* If required, temporarily block all signals so that the child will not
     * receive signals which it cannot handle. */

    sigset_t signalSet;
    sigset_t prevSignalSet;

    if (sigfillset(&signalSet))
        goto Finally;

    if (sigemptyset(&prevSignalSet))
        goto Finally;

    if (sigprocmask(SIG_BLOCK, &signalSet, &prevSignalSet))
        goto Finally;

    /* Note that the fork() will complete and launch the child process
     * before the child pid is recorded in the local variable. This
     * is an important consideration for propagating signals to
     * the child process. */

    pid_t childPid;

    RACE
    ({
        childPid = fork();
    });

    switch (childPid)
    {
    default:
        /* Forcibly set the process group of the child to avoid
         * the race that would occur if only the child attempts
         * to set its own process group */

        if (ForkProcessSetProcessGroup == aOption)
        {
            if (setpgid(childPid, childPid))
                goto Finally;
        }

        if (sigprocmask(SIG_SETMASK, &prevSignalSet, 0))
            goto Finally;

        break;

    case -1:
        break;

    case 0:
        /* Switch the process lock first in case the child process
         * needs to emit diagnostic messages so that the messages
         * will not be garbled. */

        sActiveProcessLock  = inactiveProcessLock;
        inactiveProcessLock = activeProcessLock;

        if (ForkProcessSetProcessGroup == aOption)
        {
            if (setpgid(0, 0))
                terminate(
                    errno,
                    "Unable to set process group");
        }

        /* Reset all the signals so that the child will not attempt
         * to catch signals. After that, reset the signal mask so
         * that the child will receive signals. */

        if (resetSignals_())
            terminate(
                errno,
                "Unable to reset signal handlers");

        if (sigprocmask(SIG_SETMASK, &sSigSet, 0))
            terminate(
                errno,
                "Unable to reset signal set");

        break;
    }

    rc = childPid;

Finally:

    FINALLY
    ({
        if (closeProcessLock_(sProcessLock[inactiveProcessLock]))
            terminate(
                errno,
                "Unable to close process lock");
        sProcessLock[inactiveProcessLock] = 0;
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
ownProcessName(void)
{
    return sProgramName;
}

/* -------------------------------------------------------------------------- */
struct ExitCode
extractProcessExitStatus(int aStatus)
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
        debug(0, "child exited %d", WEXITSTATUS(aStatus));

        exitCode.mStatus = WEXITSTATUS(aStatus);
    }
    else if (WIFSIGNALED(aStatus))
    {
        debug(0, "child terminated by signal %d", WTERMSIG(aStatus));

        exitCode.mStatus = 128 + WTERMSIG(aStatus);
        if (255 < exitCode.mStatus)
            exitCode.mStatus = 255;
    }

    debug(0, "exit code %d", exitCode.mStatus);

    return exitCode;
}

/* -------------------------------------------------------------------------- */
struct Duration
ownProcessElapsedTime(void)
{
    return duration(
        NanoSeconds(monotonicTime().monotonic.ns - sTimeBase.monotonic.ns));
}

/* -------------------------------------------------------------------------- */
struct MonotonicTime
ownProcessBaseTime(void)
{
    return sTimeBase;
}

/* -------------------------------------------------------------------------- */
static int
rankProcessFd_(const void *aLhs, const void *aRhs)
{
    int lhs = * (const int *) aLhs;
    int rhs = * (const int *) aRhs;

    if (lhs < rhs) return -1;
    if (lhs > rhs) return +1;
    return 0;
}

struct ProcessFdWhiteList
{
    int     *mList;
    unsigned mLen;
};

static int
countProcessFds_(void *aNumFds, const struct File *aFile)
{
    unsigned *numFds = aNumFds;

    ++(*numFds);

    return 0;
}

static int
enumerateProcessFds_(void *aWhiteList, const struct File *aFile)
{
    struct ProcessFdWhiteList *whiteList = aWhiteList;

    whiteList->mList[whiteList->mLen++] = aFile->mFd;

    return 0;
}

int
purgeProcessOrphanedFds(void)
{
    /* Remove all the file descriptors that were not created explicitly by the
     * process itself, with the exclusion of stdin, stdout and stderr. */

    int rc = -1;

    /* Count the number of file descriptors explicitly created by the
     * process itself in order to correctly size the array to whitelist
     * the explicitly created file dsscriptors. Include stdin, stdout
     * and stderr in the whitelist by default.
     *
     * Note that stdin, stdout and stderr might in fact already be
     * represented in the file descriptor list, so the resulting
     * algorithms must be capable of handing duplicates. Force that
     * scenario to be covered by explicitly repeating each of them here. */

    int stdfds[] =
    {
        STDIN_FILENO,  STDIN_FILENO,
        STDOUT_FILENO, STDOUT_FILENO,
        STDERR_FILENO, STDERR_FILENO,
    };

    unsigned numFds = NUMBEROF(stdfds);

    walkFileList(&numFds, countProcessFds_);

    /* Create the whitelist of file descriptors by copying the fds
     * from each of the explicitly created file descriptors. */

    int whiteList[numFds + 1];

    {
        struct ProcessFdWhiteList fdWhiteList =
        {
            .mList = whiteList,
            .mLen  = 0,
        };

        struct rlimit noFile;

        if (getrlimit(RLIMIT_NOFILE, &noFile))
            goto Finally;

        ensure(numFds < noFile.rlim_cur);

        fdWhiteList.mList[numFds] = noFile.rlim_cur;

        {
            for (unsigned jx = 0; NUMBEROF(stdfds) > jx; ++jx)
                fdWhiteList.mList[fdWhiteList.mLen++] = stdfds[jx];

            walkFileList(&fdWhiteList, enumerateProcessFds_);

            ensure(fdWhiteList.mLen == numFds);

            for (unsigned jx = 0; numFds > jx; ++jx)
                ensure(fdWhiteList.mList[jx] < fdWhiteList.mList[numFds]);
        }
    }

    /* Walk the file descriptor space and close all the file descriptors,
     * skipping those mentioned in the whitelist. */

    debug(0, "purging %d fds", whiteList[numFds]);

    qsort(whiteList, NUMBEROF(whiteList), sizeof(whiteList[0]), rankProcessFd_);

    for (int fd = 0, wx = 0; ; ++fd)
    {
        while (0 > whiteList[wx])
            ++wx;

        if (fd != whiteList[wx])
        {
            int closedFd = fd;

            if (closeFd(&closedFd) && EBADF != errno)
                goto Finally;
        }
        else
        {
            debug(0, "not closing fd %d", fd);

            if (NUMBEROF(whiteList) == ++wx)
                break;

            while (whiteList[wx] == fd)
                ++wx;

        }
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
