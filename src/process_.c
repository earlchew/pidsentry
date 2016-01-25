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

#include "process_.h"
#include "macros_.h"
#include "pathname_.h"
#include "fd_.h"
#include "pipe_.h"
#include "test_.h"
#include "error_.h"
#include "timekeeping_.h"
#include "parse_.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <ctype.h>

#include <sys/file.h>
#include <sys/wait.h>

struct ProcessLock
{
    struct PathName  mPathName_;
    struct PathName *mPathName;
    struct File      mFile_;
    struct File     *mFile;
    int              mLock;
};

struct ProcessAppLock
{
    void *mNull;
};

static struct ProcessAppLock  sProcessAppLock;
static pthread_mutex_t        sProcessMutex = PTHREAD_MUTEX_INITIALIZER;
static __thread sigset_t      sProcessSigMask;
static struct ProcessLock     sProcessLock_[2];
static struct ProcessLock    *sProcessLock[2];
static unsigned               sActiveProcessLock;

static unsigned                     sInit;
static struct PushedProcessSigMask  sSigMask;
static const char                  *sArg0;
static const char                  *sProgramName;
static struct MonotonicTime         sTimeBase;

/* -------------------------------------------------------------------------- */
static sigset_t
filledSigSet(void)
{
    sigset_t sigset;

    if (sigfillset(&sigset))
        terminate(
            errno,
            "Unable to create filled signal set");

    return sigset;
}

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
    struct sigaction nextAction =
    {
        .sa_handler = SIG_IGN,
        .sa_mask    = filledSigSet(),
    };

    if (sigaction(SIGPIPE, &nextAction, &prevAction))
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
int
pushProcessSigMask(
    struct PushedProcessSigMask *self,
    enum ProcessSigMaskAction   aAction,
    const int                  *aSigList)
{
    int rc = -1;

    int maskAction;
    switch (aAction)
    {
    default: errno = EINVAL; goto Finally;
    case ProcessSigMaskUnblock: maskAction = SIG_UNBLOCK; break;
    case ProcessSigMaskSet:     maskAction = SIG_SETMASK; break;
    case ProcessSigMaskBlock:   maskAction = SIG_BLOCK;   break;
    }

    sigset_t sigset;

    if ( ! aSigList)
    {
        if (sigfillset(&sigset))
            goto Finally;
    }
    else
    {
        if (sigemptyset(&sigset))
            goto Finally;
        for (size_t ix = 0; aSigList[ix]; ++ix)
        {
            if (sigaddset(&sigset, aSigList[ix]))
                goto Finally;
        }
    }

    if (pthread_sigmask(maskAction, &sigset, &self->mSigSet))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
popProcessSigMask(struct PushedProcessSigMask *self)
{
    int rc = -1;

    if (pthread_sigmask(SIG_SETMASK, &self->mSigSet, 0))
        goto Finally;

    rc = 0;

Finally:

    return rc;
}

/* -------------------------------------------------------------------------- */
static int    sDeadChildRdFd_ = -1;
static int    sDeadChildWrFd_ = -1;
static void (*sDeadChildAction_)(void *aDeadChildObserver);
static void  *sDeadChildObserver_;

static void
deadChild_(int aSigNum)
{
    if (sDeadChildAction_)
    {
        debug(1, "observed dead child");
        sDeadChildAction_(sDeadChildObserver_);
    }

    int deadChildRdFd = sDeadChildRdFd_;
    int deadChildWrFd = sDeadChildWrFd_;

    if (-1 != deadChildWrFd)
    {
        debug(1,
              "queued dead child to fd %d from fd %d",
              deadChildRdFd, deadChildWrFd);

        if (writeSignal_(deadChildWrFd, aSigNum))
        {
            if (EBADF != errno && EWOULDBLOCK != errno)
                terminate(
                    errno,
                    "Unable to indicate dead child to fd %d",
                    deadChildWrFd);
        }
    }
}

static int
resetProcessChildrenWatch_(void)
{
    sDeadChildWrFd_     = -1;
    sDeadChildRdFd_     = -1;
    sDeadChildAction_   = 0;
    sDeadChildObserver_ = 0;

    struct sigaction childAction =
    {
        .sa_handler = SIG_DFL,
    };

    return sigaction(SIGCHLD, &childAction, 0);
}

int
watchProcessChildren(const struct Pipe *aTermPipe,
                     void               aChildAction(void *aChildObserver),
                     void              *aChildObserver)
{
    int rc = -1;

    /* It is ok to mark the termination pipe non-blocking because this
     * file descriptor is not shared with any other process. */

    if (aTermPipe)
    {
        if (nonblockingFd(sDeadChildRdFd_))
        {
            errno = EINVAL;
            goto Finally;
        }

        if (nonblockingFd(sDeadChildWrFd_))
        {
            errno = EINVAL;
            goto Finally;
        }
    }

    struct sigaction childAction =
    {
        .sa_handler = deadChild_,
        .sa_mask    = filledSigSet(),
    };

    if (sigaction(SIGCHLD, &childAction, 0))
        goto Finally;

    if (aTermPipe)
    {
        sDeadChildRdFd_ = aTermPipe->mRdFile->mFd;
        sDeadChildWrFd_ = aTermPipe->mWrFile->mFd;
    }

    sDeadChildAction_   = aChildAction;
    sDeadChildObserver_ = aChildObserver;

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
static struct Duration  sClockTickPeriod;
static int              sClockTickRdFd_ = -1;
static int              sClockTickWrFd_ = -1;
static struct sigaction sClockTickSigAction =
{
    .sa_handler = SIG_ERR,
};

static void
clockTick_(int aSigNum)
{
    int clockTickRdFd = sClockTickRdFd_;
    int clockTickWrFd = sClockTickWrFd_;

    if (-1 == clockTickWrFd)
        debug(1, "received clock tick");
    else
    {
        debug(1,
              "queued clock tick to fd %d from fd %d",
              clockTickRdFd, clockTickWrFd);

        if (writeSignal_(clockTickWrFd, aSigNum))
        {
            if (EBADF != errno && EWOULDBLOCK != errno)
                terminate(
                    errno,
                    "Unable to indicate clock tick to fd %d",
                    clockTickWrFd);
        }
    }
}

static int
resetProcessClockWatch_(void)
{
    int rc = -1;

    if (SIG_ERR != sClockTickSigAction.sa_handler ||
        (sClockTickSigAction.sa_flags & SA_SIGINFO))
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

        if (sigaction(SIGALRM, &sClockTickSigAction, 0))
            goto Finally;

        sClockTickSigAction.sa_handler = SIG_ERR;
        sClockTickSigAction.sa_flags = 0;

        sClockTickPeriod = Duration(NanoSeconds(0));
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
watchProcessClock(const struct Pipe *aClockPipe,
                  struct Duration    aClockPeriod)
{
    int rc = -1;

    struct sigaction prevAction_;
    struct sigaction *prevAction = 0;

    struct sigaction clockAction =
    {
        .sa_handler = clockTick_,
        .sa_mask    = filledSigSet(),
    };

    if (sigaction(SIGALRM, &clockAction, &prevAction_))
        goto Finally;
    prevAction = &prevAction_;

    /* It is ok to mark the clock pipe non-blocking because this
     * file descriptor is not shared with any other process. */

    if (aClockPipe)
    {
        if (nonblockingFd(sClockTickRdFd_))
        {
            errno = EINVAL;
            goto Finally;
        }

        if (nonblockingFd(sClockTickWrFd_))
        {
            errno = EINVAL;
            goto Finally;
        }
    }

    /* Make sure that there are no timers already running. The
     * interface only supports one clock instance. */

    struct itimerval clockTimer;

    if (getitimer(ITIMER_REAL, &clockTimer))
        goto Finally;

    if (clockTimer.it_value.tv_sec || clockTimer.it_value.tv_usec)
    {
        errno = EPERM;
        goto Finally;
    }

    clockTimer.it_value    = timeValFromNanoSeconds(sClockTickPeriod.duration);
    clockTimer.it_interval = clockTimer.it_value;

    if (setitimer(ITIMER_REAL, &clockTimer, 0))
        goto Finally;

    if (aClockPipe)
    {
        sClockTickRdFd_ = aClockPipe->mRdFile->mFd;
        sClockTickWrFd_ = aClockPipe->mWrFile->mFd;
    }

    sClockTickSigAction = *prevAction;
    sClockTickPeriod    = aClockPeriod;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            if (prevAction)
                if (sigaction(SIGALRM, prevAction, 0))
                    terminate(
                        errno,
                        "Unable to revert SIGALRM handler");
        }
    });

    return rc;
}

int
unwatchProcessClock(void)
{
    return resetProcessClockWatch_();
}

/* -------------------------------------------------------------------------- */
static int    sSignalRdFd_ = -1;
static int    sSignalWrFd_ = -1;
static void (*sSignalAction_)(void *aSigObserver, int aSigNum);
static void  *sSignalObserver_;

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
    if (sSignalAction_)
    {
        debug(1, "observed signal %d", aSigNum);
        sSignalAction_(sSignalObserver_, aSigNum);
    }

    int signalRdFd = sSignalRdFd_;
    int signalWrFd = sSignalWrFd_;

    if (-1 != signalWrFd)
    {
        debug(1,
              "queued signal %d on fd %d to fd %d",
              aSigNum,
              signalWrFd,
              signalRdFd);

        if (writeSignal_(signalWrFd, aSigNum))
        {
            if (EWOULDBLOCK != errno)
                terminate(
                    errno,
                    "Unable to queue signal %d on fd %d to fd %d",
                    aSigNum, signalWrFd, signalRdFd);
        }
    }
}

int
watchProcessSignals(const struct Pipe *aSigPipe,
                    void               aSigAction(void *aSigObserver,
                                                  int   aSigNum),
                    void              *aSigObserver)
{
    int rc = -1;

    /* It is ok to mark the signal pipe non-blocking because this
     * file descriptor is not shared with any other process. */

    if (aSigPipe)
    {
        if (nonblockingFd(aSigPipe->mRdFile->mFd))
            goto Finally;

        if (nonblockingFd(aSigPipe->mWrFile->mFd))
            goto Finally;
    }

    for (unsigned ix = 0; NUMBEROF(sWatchedSignals_) > ix; ++ix)
    {
        struct SignalWatch *watchedSig = sWatchedSignals_ + ix;

        struct sigaction watchAction =
        {
            .sa_handler = caughtSignal_,
            .sa_mask    = filledSigSet(),
        };

        if (sigaction(watchedSig->mSigNum,
                      &watchAction,
                      &watchedSig->mSigAction))
            goto Finally;

        watchedSig->mWatched = true;
    }

    if (aSigPipe)
    {
        sSignalRdFd_ = aSigPipe->mRdFile->mFd;
        sSignalWrFd_ = aSigPipe->mWrFile->mFd;
    }

    sSignalObserver_ = aSigObserver;
    sSignalAction_   = aSigAction;

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

    sSignalWrFd_     = -1;
    sSignalRdFd_     = -1;
    sSignalObserver_ = 0;

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
fetchProcessStartTime(pid_t aPid)
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
enum ProcessState
fetchProcessState(pid_t aPid)
{
    enum ProcessState rc = ProcessStateError;

    int   statfd  = -1;
    char *statbuf = 0;

    struct ProcessDirName processDirName;

    initProcessDirName(&processDirName, aPid);

    static const char sProcessStatFileNameFmt[] = "%s/stat";

    char processStatFileName[strlen(processDirName.mDirName) +
                             sizeof(sProcessStatFileNameFmt)];

    sprintf(processStatFileName,
            sProcessStatFileNameFmt, processDirName.mDirName);

    statfd = open(processStatFileName, O_RDONLY);
    if (-1 == statfd)
        goto Finally;

    ssize_t statlen = readFdFully(statfd, &statbuf);
    if (-1 == statlen)
        goto Finally;

    char *statend = statbuf + statlen;

    for (char *bufptr = statend; bufptr != statbuf; --bufptr)
    {
        if (')' == bufptr[-1])
        {
            if (1 < statend - bufptr && ' ' == *bufptr)
            {
                switch (bufptr[1])
                {
                default:
                    errno = ENOSYS;
                    goto Finally;

                case 'R': rc = ProcessStateRunning;  break;
                case 'S': rc = ProcessStateSleeping; break;
                case 'D': rc = ProcessStateWaiting;  break;
                case 'Z': rc = ProcessStateZombie;   break;
                case 'T': rc = ProcessStateStopped;  break;
                case 't': rc = ProcessStateTraced;   break;
                case 'X': rc = ProcessStateDead;     break;
                }
            }
            break;
        }
    }

Finally:

    FINALLY
    ({
        if (-1 != statfd)
            close(statfd);

        free(statbuf);
    });

    return rc;
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

        if (lockFile(self->mFile, LOCK_EX))
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

        sArg0 = aArg0;

        /* Ensure that the recorded time base is non-zero to allow it
         * to be distinguished from the case that it was not recorded
         * at all, and also ensure that the measured elapsed process
         * time is always non-zero. */

        sTimeBase = monotonicTime();
        do
            --sTimeBase.monotonic.ns;
        while ( ! sTimeBase.monotonic.ns);

        sProgramName = strrchr(sArg0, '/');
        sProgramName = sProgramName ? sProgramName + 1 : sArg0;

        srandom(getpid());

        const int sigList[] = { SIGALRM, 0 };

        if (pushProcessSigMask(&sSigMask, ProcessSigMaskBlock, sigList))
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
acquireProcessAppLock(void)
{
    int  rc     = -1;
    bool locked = false;

    /* Blocking all signals means that signals are delivered in this
     * thread synchronously with respect to this function, so the
     * mutex can be used to obtain serialisation both inside and
     * outside signal handlers. */

    sigset_t filledset;
    if (sigfillset(&filledset))
        goto Finally;

    if (pthread_sigmask(SIG_BLOCK, &filledset, &sProcessSigMask))
        goto Finally;

    if (errno = pthread_mutex_lock(&sProcessMutex))
        goto Finally;
    locked = true;

    struct ProcessLock *processLock = sProcessLock[sActiveProcessLock];

    if (processLock && lockProcessLock_(processLock))
        goto Finally;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            if (locked)
                pthread_mutex_unlock(&sProcessMutex);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
releaseProcessAppLock(void)
{
    int rc = -1;

    struct ProcessLock *processLock = sProcessLock[sActiveProcessLock];

    if (processLock && unlockProcessLock_(processLock))
        goto Finally;

    if (errno = pthread_mutex_unlock(&sProcessMutex))
        goto Finally;

    if (pthread_sigmask(SIG_SETMASK, &sProcessSigMask, 0))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
struct ProcessAppLock *
createProcessAppLock(void)
{
    struct ProcessAppLock *self = &sProcessAppLock;

    if (acquireProcessAppLock())
        terminate(errno, "Unable to acquire application lock");

    return self;
}

/* -------------------------------------------------------------------------- */
void
destroyProcessAppLock(struct ProcessAppLock *self)
{
    if (self)
    {
        ensure(&sProcessAppLock == self);

        if (releaseProcessAppLock())
            terminate(errno, "Unable to release application lock");
    }
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
enum ProcessStatus
monitorProcess(pid_t aPid)
{
    enum ProcessStatus rc = ProcessStatusError;

    siginfo_t siginfo;

    siginfo.si_pid = 0;
    if (waitid(P_PID, aPid, &siginfo,
               WEXITED | WSTOPPED | WCONTINUED | WNOHANG | WNOWAIT))
        goto Finally;

    if (siginfo.si_pid != aPid)
        rc = ProcessStatusRunning;
    else
    {
        switch (siginfo.si_code)
        {
        default:
            errno = EINVAL;
            goto Finally;

        case CLD_EXITED:    rc = ProcessStatusExited;  break;
        case CLD_KILLED:    rc = ProcessStatusKilled;  break;
        case CLD_DUMPED:    rc = ProcessStatusDumped;  break;
        case CLD_STOPPED:   rc = ProcessStatusStopped; break;
        case CLD_TRAPPED:   rc = ProcessStatusTrapped; break;
        case CLD_CONTINUED: rc = ProcessStatusRunning; break;
        }
    }

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

    pid_t rc     = -1;
    bool  locked = false;

    struct PushedProcessSigMask *pushedSigMask = 0;

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

    if (errno = pthread_mutex_lock(&sProcessMutex))
        goto Finally;
    locked = true;

    if (createProcessLock_(&sProcessLock_[inactiveProcessLock]))
        goto Finally;
    sProcessLock[inactiveProcessLock] = &sProcessLock_[inactiveProcessLock];

    /* If required, temporarily block all signals so that the child will not
     * receive signals which it cannot handle. */

    struct PushedProcessSigMask pushedSigMask_;

    if (pushProcessSigMask(&pushedSigMask_, ProcessSigMaskBlock, 0))
        goto Finally;
    pushedSigMask = &pushedSigMask_;

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

        pushedSigMask = 0;
        if (popProcessSigMask(&pushedSigMask_))
            terminate(
                errno,
                "Unable to pop process signal mask");

        if (popProcessSigMask(&sSigMask))
            terminate(
                errno,
                "Unable to restore process signal mask");

        break;
    }

    rc = childPid;

Finally:

    FINALLY
    ({
        if (pushedSigMask)
        {
            if (popProcessSigMask(pushedSigMask))
                terminate(
                    errno,
                    "Unable to pop process signal mask");
        }

        if (closeProcessLock_(sProcessLock[inactiveProcessLock]))
            terminate(
                errno,
                "Unable to close process lock");
        sProcessLock[inactiveProcessLock] = 0;

        if (locked)
            pthread_mutex_unlock(&sProcessMutex);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
fetchProcessStartTime_(pid_t aPid, struct BootClockTime *aBootClockTime)
{
    int rc  = -1;
    int err = 0;
    int fd  = -1;

    char *buf = 0;

    static const char pathNameFmt[] = "/proc/%jd/stat";

    char pathName[sizeof(pathNameFmt) + sizeof(pid_t) * CHAR_BIT];

    if (-1 == sprintf(pathName, pathNameFmt, (intmax_t) aPid))
        goto Finally;

    fd = open(pathName, O_RDONLY);
    if (-1 == fd)
    {
        err = errno;
        goto Finally;
    }

    ssize_t buflen = readFdFully(fd, &buf);
    if (-1 == buflen)
        goto Finally;
    if ( ! buflen)
    {
        errno = ERANGE;
        goto Finally;
    }

    char *word = strrchr(buf, ')');
    if ( ! word)
    {
        errno = ERANGE;
        goto Finally;
    }

    for (unsigned ix = 2; 22 > ix; ++ix)
    {
        while (*word && ! isspace((unsigned char) *word))
            ++word;

        if ( ! *word)
        {
            errno = ERANGE;
            goto Finally;
        }

        while (*word && isspace((unsigned char) *word))
            ++word;
    }

    char *end = word;
    while (*end && ! isspace((unsigned char) *end))
        ++end;

    uint64_t starttime_ns;

    do
    {
        char starttime[end - word + 1];
        memcpy(starttime, word, sizeof(starttime)-1);
        starttime[sizeof(starttime)-1] = 0;

        uint64_t starttime_ticks;
        if (parseUInt64(starttime, &starttime_ticks))
            goto Finally;

        long clktck = sysconf(_SC_CLK_TCK);
        if (-1 == clktck)
            goto Finally;

        starttime_ns =
            starttime_ticks * (TimeScale_ns / clktck) +
            starttime_ticks / clktck * (TimeScale_ns % clktck) +
            starttime_ticks % clktck * (TimeScale_ns % clktck) / clktck;

    } while (0);

    aBootClockTime->bootclock = NanoSeconds(starttime_ns);

    rc = 0;

Finally:

    FINALLY
    ({
        if (-1 == fd)
            close(fd);

        free(buf);
    });

    if (rc)
    {
        /* Ensure that ENOENT can be used to unambiguously indicated
         * that the process specified could not be found. */

        if (err)
            errno = err;
        else if (ENOENT == errno)
            errno = EDOM;
    }

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
ownProcessName(void)
{
    extern const char *__progname;

    return sProgramName ? sProgramName : __progname;
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
    return
        Duration(
            NanoSeconds(
                sTimeBase.monotonic.ns
                ? monotonicTime().monotonic.ns - sTimeBase.monotonic.ns
                : 0));
}

/* -------------------------------------------------------------------------- */
struct MonotonicTime
ownProcessBaseTime(void)
{
    return sTimeBase;
}

/* -------------------------------------------------------------------------- */
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

    int whiteList[numFds];

    {
        struct ProcessFdWhiteList fdWhiteList =
        {
            .mList = whiteList,
            .mLen  = 0,
        };

        for (unsigned jx = 0; NUMBEROF(stdfds) > jx; ++jx)
            fdWhiteList.mList[fdWhiteList.mLen++] = stdfds[jx];

        walkFileList(&fdWhiteList, enumerateProcessFds_);

        ensure(fdWhiteList.mLen == numFds);
    }

    if (closeFdDescriptors(whiteList, NUMBEROF(whiteList)))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
