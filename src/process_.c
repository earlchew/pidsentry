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
#include "thread_.h"
#include "system_.h"

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
    char        *mFileName;
    struct File  mFile_;
    struct File *mFile;
    int          mLock;
};

struct ProcessAppLock
{
    void *mNull;;
};

struct ProcessSignalThread
{
    pthread_mutex_t mMutex;
    pthread_cond_t  mCond;
    pthread_t       mThread;
    bool            mStopping;
};

static struct ProcessAppLock  sProcessAppLock;
static struct ThreadSigMutex  sProcessSigMutex = THREAD_SIG_MUTEX_INITIALIZER;
static struct ProcessLock     sProcessLock_[2];
static struct ProcessLock    *sProcessLock[2];
static unsigned               sActiveProcessLock;

static struct ProcessSignalThread sProcessSignalThread =
{
    .mMutex = PTHREAD_MUTEX_INITIALIZER,
    .mCond  = PTHREAD_COND_INITIALIZER,
};

static unsigned              sInit;
static struct ThreadSigMask  sSigMask;
static const char           *sArg0;
static const char           *sProgramName;
static struct MonotonicTime  sTimeBase;

/* -------------------------------------------------------------------------- */
static struct sigaction sSignalVectors[NSIG];

static void
dispatchSigAction_(int aSigNum, siginfo_t *aSigInfo, void *aSigContext)
{
    FINALLY
    ({
        sSignalVectors[aSigNum].sa_sigaction(aSigNum, aSigInfo, aSigContext);
    });
}

static void
dispatchSigHandler_(int aSigNum)
{
    FINALLY
    ({
        sSignalVectors[aSigNum].sa_handler(aSigNum);
    });
}

static int
changeSigAction_(unsigned                aSigNum,
                 const struct sigaction *aNewAction,
                 struct sigaction       *aOldAction)
{
    int rc = -1;

    ensure(NUMBEROF(sSignalVectors) > aSigNum);

    if (SIG_DFL != aNewAction->sa_handler || SIG_IGN != aNewAction->sa_handler)
    {
        struct sigaction sigAction = *aNewAction;

        if (aNewAction->sa_flags & SA_SIGINFO)
            sigAction.sa_sigaction = dispatchSigAction_;
        else
            sigAction.sa_handler = dispatchSigHandler_;
    }

    if (sigaction(aSigNum, aNewAction, aOldAction))
        goto Finally;

    sSignalVectors[aSigNum] = *aNewAction;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

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

    if (changeSigAction_(SIGPIPE, &nextAction, &prevAction))
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
        if (changeSigAction_(SIGPIPE, &sSigPipeAction, 0))
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
static struct
{
    struct ThreadSigMutex mSigMutex;
    unsigned              mCount;
    struct VoidMethod     mMethod;
} sSigCont =
{
    .mSigMutex = THREAD_SIG_MUTEX_INITIALIZER,
};

static void
sigCont_(int aSigNum)
{
    /* See the commentary in ownProcessSigContCount() to understand
     * the motivation for using a lock free update here. Other solutions
     * are possible, but a lock free approach is the most straightforward. */

    __sync_add_and_fetch(&sSigCont.mCount, 1);

    lockThreadSigMutex(&sSigCont.mSigMutex);

    if (ownVoidMethodNil(sSigCont.mMethod))
        debug(1, "detected SIGCONT");
    else
    {
        debug(1, "observed SIGCONT");
        callVoidMethod(sSigCont.mMethod);
    }

    unlockThreadSigMutex(&sSigCont.mSigMutex);
}

static int
hookProcessSigCont_(void)
{
    int rc = -1;

    struct sigaction sigAction =
    {
        .sa_handler = sigCont_,
        .sa_mask    = filledSigSet(),
    };

    if (changeSigAction_(SIGCONT, &sigAction, 0))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static int
unhookProcessSigCont_(void)
{
    int rc = -1;

    struct sigaction sigAction =
    {
        .sa_handler = SIG_DFL,
    };

    if (changeSigAction_(SIGCONT, &sigAction, 0))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static int
updateProcessSigContMethod_(struct VoidMethod aMethod)
{
    lockThreadSigMutex(&sSigCont.mSigMutex);
    sSigCont.mMethod = aMethod;
    unlockThreadSigMutex(&sSigCont.mSigMutex);

    return 0;
}

static int
resetProcessSigCont_(void)
{
    int rc = -1;

    if (updateProcessSigContMethod_(VoidMethod(0, 0)))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

unsigned
ownProcessSigContCount(void)
{
    /* Because this function is called from lockMutex(), amongst other places,
     * do not use or cause lockMutex() to be used here to avoid introducing
     * the chance of infinite recursion. */

    return sSigCont.mCount;
}

int
watchProcessSigCont(struct VoidMethod aMethod)
{
    int rc = -1;

    if (updateProcessSigContMethod_(aMethod))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
unwatchProcessSigCont(void)
{
    return resetProcessSigCont_();
}

/* -------------------------------------------------------------------------- */
static struct
{
    struct ThreadSigMutex mSigMutex;
    struct VoidMethod     mMethod;
} sSigStop =
{
    .mSigMutex = THREAD_SIG_MUTEX_INITIALIZER,
};

static void
sigStop_(int aSigNum)
{
    lockThreadSigMutex(&sSigStop.mSigMutex);

    if (ownVoidMethodNil(sSigStop.mMethod))
    {
        debug(1, "detected SIGTSTP");

        if (raise(SIGSTOP))
            terminate(errno, "Unable to stop process");
    }
    else
    {
        debug(1, "observed SIGTSTP");
        callVoidMethod(sSigStop.mMethod);
    }

    unlockThreadSigMutex(&sSigStop.mSigMutex);
}

static int
hookProcessSigStop_(void)
{
    int rc = -1;

    struct sigaction sigAction =
    {
        .sa_handler = sigStop_,
        .sa_mask    = filledSigSet(),
    };

    if (changeSigAction_(SIGTSTP, &sigAction, 0))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static int
unhookProcessSigStop_(void)
{
    int rc = -1;

    struct sigaction sigAction =
    {
        .sa_handler = SIG_DFL,
    };

    if (changeSigAction_(SIGTSTP, &sigAction, 0))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static int
updateProcessSigStopMethod_(struct VoidMethod aMethod)
{
    lockThreadSigMutex(&sSigStop.mSigMutex);
    sSigStop.mMethod = aMethod;
    unlockThreadSigMutex(&sSigStop.mSigMutex);

    return 0;
}

static int
resetProcessSigStop_(void)
{
    int rc = -1;

    if (updateProcessSigStopMethod_(VoidMethod(0, 0)))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
watchProcessSigStop(struct VoidMethod aMethod)
{
    int rc = -1;

    if (updateProcessSigStopMethod_(aMethod))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
unwatchProcessSigStop(void)
{
    return resetProcessSigStop_();
}

/* -------------------------------------------------------------------------- */
static struct VoidMethod sSigChldMethod;

static void
sigChld_(int aSigNum)
{
    if ( ! ownVoidMethodNil(sSigChldMethod))
    {
        debug(1, "observed SIGCHLD");
        callVoidMethod(sSigChldMethod);
    }
}

static int
resetProcessChildrenWatch_(void)
{
    int rc = -1;

    struct sigaction sigAction =
    {
        .sa_handler = SIG_DFL,
    };

    if (changeSigAction_(SIGCHLD, &sigAction, 0))
        goto Finally;

    sSigChldMethod = VoidMethod(0, 0);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
watchProcessChildren(struct VoidMethod aMethod)
{
    int rc = -1;

    sSigChldMethod = aMethod;

    struct sigaction sigAction =
    {
        .sa_handler = sigChld_,
        .sa_mask    = filledSigSet(),
    };

    if (changeSigAction_(SIGCHLD, &sigAction, 0))
        goto Finally;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            sSigChldMethod = VoidMethod(0, 0);
    });

    return rc;
}

int
unwatchProcessChildren(void)
{
    return resetProcessChildrenWatch_();
}

/* -------------------------------------------------------------------------- */
static struct Duration   sClockTickPeriod;
static struct VoidMethod sClockMethod;
static struct sigaction  sClockTickSigAction =
{
    .sa_handler = SIG_ERR,
};

static void
clockTick_(int aSigNum)
{
    if (ownVoidMethodNil(sClockMethod))
        debug(1, "received clock tick");
    else
    {
        debug(1, "observed clock tick");
        callVoidMethod(sClockMethod);
    }
}

static int
resetProcessClockWatch_(void)
{
    int rc = -1;

    if (SIG_ERR != sClockTickSigAction.sa_handler ||
        (sClockTickSigAction.sa_flags & SA_SIGINFO))
    {
        struct itimerval disableClock =
        {
            .it_value    = { .tv_sec = 0 },
            .it_interval = { .tv_sec = 0 },
        };

        if (setitimer(ITIMER_REAL, &disableClock, 0))
            goto Finally;

        if (changeSigAction_(SIGALRM, &sClockTickSigAction, 0))
            goto Finally;

        sClockMethod = VoidMethod(0, 0);

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
watchProcessClock(struct VoidMethod aMethod,
                  struct Duration   aClockPeriod)
{
    int rc = -1;

    sClockMethod = aMethod;

    struct sigaction prevAction_;
    struct sigaction *prevAction = 0;

    struct sigaction clockAction =
    {
        .sa_handler = clockTick_,
        .sa_mask    = filledSigSet(),
    };

    if (changeSigAction_(SIGALRM, &clockAction, &prevAction_))
        goto Finally;
    prevAction = &prevAction_;

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

    sClockTickSigAction = *prevAction;
    sClockTickPeriod    = aClockPeriod;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            if (prevAction)
                if (changeSigAction_(SIGALRM, prevAction, 0))
                    terminate(errno, "Unable to revert SIGALRM handler");

            sClockMethod = VoidMethod(0, 0);
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
static struct VoidIntMethod sSignalMethod;

static struct SignalWatch {
    int              mSigNum;
    struct sigaction mSigAction;
    bool             mWatched;
} sWatchedSignals[] =
{
    { SIGHUP },
    { SIGINT },
    { SIGQUIT },
    { SIGTERM },
};

static void
caughtSignal_(int aSigNum)
{
    if ( ! ownVoidIntMethodNil(sSignalMethod))
    {
        struct ProcessSignalName sigName;

        debug(1, "observed %s", formatProcessSignalName(&sigName, aSigNum));

        callVoidIntMethod(sSignalMethod, aSigNum);
    }
}

int
watchProcessSignals(struct VoidIntMethod aMethod)
{
    int rc = -1;

    sSignalMethod = aMethod;

    /* It is ok to mark the signal pipe non-blocking because this
     * file descriptor is not shared with any other process. */

    for (unsigned ix = 0; NUMBEROF(sWatchedSignals) > ix; ++ix)
    {
        struct SignalWatch *watchedSig = sWatchedSignals + ix;

        struct sigaction watchAction =
        {
            .sa_handler = caughtSignal_,
            .sa_mask    = filledSigSet(),
        };

        if (changeSigAction_(watchedSig->mSigNum,
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
            for (unsigned ix = 0; NUMBEROF(sWatchedSignals) > ix; ++ix)
            {
                struct SignalWatch *watchedSig = sWatchedSignals + ix;

                if (watchedSig->mWatched)
                {
                    struct ProcessSignalName sigName;

                    if (changeSigAction_(
                            watchedSig->mSigNum, &watchedSig->mSigAction, 0))
                        terminate(
                            errno,
                            "Unable to revert action for %s",
                            formatProcessSignalName(
                                &sigName, watchedSig->mSigNum));

                    watchedSig->mWatched = false;
                }
            }

            sSignalMethod = VoidIntMethod(0, 0);
        }
    });

    return rc;
}

static int
resetProcessSignalsWatch_(void)
{
    int rc  = 0;
    int err = 0;

    for (unsigned ix = 0; NUMBEROF(sWatchedSignals) > ix; ++ix)
    {
        struct SignalWatch *watchedSig = sWatchedSignals + ix;

        if (watchedSig->mWatched)
        {
            if (changeSigAction_(
                    watchedSig->mSigNum, &watchedSig->mSigAction, 0))
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

    sSignalMethod = VoidIntMethod(0, 0);

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

    if (resetProcessSigStop_())
        goto Finally;

    if (resetProcessSigCont_())
        goto Finally;

    if (resetProcessClockWatch_())
        goto Finally;

    if (resetProcessSignalsWatch_())
        goto Finally;

    if (resetProcessChildrenWatch_())
        goto Finally;

    /* Do not call resetProcessSigPipe_() here since this function is
     * called from forkProcess() and that would mean that the child
     * process would not receive EPIPE on writes to broken pipes. Instead
     * defer the call to execProcess() so that new programs will have
     * SIGPIPE delivered.
     *
     *    if (resetProcessSigPipe_())
     *       goto Finally;
     */

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
const char *
formatProcessSignalName(struct ProcessSignalName *self, int aSigNum)
{
    const char *signalName = strsignal(aSigNum);

    if (signalName)
        self->mSignalName = signalName;
    else
    {
        sprintf(self->mSignalText_, PROCESS_SIGNALNAME_FMT_, aSigNum);
        self->mSignalName = self->mSignalText_;
    }

    return self->mSignalName;
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

    ssize_t statlen = readFdFully(statfd, &statbuf, 0);
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
        if (closeFd(&statfd))
            terminate(
                errno, "Unable to close file descriptor %d", statfd);

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
    self->mFileName = 0;

    static const char pathFmt[] = "/proc/%jd/.";

    char path[sizeof(pathFmt) + sizeof(pid_t) * CHAR_BIT];

    if (-1 == sprintf(path, pathFmt, (intmax_t) getpid()))
        goto Finally;

    self->mFileName = strdup(path);
    if ( ! self->mFileName)
        goto Finally;

    if (createFile(
            &self->mFile_,
            open(self->mFileName, O_RDONLY | O_CLOEXEC)))
        goto Finally;
    self->mFile = &self->mFile_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            if (closeFile(self->mFile))
                terminate(
                    errno,
                    "Unable to close file descriptor %d",
                    self->mFile->mFd);

            free(self->mFileName);
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
        free(self->mFileName);

        if (closeFile(self->mFile))
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
static void *
signalThread_(void *self_)
{
    struct ProcessSignalThread *self = self_;

    /* This is a spare thread which will always be available as a context
     * in which signals can be delivered in the case that all other
     * threads are unable accept signals. */

    lockMutex(&self->mMutex);
    while ( ! self->mStopping)
        waitCond(&self->mCond, &self->mMutex);
    unlockMutex(&self->mMutex);

    return 0;
}

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

        if (pushThreadSigMask(&sSigMask, ThreadSigMaskBlock, sigList))
            goto Finally;

        if (createProcessLock_(&sProcessLock_[sActiveProcessLock]))
            goto Finally;
        sProcessLock[sActiveProcessLock] = &sProcessLock_[sActiveProcessLock];

        if (Error_init())
            goto Finally;

        createThread(
            &sProcessSignalThread.mThread, 0,
            signalThread_, &sProcessSignalThread);

        hookProcessSigCont_();
        hookProcessSigStop_();
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

        unhookProcessSigStop_();
        unhookProcessSigCont_();

        lockMutex(&sProcessSignalThread.mMutex);
        sProcessSignalThread.mStopping = true;
        unlockMutexSignal(&sProcessSignalThread.mMutex,
                          &sProcessSignalThread.mCond);

        joinThread(&sProcessSignalThread.mThread);

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
    int rc = -1;

    struct ThreadSigMutex *lock = 0;

    lock = lockThreadSigMutex(&sProcessSigMutex);

    if (1 == ownThreadSigMutexLocked(&sProcessSigMutex))
    {
        struct ProcessLock *processLock = sProcessLock[sActiveProcessLock];

        if (processLock && lockProcessLock_(processLock))
            goto Finally;
    }

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            lock = unlockThreadSigMutex(lock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
releaseProcessAppLock(void)
{
    int rc = -1;

    struct ThreadSigMutex *lock = &sProcessSigMutex;

    if (1 == ownThreadSigMutexLocked(lock))
    {
        struct ProcessLock *processLock = sProcessLock[sActiveProcessLock];

        if (processLock && unlockProcessLock_(processLock))
            goto Finally;
    }

    rc = 0;

Finally:

    FINALLY
    ({
        lock = unlockThreadSigMutex(lock);
    });

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

    return processLock ? processLock->mFileName : 0;
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
forkProcess(enum ForkProcessOption aOption, pid_t aPgid)
{
    pid_t rc = -1;

    const char            *err  = 0;
    struct ThreadSigMutex *lock = 0;

    unsigned activeProcessLock   = 0 + sActiveProcessLock;
    unsigned inactiveProcessLock = 1 - activeProcessLock;

    ensure(NUMBEROF(sProcessLock_) > activeProcessLock);
    ensure(NUMBEROF(sProcessLock_) > inactiveProcessLock);

    ensure( ! sProcessLock[inactiveProcessLock]);

#ifdef __linux__
    long clocktick = sysconf(_SC_CLK_TCK);
    if (-1 == clocktick)
        goto Finally;
#endif

    /* Temporarily block all signals so that the child will not receive
     * signals which it cannot handle reliably, and so that signal
     * handlers will not run in this process when the process mutex
     * is held. */

    lock = lockThreadSigMutex(&sProcessSigMutex);

    /* The child process needs separate process lock. It cannot share
     * the process lock with the parent because flock(2) distinguishes
     * locks by file descriptor table entry. Create the process lock
     * in the parent first so that the child process is guaranteed to
     * be able to synchronise its messages. */

    if (sProcessLock[sActiveProcessLock])
    {
        ensure(
            sProcessLock[activeProcessLock] ==
            &sProcessLock_[activeProcessLock]);

        if (createProcessLock_(&sProcessLock_[inactiveProcessLock]))
            goto Finally;
        sProcessLock[inactiveProcessLock] = &sProcessLock_[inactiveProcessLock];
    }

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
            if (setpgid(childPid, aPgid ? aPgid : childPid))
                goto Finally;
        }

        /* On Linux, fetchProcessSignature() uses the process start
         * time from /proc/pid/stat, but that start time is measured
         * in _SC_CLK_TCK periods which limits the rate at which
         * processes can be forked without causing ambiguity. Although
         * this ambiguity is largely theoretical, it is a simple matter
         * to overcome. */

#ifdef __linux__
        monotonicSleep(
            Duration(NanoSeconds(TimeScale_ns / clocktick * 5 / 4)));
#endif
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
            if (setpgid(0, aPgid))
            {
                err = "Unable to set process group";
                goto Finally;
            }
        }

        /* Reset all the signals so that the child will not attempt
         * to catch signals. After that, reset the signal mask so
         * that the child will receive signals. */

        if (resetSignals_())
        {
            err = "Unable to reset signal handlers";
            goto Finally;
        }

        /* This is the only thread running in the new process so
         * it is safe to release the process mutex here. Once the
         * process mutex and the signal mask are restored, reinstate
         * the original process signal mask. */

        lock = unlockThreadSigMutex(lock);

        break;
    }

    rc = childPid;

Finally:

    FINALLY
    ({
        int errcode = errno;

        if (closeProcessLock_(sProcessLock[inactiveProcessLock]))
            terminate(
                errno,
                "Unable to close process lock");
        sProcessLock[inactiveProcessLock] = 0;

        lock = unlockThreadSigMutex(lock);

        if (err)
            terminate(errcode, "%s", err);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
void
execProcess(const char *aCmd, char **aArgv)
{
    if (popThreadSigMask(&sSigMask))
        goto Finally;

    /* Call resetProcessSigPipe_() here to ensure that SIGPIPE will be
     * delivered to the new program. Note that it is possible that there
     * was no previous call to forkProcess(), though this is normally
     * the case. */

    if (resetProcessSigPipe_())
        goto Finally;

    execvp(aCmd, aArgv);

Finally:

    FINALLY({});
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
        struct ProcessSignalName sigName;

        debug(
            0,
            "child terminated by %s",
            formatProcessSignalName(&sigName, WTERMSIG(aStatus)));

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
int
fetchProcessSignature(pid_t aPid, char **aSignature)
{
    int rc  = -1;
    int err = 0;
    int fd  = -1;

    char *buf       = 0;
    char *signature = 0;

    const char *incarnation = fetchSystemIncarnation();
    if ( ! incarnation)
        goto Finally;

    /* Note that it is expected that forkProcess() will guarantee that
     * the pid of the child process combined with its signature results
     * in a universally unique key. Because the pid is recycled over time
     * (as well as being reused after each reboot), the signature must
     * unambiguously qualify the pid. */

    do
    {
        struct ProcessDirName processDirName;

        initProcessDirName(&processDirName, aPid);

        static const char sProcessStatFileNameFmt[] = "%s/stat";

        char processStatFileName[strlen(processDirName.mDirName) +
                                 sizeof(sProcessStatFileNameFmt)];

        sprintf(processStatFileName,
                sProcessStatFileNameFmt, processDirName.mDirName);

        fd = open(processStatFileName, O_RDONLY);
        if (-1 == fd)
        {
            err = errno;
            goto Finally;
        }

    } while (0);

    ssize_t buflen = readFdFully(fd, &buf, 0);
    if (-1 == buflen)
        goto Finally;
    if ( ! buflen)
    {
        errno = ERANGE;
        goto Finally;
    }

    char *bufend = buf + buflen;
    char *word   = memrchr(buf, ')', buflen);
    if ( ! word)
    {
        errno = ERANGE;
        goto Finally;
    }

    for (unsigned ix = 2; 22 > ix; ++ix)
    {
        while (word != bufend && ! isspace((unsigned char) *word))
            ++word;

        if (word == bufend)
        {
            errno = ERANGE;
            goto Finally;
        }

        while (word != bufend && isspace((unsigned char) *word))
            ++word;
    }

    char *end = word;
    while (end != bufend && ! isspace((unsigned char) *end))
        ++end;

    do
    {
        char timestamp[end-word+1];
        memcpy(timestamp, word, end-word);
        timestamp[sizeof(timestamp)-1] = 0;

        static const char signatureFmt[] = "%s:%s";

        size_t signatureLen =
            strlen(incarnation) + sizeof(timestamp) + sizeof(signatureFmt);

        signature = malloc(signatureLen);
        if ( ! signature)
            goto Finally;

        if (0 > sprintf(signature, signatureFmt, incarnation, timestamp))
            goto Finally;

    } while (0);

    *aSignature = signature;
    signature   = 0;

    rc = 0;

Finally:

    FINALLY
    ({
        if (closeFd(&fd))
            terminate(errno, "Unable to close file descriptor %d", fd);

        free(buf);
        free(signature);
    });

    return rc;
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
