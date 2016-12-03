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
#include "pipe_.h"
#include "stdfdfiller_.h"
#include "bellsocketpair_.h"
#include "timekeeping_.h"
#include "thread_.h"
#include "fdset_.h"
#include "random_.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/file.h>
#include <sys/wait.h>

#include <valgrind/valgrind.h>

struct ProcessLock
{
    struct File            mFile_;
    struct File           *mFile;
    const struct LockType *mLock;
};

struct ProcessAppLock
{
    void *mNull;
};

static struct ProcessAppLock processAppLock_;

static unsigned processAbort_;
static unsigned processQuit_;

static struct
{
    struct ThreadSigMutex  mMutex_;
    struct ThreadSigMutex *mMutex;
    struct ProcessLock     mLock_;
    struct ProcessLock    *mLock;
} processLock_ =
{
    .mMutex_ = THREAD_SIG_MUTEX_INITIALIZER(processLock_.mMutex_),
    .mMutex  = &processLock_.mMutex_,
};

struct ForkProcessChannel_;

static struct ProcessForkChildLock_
{
    pthread_mutex_t  mMutex_;
    pthread_mutex_t *mMutex;
    struct Pid       mProcess;
    struct Tid       mThread;
    unsigned         mCount;

    struct ForkProcessChannel_ *mChannelList;

} processForkChildLock_ =
{
    .mMutex_ = PTHREAD_MUTEX_INITIALIZER,
};

static struct
{
    pthread_mutex_t        mMutex_;
    pthread_mutex_t       *mMutex;
    struct Pid             mParentPid;
    struct RWMutexWriter   mSigVecLock_;
    struct RWMutexWriter  *mSigVecLock;
    struct ThreadSigMutex *mSigLock;
    struct ThreadSigMutex *mLock;
} processFork_ =
{
    .mMutex_ = PTHREAD_MUTEX_INITIALIZER,
};

static unsigned              moduleInit_;
static bool                  moduleInitOnce_;
static bool                  moduleInitAtFork_;
static sigset_t              processSigMask_;
static const char           *processArg0_;
static const char           *programName_;
static struct MonotonicTime  processTimeBase_;

static const char *signalNames_[NSIG] =
{
    [SIGHUP]    = "SIGHUP",
    [SIGABRT]   = "SIGABRT",
    [SIGALRM]   = "SIGALRM",
    [SIGBUS]    = "SIGBUS",
    [SIGCHLD]   = "SIGCHLD",
    [SIGCONT]   = "SIGCONT",
    [SIGFPE]    = "SIGFPE",
    [SIGHUP]    = "SIGHUP",
    [SIGILL]    = "SIGILL",
    [SIGINT]    = "SIGINT",
    [SIGKILL]   = "SIGKILL",
    [SIGPIPE]   = "SIGPIPE",
    [SIGQUIT]   = "SIGQUIT",
    [SIGSEGV]   = "SIGSEGV",
    [SIGSTOP]   = "SIGSTOP",
    [SIGTERM]   = "SIGTERM",
    [SIGTSTP]   = "SIGTSTP",
    [SIGTTIN]   = "SIGTTIN",
    [SIGTTOU]   = "SIGTTOU",
    [SIGUSR1]   = "SIGUSR1",
    [SIGUSR2]   = "SIGUSR2",
    [SIGPOLL]   = "SIGPOLL",
    [SIGPROF]   = "SIGPROF",
    [SIGSYS]    = "SIGSYS",
    [SIGTRAP]   = "SIGTRAP",
    [SIGURG]    = "SIGURG",
    [SIGVTALRM] = "SIGVTALRM",
    [SIGXCPU]   = "SIGXCPU",
    [SIGXFSZ]   = "SIGXFSZ",
};

/* -------------------------------------------------------------------------- */
static unsigned __thread processSignalContext_;

struct ProcessSignalVector
{
    struct sigaction mAction;
    pthread_mutex_t  mActionMutex_;
    pthread_mutex_t *mActionMutex;
};

static struct
{
    struct ProcessSignalVector mVector[NSIG];
    pthread_rwlock_t           mVectorLock;
    struct ThreadSigMutex      mSignalMutex;

} processSignals_ =
{
    .mVectorLock  = PTHREAD_RWLOCK_INITIALIZER,
    .mSignalMutex = THREAD_SIG_MUTEX_INITIALIZER(processSignals_.mSignalMutex),
};

static void
dispatchSigExit_(int aSigNum)
{
    /* Check for handlers for termination signals that might compete
     * with programmatically requested behaviour. */

    if (SIGABRT == aSigNum && processAbort_)
        abortProcess();

    if (SIGQUIT == aSigNum && processQuit_)
        quitProcess();
}

static void
runSigAction_(int aSigNum, siginfo_t *aSigInfo, void *aSigContext)
{
    struct ProcessSignalVector *sv = &processSignals_.mVector[aSigNum];

    struct RWMutexReader  sigVecLock_;
    struct RWMutexReader *sigVecLock;

    sigVecLock = createRWMutexReader(
        &sigVecLock_, &processSignals_.mVectorLock);

    enum ErrorFrameStackKind stackKind =
        switchErrorFrameStack(ErrorFrameStackSignal);

    struct ProcessSignalName sigName;
    debug(1,
          "dispatch signal %s",
          formatProcessSignalName(&sigName, aSigNum));

    pthread_mutex_t *actionLock = lockMutex(sv->mActionMutex);
    {
        dispatchSigExit_(aSigNum);

        if (SIG_DFL != sv->mAction.sa_handler &&
            SIG_IGN != sv->mAction.sa_handler)
        {
            ++processSignalContext_;

            struct ErrorFrameSequence frameSequence =
                pushErrorFrameSequence();

            sv->mAction.sa_sigaction(aSigNum, aSigInfo, aSigContext);

            popErrorFrameSequence(frameSequence);

            --processSignalContext_;
        }
    }
    actionLock = unlockMutex(actionLock);

    switchErrorFrameStack(stackKind);
    sigVecLock = destroyRWMutexReader(sigVecLock);
}

static void
dispatchSigAction_(int aSigNum, siginfo_t *aSigInfo, void *aSigContext)
{
    FINALLY
    ({
        runSigAction_(aSigNum, aSigInfo, aSigContext);
    });
}

static void
runSigHandler_(int aSigNum)
{
    struct ProcessSignalVector *sv = &processSignals_.mVector[aSigNum];

    struct RWMutexReader  sigVecLock_;
    struct RWMutexReader *sigVecLock;

    sigVecLock = createRWMutexReader(
        &sigVecLock_, &processSignals_.mVectorLock);

    enum ErrorFrameStackKind stackKind =
        switchErrorFrameStack(ErrorFrameStackSignal);

    struct ProcessSignalName sigName;
    debug(1,
          "dispatch signal %s",
          formatProcessSignalName(&sigName, aSigNum));

    pthread_mutex_t *actionLock = lockMutex(sv->mActionMutex);
    {
        dispatchSigExit_(aSigNum);

        if (SIG_DFL != sv->mAction.sa_handler &&
            SIG_IGN != sv->mAction.sa_handler)
        {
            ++processSignalContext_;

            struct ErrorFrameSequence frameSequence =
                pushErrorFrameSequence();

            sv->mAction.sa_handler(aSigNum);

            popErrorFrameSequence(frameSequence);

            --processSignalContext_;
        }
    }
    actionLock = unlockMutex(actionLock);

    switchErrorFrameStack(stackKind);
    sigVecLock = destroyRWMutexReader(sigVecLock);
}

static void
dispatchSigHandler_(int aSigNum)
{
    FINALLY
    ({
        runSigHandler_(aSigNum);
    });
}

static CHECKED int
changeSigAction_(unsigned          aSigNum,
                 struct sigaction  aNewAction,
                 struct sigaction *aOldAction)
{
    int rc = -1;

    ensure(NUMBEROF(processSignals_.mVector) > aSigNum);

    struct RWMutexReader  sigVecLock_;
    struct RWMutexReader *sigVecLock = 0;

    struct ThreadSigMask  threadSigMask_;
    struct ThreadSigMask *threadSigMask = 0;

    pthread_mutex_t *actionLock = 0;

    struct sigaction nextAction = aNewAction;

    if (SIG_DFL != nextAction.sa_handler && SIG_IGN != nextAction.sa_handler)
    {
        if (nextAction.sa_flags & SA_SIGINFO)
            nextAction.sa_sigaction = dispatchSigAction_;
        else
            nextAction.sa_handler = dispatchSigHandler_;

        /* Require that signal delivery not restart system calls.
         * This is important so that event loops have a chance
         * to re-compute their deadlines. See restart_syscalls()
         * for related information pertaining to SIGCONT.
         *
         * See the wrappers in eintr_.c that wrap system calls
         * to ensure that restart semantics are available. */

        ERROR_IF(
            (nextAction.sa_flags & SA_RESTART));

        /* Require that signal delivery is not recursive to avoid
         * having to deal with too many levels of re-entrancy, but
         * allow programmatic failures to be delivered in order to
         * terminate the program. */

        sigset_t filledSigSet;
        ERROR_IF(
            sigfillset(&filledSigSet));

        ERROR_IF(
            sigdelset(&filledSigSet, SIGABRT));

        nextAction.sa_mask   = filledSigSet;
        nextAction.sa_flags &= ~ SA_NODEFER;
    }

    {
        struct ThreadSigMutex *sigLock =
            lockThreadSigMutex(&processSignals_.mSignalMutex);

        if ( ! processSignals_.mVector[aSigNum].mActionMutex)
            processSignals_.mVector[aSigNum].mActionMutex =
                createMutex(&processSignals_.mVector[aSigNum].mActionMutex_);

        sigLock = unlockThreadSigMutex(sigLock);
    }

    /* Block signal delivery into this thread to avoid the signal
     * dispatch attempting to acquire the dispatch mutex recursively
     * in the same thread context. */

    sigVecLock = createRWMutexReader(
        &sigVecLock_, &processSignals_.mVectorLock);

    threadSigMask = pushThreadSigMask(
        &threadSigMask_, ThreadSigMaskBlock, (const int []) { aSigNum, 0 });

    actionLock = lockMutex(processSignals_.mVector[aSigNum].mActionMutex);

    struct sigaction prevAction;
    ERROR_IF(
        sigaction(aSigNum, &nextAction, &prevAction));

    /* Do not overwrite the output result unless the underlying
     * sigaction() call succeeds. */

    if (aOldAction)
        *aOldAction = prevAction;

    processSignals_.mVector[aSigNum].mAction = aNewAction;

    rc = 0;

Finally:

    FINALLY
    ({
        actionLock = unlockMutex(actionLock);

        threadSigMask = popThreadSigMask(threadSigMask);

        sigVecLock = destroyRWMutexReader(sigVecLock);
    });

    return rc;
}

unsigned
ownProcessSignalContext(void)
{
    return processSignalContext_;
}

/* -------------------------------------------------------------------------- */
static struct sigaction processSigPipeAction_ =
{
    .sa_handler = SIG_ERR,
};

int
ignoreProcessSigPipe(void)
{
    int rc = -1;

    ERROR_IF(
        changeSigAction_(
            SIGPIPE,
            (struct sigaction) { .sa_handler = SIG_IGN },
            &processSigPipeAction_));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
resetProcessSigPipe_(void)
{
    int rc = -1;

    if (SIG_ERR != processSigPipeAction_.sa_handler ||
        (processSigPipeAction_.sa_flags & SA_SIGINFO))
    {
        ERROR_IF(
            changeSigAction_(SIGPIPE, processSigPipeAction_, 0));

        processSigPipeAction_.sa_handler = SIG_ERR;
        processSigPipeAction_.sa_flags   = 0;
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
    struct ThreadSigMutex     mSigMutex;
    unsigned                  mCount;
    struct WatchProcessMethod mMethod;
} processSigCont_ =
{
    .mSigMutex = THREAD_SIG_MUTEX_INITIALIZER(processSigCont_.mSigMutex),
};

static void
sigCont_(int aSigNum)
{
    /* See the commentary in ownProcessSigContCount() to understand
     * the motivation for using a lock free update here. Other solutions
     * are possible, but a lock free approach is the most straightforward. */

    __sync_add_and_fetch(&processSigCont_.mCount, 2);

    struct ThreadSigMutex *lock = lockThreadSigMutex(
        &processSigCont_.mSigMutex);

    if (ownWatchProcessMethodNil(processSigCont_.mMethod))
        debug(1, "detected SIGCONT");
    else
    {
        debug(1, "observed SIGCONT");
        ABORT_IF(
            callWatchProcessMethod(processSigCont_.mMethod));
    }

    lock = unlockThreadSigMutex(lock);
}

static CHECKED int
hookProcessSigCont_(void)
{
    int rc = -1;

    ERROR_IF(
        changeSigAction_(
            SIGCONT,
            (struct sigaction) { .sa_handler = sigCont_ }, 0));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
unhookProcessSigCont_(void)
{
    int rc = -1;

    ERROR_IF(
        changeSigAction_(
            SIGCONT,
            (struct sigaction) { .sa_handler = SIG_DFL },
            0));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
updateProcessSigContMethod_(struct WatchProcessMethod aMethod)
{
    struct ThreadSigMutex *lock =
        lockThreadSigMutex(&processSigCont_.mSigMutex);
    processSigCont_.mMethod = aMethod;
    lock = unlockThreadSigMutex(lock);

    return 0;
}

static CHECKED int
resetProcessSigCont_(void)
{
    int rc = -1;

    ERROR_IF(
        updateProcessSigContMethod_(WatchProcessMethodNil()));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
watchProcessSigCont(struct WatchProcessMethod aMethod)
{
    int rc = -1;

    ERROR_IF(
        updateProcessSigContMethod_(aMethod));

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
static CHECKED unsigned
fetchProcessSigContTracker_(void)
{
    /* Because this function is called from lockMutex(), amongst other places,
     * do not use or cause lockMutex() to be used here to avoid introducing
     * the chance of infinite recursion.
     *
     * Use the least sigificant bit to ensure that only non-zero counts
     * are valid, allowing detection of zero initialised values that
     * have not been constructed properly. */

    return 1 | processSigCont_.mCount;
}

struct ProcessSigContTracker
ProcessSigContTracker_(void)
{
    return (struct ProcessSigContTracker)
    {
        .mCount = fetchProcessSigContTracker_(),
    };
}

/* -------------------------------------------------------------------------- */
bool
checkProcessSigContTracker(struct ProcessSigContTracker *self)
{
    unsigned sigContCount = self->mCount;

    ensure(1 && sigContCount);

    self->mCount = fetchProcessSigContTracker_();

    return sigContCount != self->mCount;
}

/* -------------------------------------------------------------------------- */
static struct
{
    struct ThreadSigMutex     mSigMutex;
    struct WatchProcessMethod mMethod;
} processSigStop_ =
{
    .mSigMutex = THREAD_SIG_MUTEX_INITIALIZER(processSigStop_.mSigMutex),
};

static void
sigStop_(int aSigNum)
{
    struct ThreadSigMutex *lock =
        lockThreadSigMutex(&processSigStop_.mSigMutex);

    if (ownWatchProcessMethodNil(processSigStop_.mMethod))
    {
        debug(1, "detected SIGTSTP");

        ABORT_IF(
            raise(SIGSTOP),
            {
                terminate(errno, "Unable to stop process");
            });
    }
    else
    {
        debug(1, "observed SIGTSTP");
        ABORT_IF(
            callWatchProcessMethod(processSigStop_.mMethod));
    }

    lock = unlockThreadSigMutex(lock);
}

static CHECKED int
hookProcessSigStop_(void)
{
    int rc = -1;

    ERROR_IF(
        changeSigAction_(
            SIGTSTP,
            (struct sigaction) { .sa_handler = sigStop_ },
            0));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
unhookProcessSigStop_(void)
{
    int rc = -1;

    ERROR_IF(
        changeSigAction_(
            SIGTSTP,
            (struct sigaction) { .sa_handler = SIG_DFL },
            0));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
updateProcessSigStopMethod_(struct WatchProcessMethod aMethod)
{
    struct ThreadSigMutex *lock =
        lockThreadSigMutex(&processSigStop_.mSigMutex);
    processSigStop_.mMethod = aMethod;
    lock = unlockThreadSigMutex(lock);

    return 0;
}

static CHECKED int
resetProcessSigStop_(void)
{
    int rc = -1;

    ERROR_IF(
        updateProcessSigStopMethod_(WatchProcessMethodNil()));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
watchProcessSigStop(struct WatchProcessMethod aMethod)
{
    int rc = -1;

    ERROR_IF(
        updateProcessSigStopMethod_(aMethod));

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
static struct WatchProcessMethod processSigChldMethod_;

static void
sigChld_(int aSigNum)
{
    if ( ! ownWatchProcessMethodNil(processSigChldMethod_))
    {
        debug(1, "observed SIGCHLD");
        ABORT_IF(
            callWatchProcessMethod(processSigChldMethod_));
    }
}

static CHECKED int
resetProcessChildrenWatch_(void)
{
    int rc = -1;

    ERROR_IF(
        changeSigAction_(
            SIGCHLD,
            (struct sigaction) { .sa_handler = SIG_DFL },
            0));

    processSigChldMethod_ = WatchProcessMethodNil();

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
watchProcessChildren(struct WatchProcessMethod aMethod)
{
    int rc = -1;

    struct WatchProcessMethod sigChldMethod = processSigChldMethod_;

    processSigChldMethod_ = aMethod;

    ERROR_IF(
        changeSigAction_(
            SIGCHLD,
            (struct sigaction) { .sa_handler = sigChld_ },
            0));

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            processSigChldMethod_ = sigChldMethod;
    });

    return rc;
}

int
unwatchProcessChildren(void)
{
    return resetProcessChildrenWatch_();
}

/* -------------------------------------------------------------------------- */
static struct Duration           processClockTickPeriod_;
static struct WatchProcessMethod processClockMethod_;
static struct sigaction          processClockTickSigAction_ =
{
    .sa_handler = SIG_ERR,
};

static void
clockTick_(int aSigNum)
{
    if (ownWatchProcessMethodNil(processClockMethod_))
        debug(1, "received clock tick");
    else
    {
        debug(1, "observed clock tick");
        ABORT_IF(
            callWatchProcessMethod(processClockMethod_));
    }
}

static CHECKED int
resetProcessClockWatch_(void)
{
    int rc = -1;

    if (SIG_ERR != processClockTickSigAction_.sa_handler ||
        (processClockTickSigAction_.sa_flags & SA_SIGINFO))
    {
        struct itimerval disableClock =
        {
            .it_value    = { .tv_sec = 0 },
            .it_interval = { .tv_sec = 0 },
        };

        ERROR_IF(
            setitimer(ITIMER_REAL, &disableClock, 0));

        ERROR_IF(
            changeSigAction_(SIGALRM, processClockTickSigAction_, 0));

        processClockMethod_ = WatchProcessMethodNil();

        processClockTickSigAction_.sa_handler = SIG_ERR;
        processClockTickSigAction_.sa_flags = 0;

        processClockTickPeriod_ = ZeroDuration;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
watchProcessClock(struct WatchProcessMethod aMethod,
                  struct Duration           aClockPeriod)
{
    int rc = -1;

    struct WatchProcessMethod clockMethod = processClockMethod_;

    processClockMethod_ = aMethod;

    struct sigaction  prevAction_;
    struct sigaction *prevAction = 0;

    ERROR_IF(
        changeSigAction_(
            SIGALRM,
            (struct sigaction) { .sa_handler = clockTick_ },
            &prevAction_));
    prevAction = &prevAction_;

    /* Make sure that there are no timers already running. The
     * interface only supports one clock instance. */

    struct itimerval clockTimer;

    ERROR_IF(
        getitimer(ITIMER_REAL, &clockTimer));

    ERROR_IF(
        clockTimer.it_value.tv_sec || clockTimer.it_value.tv_usec,
        {
            errno = EPERM;
        });

    clockTimer.it_interval = clockTimer.it_value;
    clockTimer.it_value    = timeValFromNanoSeconds(
        processClockTickPeriod_.duration);

    ERROR_IF(
        setitimer(ITIMER_REAL, &clockTimer, 0));

    processClockTickSigAction_ = *prevAction;
    processClockTickPeriod_    = aClockPeriod;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            if (prevAction)
                ABORT_IF(
                    changeSigAction_(SIGALRM, *prevAction, 0),
                    {
                        terminate(errno, "Unable to revert SIGALRM handler");
                    });

            processClockMethod_ = clockMethod;
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
static struct WatchProcessSignalMethod processWatchedSignalMethod_;

static struct SignalWatch {
    int              mSigNum;
    struct sigaction mSigAction;
    bool             mWatched;
} processWatchedSignals_[] =
{
    { SIGHUP },
    { SIGINT },
    { SIGQUIT },
    { SIGTERM },
};

static void
caughtSignal_(int aSigNum, siginfo_t *aSigInfo, void *aUContext_)
{
    if ( ! ownWatchProcessSignalMethodNil(processWatchedSignalMethod_))
    {
        struct ProcessSignalName sigName;

        struct Pid pid = Pid(aSigInfo->si_pid);
        struct Uid uid = Uid(aSigInfo->si_uid);

        debug(1, "observed %s pid %" PRId_Pid " uid %" PRId_Uid,
              formatProcessSignalName(&sigName, aSigNum),
              FMTd_Pid(pid),
              FMTd_Uid(uid));

        callWatchProcessSignalMethod(
            processWatchedSignalMethod_, aSigNum, pid, uid);
    }
}

int
watchProcessSignals(struct WatchProcessSignalMethod aMethod)
{
    int rc = -1;

    processWatchedSignalMethod_ = aMethod;

    for (unsigned ix = 0; NUMBEROF(processWatchedSignals_) > ix; ++ix)
    {
        struct SignalWatch *watchedSig = processWatchedSignals_ + ix;

        ERROR_IF(
            changeSigAction_(
                watchedSig->mSigNum,
                (struct sigaction) { .sa_sigaction = caughtSignal_,
                                     .sa_flags     = SA_SIGINFO },
                &watchedSig->mSigAction));

        watchedSig->mWatched = true;
    }

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            for (unsigned ix = 0; NUMBEROF(processWatchedSignals_) > ix; ++ix)
            {
                struct SignalWatch *watchedSig = processWatchedSignals_ + ix;

                if (watchedSig->mWatched)
                {
                    struct ProcessSignalName sigName;

                    ABORT_IF(
                        changeSigAction_(
                            watchedSig->mSigNum, watchedSig->mSigAction, 0),
                        {
                            terminate(
                                errno,
                                "Unable to revert action for %s",
                                formatProcessSignalName(
                                    &sigName, watchedSig->mSigNum));
                        });

                    watchedSig->mWatched = false;
                }
            }

            processWatchedSignalMethod_ = WatchProcessSignalMethodNil();
        }
    });

    return rc;
}

static CHECKED int
resetProcessSignalsWatch_(void)
{
    int rc  = 0;
    int err = 0;

    for (unsigned ix = 0; NUMBEROF(processWatchedSignals_) > ix; ++ix)
    {
        struct SignalWatch *watchedSig = processWatchedSignals_ + ix;

        if (watchedSig->mWatched)
        {
            if (changeSigAction_(
                    watchedSig->mSigNum, watchedSig->mSigAction, 0))
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

    processWatchedSignalMethod_ = WatchProcessSignalMethodNil();

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
static CHECKED int
resetSignals_(void)
{
    int rc = -1;

    ERROR_IF(
        resetProcessSigStop_());
    ERROR_IF(
        resetProcessSigCont_());
    ERROR_IF(
        resetProcessClockWatch_());
    ERROR_IF(
        resetProcessSignalsWatch_());
    ERROR_IF(
        resetProcessChildrenWatch_());

    /* Do not call resetProcessSigPipe_() here since this function is
     * called from forkProcess() and that would mean that the child
     * process would not receive EPIPE on writes to broken pipes. Instead
     * defer the call to execProcess() so that new programs will have
     * SIGPIPE delivered.
     *
     *    if (resetProcessSigPipe_())
     *       goto Finally_;
     */

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
initProcessDirName(struct ProcessDirName *self, struct Pid aPid)
{
    int rc = -1;

    ERROR_IF(
        0 > sprintf(self->mDirName, PROCESS_DIRNAME_FMT_, FMTd_Pid(aPid)));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
formatProcessSignalName(struct ProcessSignalName *self, int aSigNum)
{
    self->mSignalName = 0;

    if (0 <= aSigNum && NUMBEROF(signalNames_) > aSigNum)
        self->mSignalName = signalNames_[aSigNum];

    if ( ! self->mSignalName)
    {
        static const char signalErr[] = "signal ?";

        typedef char SignalErrCheckT[
            2 * (sizeof(signalErr) < sizeof(PROCESS_SIGNALNAME_FMT_)) - 1];

        if (0 > sprintf(self->mSignalText_, PROCESS_SIGNALNAME_FMT_, aSigNum))
            strcpy(self->mSignalText_, signalErr);
        self->mSignalName = self->mSignalText_;
    }

    return self->mSignalName;
}

/* -------------------------------------------------------------------------- */
struct ProcessState
fetchProcessState(struct Pid aPid)
{
    struct ProcessState rc = { .mState = ProcessStateError };

    int   statFd  = -1;
    char *statBuf = 0;

    struct ProcessDirName processDirName;

    static const char processStatFileNameFmt_[] = "%s/stat";

    ERROR_IF(
        initProcessDirName(&processDirName, aPid));

    {
        char processStatFileName[strlen(processDirName.mDirName) +
                                 sizeof(processStatFileNameFmt_)];

        ERROR_IF(
            0 > sprintf(processStatFileName,
                        processStatFileNameFmt_, processDirName.mDirName));

        ERROR_IF(
            (statFd = openFd(processStatFileName, O_RDONLY, 0),
             -1 == statFd));
    }

    ssize_t statlen;
    ERROR_IF(
        (statlen = readFdFully(statFd, &statBuf, 0),
         -1 == statlen));

    char *statend = statBuf + statlen;

    for (char *bufptr = statend; bufptr != statBuf; --bufptr)
    {
        if (')' == bufptr[-1])
        {
            if (1 < statend - bufptr && ' ' == *bufptr)
            {
                switch (bufptr[1])
                {
                default:
                    ERROR_IF(
                        true,
                        {
                            errno = ENOSYS;
                        });

                case 'R': rc.mState = ProcessStateRunning;  break;
                case 'S': rc.mState = ProcessStateSleeping; break;
                case 'D': rc.mState = ProcessStateWaiting;  break;
                case 'Z': rc.mState = ProcessStateZombie;   break;
                case 'T': rc.mState = ProcessStateStopped;  break;
                case 't': rc.mState = ProcessStateTraced;   break;
                case 'X': rc.mState = ProcessStateDead;     break;
                }
            }
            break;
        }
    }

Finally:

    FINALLY
    ({
        statFd = closeFd(statFd);

        free(statBuf);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static CHECKED int
createProcessLock_(struct ProcessLock *self)
{
    int rc = -1;

    self->mFile = 0;
    self->mLock = 0;

    ERROR_IF(
        temporaryFile(&self->mFile_));
    self->mFile = &self->mFile_;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
closeProcessLock_(struct ProcessLock *self)
{
    if (self)
        self->mFile = closeFile(self->mFile);
}

/* -------------------------------------------------------------------------- */
static CHECKED int
lockProcessLock_(struct ProcessLock *self)
{
    int rc = -1;

    ensure( ! self->mLock);

    ERROR_IF(
        lockFileRegion(self->mFile, LockTypeWrite, 0, 0));

    self->mLock = &LockTypeWrite;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
unlockProcessLock_(struct ProcessLock *self)
{
    ensure(self->mLock);

    ABORT_IF(
        unlockFileRegion(self->mFile, 0, 0));

    self->mLock = 0;
}

/* -------------------------------------------------------------------------- */
static void
forkProcessLock_(struct ProcessLock *self)
{
    if (self->mLock)
    {
        ABORT_IF(
            unlockFileRegion(self->mFile, 0, 0));
        ABORT_IF(
            lockFileRegion(self->mFile, LockTypeWrite, 0, 0));
    }
}

/* -------------------------------------------------------------------------- */
int
acquireProcessAppLock(void)
{
    int rc = -1;

    struct ThreadSigMutex *lock =
        lockThreadSigMutex(processLock_.mMutex);

    if (1 == ownThreadSigMutexLocked(processLock_.mMutex))
    {
        if (processLock_.mLock)
            ERROR_IF(
                lockProcessLock_(processLock_.mLock));
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

    struct ThreadSigMutex *lock = processLock_.mMutex;

    if (1 == ownThreadSigMutexLocked(lock))
    {
        if (processLock_.mLock)
            unlockProcessLock_(processLock_.mLock);
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
    struct ProcessAppLock *self = &processAppLock_;

    ABORT_IF(
        acquireProcessAppLock(),
        {
            terminate(errno, "Unable to acquire application lock");
        });

    return self;
}

/* -------------------------------------------------------------------------- */
struct ProcessAppLock *
destroyProcessAppLock(struct ProcessAppLock *self)
{
    if (self)
    {
        ensure(&processAppLock_ == self);

        ABORT_IF(
            releaseProcessAppLock(),
            {
                terminate(errno, "Unable to release application lock");
            });
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
unsigned
ownProcessAppLockCount(void)
{
    return ownThreadSigMutexLocked(processLock_.mMutex);
}

/* -------------------------------------------------------------------------- */
const struct File *
ownProcessAppLockFile(const struct ProcessAppLock *self)
{
    ensure(&processAppLock_ == self);

    return processLock_.mLock ? processLock_.mLock->mFile : 0;
}

/* -------------------------------------------------------------------------- */
static CHECKED struct ProcessForkChildLock_ *
acquireProcessForkChildLock_(struct ProcessForkChildLock_ *self)
{
    struct Tid tid = ownThreadId();
    struct Pid pid = ownProcessId();

    ensure( ! self->mProcess.mPid || self->mProcess.mPid == pid.mPid);

    if (self->mThread.mTid == tid.mTid)
        ++self->mCount;
    else
    {
        pthread_mutex_t *lock = lockMutex(&self->mMutex_);

        self->mMutex   = lock;
        self->mThread  = tid;
        self->mProcess = pid;

        ensure( ! self->mCount);

        ++self->mCount;
    }

    return self;
}

/* -------------------------------------------------------------------------- */
static CHECKED struct ProcessForkChildLock_ *
relinquishProcessForkChildLock_(struct ProcessForkChildLock_ *self,
                                unsigned                      aRelease)
{
    if (self)
    {
        struct Tid tid = ownThreadId();
        struct Pid pid = ownProcessId();

        ensure(self->mCount);
        ensure(self->mMutex == &self->mMutex_);

        /* Note that the owning tid will not match in the main thread
         * of the child process (which will have a different tid) after
         * the lock is acquired and the parent process is forked. */

        ensure(self->mProcess.mPid);

        if (self->mProcess.mPid == pid.mPid)
            ensure(self->mThread.mTid == tid.mTid);
        else
        {
            self->mProcess = pid;
            self->mThread  = tid;
        }

        ensure(aRelease <= self->mCount);

        self->mCount -= aRelease;

        if ( ! self->mCount)
        {
            pthread_mutex_t *lock = self->mMutex;

            self->mThread = Tid(0);
            self->mMutex  = 0;

            lock = unlockMutex(lock);
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
static CHECKED struct ProcessForkChildLock_ *
releaseProcessForkChildLock_(struct ProcessForkChildLock_ *self)
{
    return relinquishProcessForkChildLock_(self, 1);
}

/* -------------------------------------------------------------------------- */
static CHECKED struct ProcessForkChildLock_ *
resetProcessForkChildLock_(struct ProcessForkChildLock_ *self)
{
    return relinquishProcessForkChildLock_(self, self->mCount);
}

/* -------------------------------------------------------------------------- */
int
reapProcessChild(struct Pid aPid, int *aStatus)
{
    int rc = -1;

    ERROR_IF(
        -1 == aPid.mPid || ! aPid.mPid,
        {
            errno = EINVAL;
        });

    pid_t pid;

    do
    {
        ERROR_IF(
            (pid = waitpid(aPid.mPid, aStatus, __WALL),
             -1 == pid && EINTR != errno));
    } while (pid != aPid.mPid);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
waitProcessChild_(const siginfo_t *aSigInfo)
{
    int rc = ChildProcessStateError;

    switch (aSigInfo->si_code)
    {
    default:
        break;

    case CLD_EXITED: rc = ChildProcessStateExited;  break;
    case CLD_KILLED: rc = ChildProcessStateKilled;  break;
    case CLD_DUMPED: rc = ChildProcessStateDumped;  break;
    }

Finally:

    FINALLY({});

    return rc;
}

struct ChildProcessState
waitProcessChild(struct Pid aPid)
{
    struct ChildProcessState rc = { .mChildState = ChildProcessStateError };

    ERROR_IF(
        -1 == aPid.mPid || ! aPid.mPid,
        {
            errno = EINVAL;
        });

    siginfo_t siginfo;

    do
    {
        siginfo.si_pid = 0;
        ERROR_IF(
            waitid(P_PID, aPid.mPid, &siginfo, WEXITED | WNOWAIT) &&
            EINTR != errno);

    } while (siginfo.si_pid != aPid.mPid);

    rc.mChildStatus = siginfo.si_status;

    ERROR_IF(
        (rc.mChildState = waitProcessChild_(&siginfo),
         ChildProcessStateError == rc.mChildState),
        {
            errno = EINVAL;
        });

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
struct ChildProcessState
monitorProcessChild(struct Pid aPid)
{
    struct ChildProcessState rc = { .mChildState = ChildProcessStateError };

    siginfo_t siginfo;

    while (1)
    {
        siginfo.si_pid = 0;

        int waitErr;
        ERROR_IF(
            (waitErr = waitid(P_PID, aPid.mPid, &siginfo,
                   WEXITED | WSTOPPED | WCONTINUED | WNOHANG | WNOWAIT),
            waitErr && EINTR != errno));
        if (waitErr)
            continue;

        break;
    }

    if (siginfo.si_pid != aPid.mPid)
        rc.mChildState = ChildProcessStateRunning;
    else
    {
        rc.mChildStatus = siginfo.si_status;

        switch (siginfo.si_code)
        {
        default:
            ERROR_IF(
                true,
                {
                    errno = EINVAL;
                });

        case CLD_EXITED:    rc.mChildState = ChildProcessStateExited;  break;
        case CLD_KILLED:    rc.mChildState = ChildProcessStateKilled;  break;
        case CLD_DUMPED:    rc.mChildState = ChildProcessStateDumped;  break;
        case CLD_STOPPED:   rc.mChildState = ChildProcessStateStopped; break;
        case CLD_TRAPPED:   rc.mChildState = ChildProcessStateTrapped; break;
        case CLD_CONTINUED: rc.mChildState = ChildProcessStateRunning; break;
        }
    }

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
struct Pid
waitProcessChildren()
{
    int rc = -1;

    struct Pid pid;

    while (1)
    {
        siginfo_t siginfo;

        siginfo.si_pid = 0;

        pid_t waitErr;
        ERROR_IF(
            (waitErr = waitid(P_ALL, 0, &siginfo, WEXITED | WNOWAIT),
             waitErr && EINTR != errno && ECHILD != errno));
        if (waitErr && EINTR == errno)
            continue;

        if (waitErr)
            pid = Pid(0);
        else
        {
            ensure(siginfo.si_pid);
            pid = Pid(siginfo.si_pid);
        }

        break;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc ? Pid(-1) : pid;
}

/* -------------------------------------------------------------------------- */
struct ForkProcessResult_
{
    int mReturnCode;
    int mErrCode;
};

struct ForkProcessChannel_
{
    struct ForkProcessChannel_  *mPrev;
    struct ForkProcessChannel_ **mList;

    struct Pipe  mResultPipe_;
    struct Pipe *mResultPipe;

    struct BellSocketPair  mResultSocket_;
    struct BellSocketPair *mResultSocket;
};

static CHECKED struct ForkProcessChannel_ *
closeForkProcessChannel_(
    struct ForkProcessChannel_ *self)
{
    if (self)
    {
        *self->mList = self->mPrev;
        self->mPrev  = 0;

        self->mResultSocket = closeBellSocketPair(self->mResultSocket);
        self->mResultPipe   = closePipe(self->mResultPipe);
    }

    return 0;
}

static void
closeForkProcessChannelResultChild_(
    struct ForkProcessChannel_ *self)
{
    closePipeWriter(self->mResultPipe);
    closeBellSocketPairChild(self->mResultSocket);
}

static void
closeForkProcessChannelResultParent_(
    struct ForkProcessChannel_ *self)
{
    closePipeReader(self->mResultPipe);
    closeBellSocketPairParent(self->mResultSocket);
}

static CHECKED int
createForkProcessChannel_(
    struct ForkProcessChannel_ *self, struct ForkProcessChannel_ **aList)
{
    int rc = -1;

    struct StdFdFiller  stdFdFiller_;
    struct StdFdFiller *stdFdFiller = 0;

    self->mResultPipe   = 0;
    self->mResultSocket = 0;
    self->mList         = aList;

    self->mPrev  = *aList;
    *self->mList = self;

    /* Prevent the fork communication channels from inadvertently becoming
     * stdin, stdout or stderr. */

    ERROR_IF(
        createStdFdFiller(&stdFdFiller_));
    stdFdFiller = &stdFdFiller_;

    ERROR_IF(
        createPipe(&self->mResultPipe_, O_CLOEXEC));
    self->mResultPipe = &self->mResultPipe_;

    ERROR_IF(
        createBellSocketPair(&self->mResultSocket_, O_CLOEXEC));
    self->mResultSocket = &self->mResultSocket_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            self = closeForkProcessChannel_(self);

        stdFdFiller = closeStdFdFiller(stdFdFiller);
    });

    return rc;
}

static CHECKED int
includeForkProcessChannelFdSet_(
    const struct ForkProcessChannel_ *self,
    struct FdSet                     *aFdSet)
{
    int rc = -1;

    struct File *filelist[] =
    {
        self->mResultPipe->mRdFile,
        self->mResultPipe->mWrFile,

        self->mResultSocket->mSocketPair->mChildSocket ?
        self->mResultSocket->mSocketPair->mChildSocket->mSocket->mFile : 0,

        self->mResultSocket->mSocketPair->mParentSocket ?
        self->mResultSocket->mSocketPair->mParentSocket->mSocket->mFile : 0,
    };

    for (unsigned ix = 0; NUMBEROF(filelist) > ix; ++ix)
    {
        if (filelist[ix])
            ERROR_IF(
                insertFdSetFile(aFdSet, filelist[ix]) &&
                EEXIST != errno);
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
excludeForkProcessChannelFdSet_(
    const struct ForkProcessChannel_ *self,
    struct FdSet                     *aFdSet)
{
    int rc = -1;

    struct File *filelist[] =
    {
        self->mResultPipe->mRdFile,
        self->mResultPipe->mWrFile,

        self->mResultSocket->mSocketPair->mChildSocket ?
        self->mResultSocket->mSocketPair->mChildSocket->mSocket->mFile : 0,

        self->mResultSocket->mSocketPair->mParentSocket ?
        self->mResultSocket->mSocketPair->mParentSocket->mSocket->mFile : 0,
    };

    for (unsigned ix = 0; NUMBEROF(filelist) > ix; ++ix)
    {
        if (filelist[ix])
            ERROR_IF(
                removeFdSetFile(aFdSet, filelist[ix]) &&
                ENOENT != errno);
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
sendForkProcessChannelResult_(
    struct ForkProcessChannel_      *self,
    const struct ForkProcessResult_ *aResult)
{
    int rc = -1;

    ERROR_IF(
        sizeof(*aResult) != writeFile(
            self->mResultPipe->mWrFile, (char *) aResult, sizeof(*aResult), 0));

    ERROR_IF(
        ringBellSocketPairChild(self->mResultSocket));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
recvForkProcessChannelResult_(
    struct ForkProcessChannel_ *self,
    struct ForkProcessResult_  *aResult)
{
    int rc = -1;

    ERROR_IF(
        waitBellSocketPairParent(self->mResultSocket, 0));

    ERROR_IF(
        sizeof(*aResult) != readFile(
            self->mResultPipe->mRdFile, (char *) aResult, sizeof(*aResult), 0));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
recvForkProcessChannelAcknowledgement_(
    struct ForkProcessChannel_ *self)
{
    int rc = -1;

    ERROR_IF(
        waitBellSocketPairChild(self->mResultSocket, 0));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static CHECKED int
sendForkProcessChannelAcknowledgement_(
    struct ForkProcessChannel_ *self)
{
    return ringBellSocketPairParent(self->mResultSocket);
}

/* -------------------------------------------------------------------------- */
static void
callForkMethod_(struct ForkProcessMethod aMethod) NORETURN;

static void
callForkMethod_(struct ForkProcessMethod aMethod)
{
    int status = EXIT_SUCCESS;

    if ( ! ownForkProcessMethodNil(aMethod))
    {
        ABORT_IF(
            (status = callForkProcessMethod(aMethod),
             -1 == status || (errno = 0, 0 > status || 255 < status)),
            {
                if (-1 != status)
                    terminate(
                        0,
                        "Out of range exit status %d", status);
            });
    }

    /* When running with valgrind, use execl() to prevent
     * valgrind performing a leak check on the temporary
     * process. */

    if (RUNNING_ON_VALGRIND)
    {
        static const char exitCmdFmt[] = "exit %d";

        char exitCmd[sizeof(exitCmdFmt) + sizeof(status) * CHAR_BIT];

        ABORT_IF(
            0 > sprintf(exitCmd, exitCmdFmt, status),
            {
                terminate(
                    errno,
                    "Unable to format exit status command: %d", status);
            });

        ABORT_IF(
            (execl("/bin/sh", "sh", "-c", exitCmd, (char *) 0), true),
            {
                terminate(
                    errno,
                    "Unable to exit process: %s", exitCmd);
            });
    }

    exitProcess(status);
}

static CHECKED int
forkProcessChild_PostParent_(
    struct ForkProcessChannel_        *self,
    enum ForkProcessOption             aOption,
    struct Pid                         aChildPid,
    struct Pgid                        aChildPgid,
    struct PostForkParentProcessMethod aPostForkParentMethod,
    struct FdSet                      *aBlacklistFds)
{
    int rc = -1;

    struct StdFdFiller  stdFdFiller_;
    struct StdFdFiller *stdFdFiller = 0;

    /* Forcibly set the process group of the child to avoid
     * the race that would occur if only the child attempts
     * to set its own process group */

    if (ForkProcessSetProcessGroup == aOption)
        ERROR_IF(
            setpgid(aChildPid.mPid,
                    aChildPgid.mPgid ? aChildPgid.mPgid : aChildPid.mPid));

    /* On Linux, struct PidSignature uses the process start
     * time from /proc/pid/stat, but that start time is measured
     * in _SC_CLK_TCK periods which limits the rate at which
     * processes can be forked without causing ambiguity. Although
     * this ambiguity is largely theoretical, it is a simple matter
     * to overcome by constraining the rate at which processes can
     * fork. */

#ifdef __linux__
    {
        long clocktick;
        ERROR_IF(
            (clocktick = sysconf(_SC_CLK_TCK),
             -1 == clocktick));

        monotonicSleep(
            Duration(NanoSeconds(TimeScale_ns / clocktick * 5 / 4)));
    }
#endif

    /* Sequence fork operations with the child so that the actions
     * are completely synchronised. The actions are performed in
     * the following order:
     *
     *      o Close all but whitelisted fds in child
     *      o Run child post fork method
     *      o Run parent post fork method
     *      o Close only blacklisted fds in parent
     *      o Continue both parent and child process
     *
     * The blacklisted fds in the parent will only be closed if there
     * are no errors detected in either the child or the parent
     * post fork method. This provides the parent with the potential
     * to retry the operation. */

    closeForkProcessChannelResultChild_(self);

    for (struct ForkProcessChannel_ *processChannel = self;
         processChannel;
         processChannel = processChannel->mPrev)
    {
        ERROR_IF(
            excludeForkProcessChannelFdSet_(processChannel, aBlacklistFds));
    }

    {
        struct ForkProcessResult_ forkResult;

        ERROR_IF(
            recvForkProcessChannelResult_(self, &forkResult));

        ERROR_IF(
            forkResult.mReturnCode,
            {
                errno = forkResult.mErrCode;
            });

        ERROR_IF(
            ! ownPostForkParentProcessMethodNil(aPostForkParentMethod) &&
            callPostForkParentProcessMethod(
                aPostForkParentMethod, aChildPid));

        /* Always remove the stdin, stdout and stderr from the blacklisted
         * file descriptors. If they were indeed blacklisted, then for stdin
         * and stdout, close and replace each so that the descriptors themselves
         * remain valid, but leave stderr intact. */

        {
            ERROR_IF(
                createStdFdFiller(&stdFdFiller_));
            stdFdFiller = &stdFdFiller_;

            const struct
            {
                int  mFd;
                int  mAltFd;

            } stdfdlist[] = {
                { .mFd = STDIN_FILENO,
                  .mAltFd = stdFdFiller->mFile[0]->mFd },

                { .mFd = STDOUT_FILENO,
                  .mAltFd = stdFdFiller->mFile[1]->mFd },

                { .mFd = STDERR_FILENO,
                  .mAltFd = -1 },
            };

            for (unsigned ix = 0; NUMBEROF(stdfdlist) > ix; ++ix)
            {
                int fd    = stdfdlist[ix].mFd;
                int altFd = stdfdlist[ix].mAltFd;

                int err;
                ERROR_IF(
                    (err = removeFdSet(aBlacklistFds, fd),
                     err && ENOENT != errno));

                if ( ! err && -1 != altFd)
                    ERROR_IF(
                        fd != duplicateFd(altFd, fd));
            }

            stdFdFiller = closeStdFdFiller(stdFdFiller);
        }

        ERROR_IF(
            closeFdOnlyBlackList(aBlacklistFds));

        ERROR_IF(
            sendForkProcessChannelAcknowledgement_(self));
    }

    rc = 0;

Finally:

    FINALLY
    ({
        stdFdFiller = closeStdFdFiller(stdFdFiller);
    });

    return rc;
}

static void
forkProcessChild_PostChild_(
    struct ForkProcessChannel_        *self,
    enum ForkProcessOption             aOption,
    struct Pgid                        aChildPgid,
    struct PostForkChildProcessMethod  aPostForkChildMethod,
    struct FdSet                      *aWhitelistFds)
{
    int rc = -1;

    /* Detach this instance from its recursive ancestors if any because
     * the child process will only interact with its direct parent. */

    self->mPrev = 0;

    /* Ensure that the behaviour of each child diverges from the
     * behaviour of the parent. This is primarily useful for
     * testing. */

    scrambleRandomSeed(ownProcessId().mPid);

    if (ForkProcessSetSessionLeader == aOption)
    {
        ERROR_IF(
            -1 == setsid());
    }
    else if (ForkProcessSetProcessGroup == aOption)
    {
        ERROR_IF(
            setpgid(0, aChildPgid.mPgid));
    }

    /* Reset all the signals so that the child will not attempt
     * to catch signals. The parent should have set the signal
     * mask appropriately. */

    ERROR_IF(
        resetSignals_());

    closeForkProcessChannelResultParent_(self);

    ERROR_IF(
        includeForkProcessChannelFdSet_(self, aWhitelistFds));

    ERROR_IF(
        closeFdExceptWhiteList(aWhitelistFds));

    ERROR_IF(
        ! ownPostForkChildProcessMethodNil(aPostForkChildMethod) &&
        callPostForkChildProcessMethod(aPostForkChildMethod));

    {
        /* Send the child fork process method result to the parent so
         * that it can return an error code to the caller, then wait
         * for the parent to acknowledge. */

        struct ForkProcessResult_ forkResult =
        {
            .mReturnCode = 0,
            .mErrCode    = 0,
        };

        if (sendForkProcessChannelResult_(self, &forkResult) ||
            recvForkProcessChannelAcknowledgement_(self))
        {
            /* Terminate the child if the result could not be sent. The
             * parent will detect the child has terminated because the
             * result socket will close.
             *
             * Also terminate the child if the acknowledgement could not
             * be received from the parent. This will typically be because
             * the parent has terminated without sending the
             * acknowledgement, so it is reasonable to simply terminate
             * the child. */

            exitProcess(EXIT_FAILURE);
        }
    }

    rc = 0;

Finally:

    if (rc)
    {
        /* This is the error path running in the child. After attempting
         * to send the error indication, simply terminate the child.
         * The parent should either receive the failure indication, or
         * detect that the child has terminated before sending the
         * error indication. */

        struct ForkProcessResult_ forkResult =
        {
            .mReturnCode = rc,
            .mErrCode    = errno,
        };

        while (
            sendForkProcessChannelResult_(self, &forkResult) ||
            recvForkProcessChannelAcknowledgement_(self))
        {
            break;
        }

        exitProcess(EXIT_FAILURE);
    }
}

struct Pid
forkProcessChild(enum ForkProcessOption             aOption,
                 struct Pgid                        aPgid,
                 struct PreForkProcessMethod        aPreForkMethod,
                 struct PostForkChildProcessMethod  aPostForkChildMethod,
                 struct PostForkParentProcessMethod aPostForkParentMethod,
                 struct ForkProcessMethod           aForkMethod)
{
    pid_t rc = -1;

    struct Pid childPid = Pid(-1);

    struct FdSet  blacklistFds_;
    struct FdSet *blacklistFds = 0;

    struct FdSet  whitelistFds_;
    struct FdSet *whitelistFds = 0;

    struct ForkProcessChannel_  forkChannel_;
    struct ForkProcessChannel_ *forkChannel = 0;

    /* Acquire the processForkLock_ so that other threads that issue
     * a raw fork() will synchronise with this code via the pthread_atfork()
     * handler. */

    struct ProcessForkChildLock_ *forkLock =
        acquireProcessForkChildLock_(&processForkChildLock_);

    ensure(ForkProcessSetProcessGroup == aOption || ! aPgid.mPgid);

    ERROR_IF(
        createFdSet(&blacklistFds_));
    blacklistFds = &blacklistFds_;

    ERROR_IF(
        createFdSet(&whitelistFds_));
    whitelistFds = &whitelistFds_;

    ERROR_IF(
        ! ownPreForkProcessMethodNil(aPreForkMethod) &&
        callPreForkProcessMethod(
            aPreForkMethod,
            & (struct PreForkProcess)
            {
                .mBlacklistFds = blacklistFds,
                .mWhitelistFds = whitelistFds,
            }));

    /* Do not open the fork channel until after the pre fork method
     * has been run so that these additional file descriptors are
     * not visible to that method. */

    ERROR_IF(
        createForkProcessChannel_(&forkChannel_, &forkLock->mChannelList));
    forkChannel = &forkChannel_;

    /* Always include stdin, stdout and stderr in the whitelisted
     * fds for the child, and never include these in the blacklisted
     * fds for the parent. If required, the child and the parent can
     * close these in their post fork methods. */

    const int stdfdlist[] =
    {
        STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO,
    };

    for (unsigned ix = 0; NUMBEROF(stdfdlist) > ix; ++ix)
    {
        ERROR_IF(
            insertFdSet(whitelistFds, stdfdlist[ix]) && EEXIST != errno);

        ERROR_IF(
            removeFdSet(blacklistFds, stdfdlist[ix]) && ENOENT != errno);
    }

    if (processLock_.mLock)
    {
        ERROR_IF(
            insertFdSetFile(whitelistFds,
                            processLock_.mLock->mFile) && EEXIST != errno);
        ERROR_IF(
            removeFdSetFile(blacklistFds,
                            processLock_.mLock->mFile) && ENOENT != errno);
    }

    /* Note that the fork() will complete and launch the child process
     * before the child pid is recorded in the local variable. This
     * is an important consideration for propagating signals to
     * the child process. */

    TEST_RACE
    ({
        childPid = Pid(fork());
    });

    switch (childPid.mPid)
    {
    default:
        ERROR_IF(
            forkProcessChild_PostParent_(
                forkChannel,
                aOption,
                childPid,
                aPgid,
                aPostForkParentMethod,
                blacklistFds));
        break;

    case -1:
        break;

    case 0:
        {
            /* At the first chance, reset the fork lock so that
             * its internal state can be synchronised to the new
             * child process. This allows the code in the
             * PostForkChildProcessMethod to invoke
             * acquireProcessForkChildLock_() without triggering
             * assertions. */

            struct ProcessForkChildLock_ *childForkLock = forkLock;

            forkLock = resetProcessForkChildLock_(forkLock);
            forkLock = acquireProcessForkChildLock_(childForkLock);
        }

        forkProcessChild_PostChild_(
                forkChannel,
                aOption,
                aPgid,
                aPostForkChildMethod,
                whitelistFds);

        forkChannel = closeForkProcessChannel_(forkChannel);
        forkLock    = releaseProcessForkChildLock_(forkLock);

        callForkMethod_(aForkMethod);

        ensure(0);
        break;
    }

    rc = childPid.mPid;

Finally:

    FINALLY
    ({
        forkChannel  = closeForkProcessChannel_(forkChannel);
        blacklistFds = closeFdSet(blacklistFds);
        whitelistFds = closeFdSet(whitelistFds);

        forkLock = releaseProcessForkChildLock_(forkLock);

        if (-1 == rc)
        {
            /* If the parent has successfully created a child process, but
             * there is a problem, then the child needs to be reaped. */

            if (-1 != childPid.mPid)
            {
                int status;
                ABORT_IF(
                    reapProcessChild(childPid, &status));
            }
        }
    });

    return Pid(rc);
}

/* -------------------------------------------------------------------------- */
struct ForkProcessDaemon
{
    struct PreForkProcessMethod       mPreForkMethod;
    struct PostForkChildProcessMethod mChildMethod;
    struct ForkProcessMethod          mForkMethod;
    struct SocketPair                *mSyncSocket;
    struct ThreadSigMask             *mSigMask;
};

struct ForkProcessDaemonSigHandler
{
    unsigned mHangUp;
};

static CHECKED int
forkProcessDaemonSignalHandler_(
    struct ForkProcessDaemonSigHandler *self,
    int                                 aSigNum,
    struct Pid                          aPid,
    struct Uid                          aUid)
{
    ++self->mHangUp;

    struct ProcessSignalName sigName;
    debug(1,
          "daemon received %s pid %" PRId_Pid " uid %" PRId_Uid,
          formatProcessSignalName(&sigName, aSigNum),
          FMTd_Pid(aPid),
          FMTd_Uid(aUid));

    return 0;
}

static int
forkProcessDaemonChild_(struct ForkProcessDaemon *self)
{
    int rc = -1;

    struct Pid daemonPid = ownProcessId();

    struct ForkProcessDaemonSigHandler sigHandler = { .mHangUp = 0 };

    ERROR_IF(
        watchProcessSignals(
            WatchProcessSignalMethod(
                &sigHandler, &forkProcessDaemonSignalHandler_)));

    /* Once the signal handler is established to catch SIGHUP, allow
     * the parent to stop and then make the daemon process an orphan. */

    ERROR_IF(
        waitThreadSigMask((const int []) { SIGHUP, 0 }));

    self->mSigMask = popThreadSigMask(self->mSigMask);

    debug(0, "daemon orphaned");

    ERROR_UNLESS(
        sizeof(daemonPid.mPid) == sendUnixSocket(
            self->mSyncSocket->mChildSocket,
            (void *) &daemonPid.mPid,
            sizeof(daemonPid.mPid)));

    char buf[1];
    ERROR_UNLESS(
        sizeof(buf) == recvUnixSocket(
            self->mSyncSocket->mChildSocket, buf, sizeof(buf)));

    self->mSyncSocket = closeSocketPair(self->mSyncSocket);

    if ( ! ownForkProcessMethodNil(self->mForkMethod))
        ERROR_IF(
            callForkProcessMethod(self->mForkMethod));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static int
forkProcessDaemonGuardian_(struct ForkProcessDaemon *self)
{
    int rc = -1;

    closeSocketPairParent(self->mSyncSocket);

    struct Pid daemonPid;
    ERROR_IF(
        (daemonPid = forkProcessChild(
            ForkProcessSetProcessGroup,
            Pgid(0),
            PreForkProcessMethod(
                self,
                LAMBDA(
                    int, (struct ForkProcessDaemon    *self_,
                          const struct PreForkProcess *aPreFork),
                    {
                        return fillFdSet(aPreFork->mWhitelistFds);
                    })),
            self->mChildMethod,
            PostForkParentProcessMethodNil(),
            ForkProcessMethod(
                self, forkProcessDaemonChild_)),
         -1 == daemonPid.mPid));

    /* Terminate the server to make the child an orphan. The child
     * will become the daemon, and when it is adopted by init(8).
     *
     * When a parent process terminates, Posix says:
     *
     *    o If the process is a controlling process, the SIGHUP signal
     *      shall be sent to each process in the foreground process group
     *      of the controlling terminal belonging to the calling process.
     *
     *    o If the exit of the process causes a process group to become
     *      orphaned, and if any member of the newly-orphaned process
     *      group is stopped, then a SIGHUP signal followed by a
     *      SIGCONT signal shall be sent to each process in the
     *      newly-orphaned process group.
     *
     * The server created here is not a controlling process since it is
     * not a session leader (although it might have a controlling terminal).
     * So no SIGHUP is sent for the first reason.
     *
     * To avoid ambiguity, the child is always placed into its own
     * process group and stopped, so that when it is orphaned it is
     * guaranteed to receive a SIGHUP signal. */

    while (1)
    {
        ERROR_IF(
            kill(daemonPid.mPid, SIGSTOP));

        monotonicSleep(Duration(NSECS(MilliSeconds(100))));

        struct ChildProcessState daemonStatus =
            monitorProcessChild(daemonPid);
        if (ChildProcessStateStopped == daemonStatus.mChildState)
            break;

        monotonicSleep(Duration(NSECS(Seconds(1))));
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static int
forkProcessDaemonPreparation_(struct ForkProcessDaemon    *self,
                              const struct PreForkProcess *aPreFork)
{
    int rc = -1;

    if ( ! ownPreForkProcessMethodNil(self->mPreForkMethod))
        ERROR_IF(
            callPreForkProcessMethod(self->mPreForkMethod, aPreFork));

    ERROR_IF(
        removeFdSet(
            aPreFork->mBlacklistFds,
            self->mSyncSocket->mParentSocket->mSocket->mFile->mFd) &&
        ENOENT != errno);

    ERROR_IF(
        insertFdSet(
            aPreFork->mWhitelistFds,
            self->mSyncSocket->mParentSocket->mSocket->mFile->mFd) &&
        EEXIST != errno);

    ERROR_IF(
        removeFdSet(
            aPreFork->mBlacklistFds,
            self->mSyncSocket->mChildSocket->mSocket->mFile->mFd) &&
        ENOENT != errno);

    ERROR_IF(
        insertFdSet(
            aPreFork->mWhitelistFds,
            self->mSyncSocket->mChildSocket->mSocket->mFile->mFd) &&
        EEXIST != errno);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

struct Pid
forkProcessDaemon(struct PreForkProcessMethod        aPreForkMethod,
                  struct PostForkChildProcessMethod  aPostForkChildMethod,
                  struct PostForkParentProcessMethod aPostForkParentMethod,
                  struct ForkProcessMethod           aForkMethod)
{
    pid_t rc = -1;

    struct ThreadSigMask  sigMask_;
    struct ThreadSigMask *sigMask = 0;

    struct SocketPair  syncSocket_;
    struct SocketPair *syncSocket = 0;

    sigMask = pushThreadSigMask(
        &sigMask_, ThreadSigMaskBlock, (const int []) { SIGHUP, 0 });

    ERROR_IF(
        createSocketPair(&syncSocket_, O_CLOEXEC));
    syncSocket = &syncSocket_;

    struct ForkProcessDaemon daemonProcess =
    {
        .mSigMask       = sigMask,
        .mPreForkMethod = aPreForkMethod,
        .mChildMethod   = aPostForkChildMethod,
        .mForkMethod    = aForkMethod,
        .mSyncSocket    = syncSocket,
    };

    struct Pid serverPid;
    ERROR_IF(
        (serverPid = forkProcessChild(
            ForkProcessInheritProcessGroup,
            Pgid(0),
            PreForkProcessMethod(
                &daemonProcess, forkProcessDaemonPreparation_),
            PostForkChildProcessMethod(
                &daemonProcess, forkProcessDaemonGuardian_),
            PostForkParentProcessMethod(
                &daemonProcess,
                LAMBDA(
                    int, (struct ForkProcessDaemon *self_,
                          struct Pid                aGuardianPid),
                    {
                        closeSocketPairChild(self_->mSyncSocket);
                        return 0;
                    })),
            ForkProcessMethodNil()),
         -1 == serverPid.mPid));

    int status;
    ERROR_IF(
        reapProcessChild(serverPid, &status));

    ERROR_UNLESS(
        WIFEXITED(status) && ! WEXITSTATUS(status));

    struct Pid daemonPid;
    ERROR_UNLESS(
        sizeof(daemonPid.mPid) == recvUnixSocket(syncSocket->mParentSocket,
                                                 (void *) &daemonPid.mPid,
                                                 sizeof(daemonPid.mPid)));

    char buf[1] = { 0 };
    ERROR_UNLESS(
        sizeof(buf) == sendUnixSocket(
            syncSocket->mParentSocket, buf, sizeof(buf)));

    rc = daemonPid.mPid;

Finally:

    FINALLY
    ({
        syncSocket = closeSocketPair(syncSocket);

        sigMask = popThreadSigMask(sigMask);
    });

    return Pid(rc);
}

/* -------------------------------------------------------------------------- */
void
execProcess(const char *aCmd, const char * const *aArgv)
{
    /* Call resetProcessSigPipe_() here to ensure that SIGPIPE will be
     * delivered to the new program. Note that it is possible that there
     * was no previous call to forkProcess(), though this is normally
     * the case. */

    ERROR_IF(
        resetProcessSigPipe_());

    ERROR_IF(
        errno = pthread_sigmask(SIG_SETMASK, &processSigMask_, 0));

    execvp(aCmd, (char * const *) aArgv);

Finally:

    FINALLY({});
}

/* -------------------------------------------------------------------------- */
void
execShell(const char *aCmd)
{
    const char *shell = getenv("SHELL");

    if ( ! shell)
        shell = "/bin/sh";

    const char *cmd[] = { shell, "-c", aCmd, 0 };

    execProcess(shell, cmd);
}

/* -------------------------------------------------------------------------- */
void
exitProcess(int aStatus)
{
    _exit(aStatus);

    while (1)
        raise(SIGKILL);
}

/* -------------------------------------------------------------------------- */
int
signalProcessGroup(struct Pgid aPgid, int aSignal)
{
    int rc = -1;

    struct ProcessSignalName sigName;

    ensure(aPgid.mPgid);

    debug(0,
          "sending %s to process group pgid %" PRId_Pgid,
          formatProcessSignalName(&sigName, aSignal),
          FMTd_Pgid(aPgid));

    ERROR_IF(
        killpg(aPgid.mPgid, aSignal));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
killProcess_(int aSigNum, unsigned *aSigTrigger) __attribute__((__noreturn__));

static void
killProcess_(int aSigNum, unsigned *aSigTrigger)
{
    /* When running under valgrind, do not abort() because it causes the
     * program to behave as if it received SIGKILL. Instead, exit the
     * program immediately and allow valgrind to survey the program for
     * for leaks. */

    if (RUNNING_ON_VALGRIND)
        _exit(128 + aSigNum);

    /* Other threads might be attaching or have attached a signal handler,
     * and the signal might be blocked.
     *
     * Also, multiple threads might call this function at the same time.
     *
     * Try to raise the signal in this thread, but also mark the signal
     * trigger which will be noticed if a handler is already attached.
     *
     * Do not call back into any application libraries to avoid the risk
     * of infinite recursion, however be aware that dispatchSigHandler_()
     * might end up calling into this function recursively.
     *
     * Do not call library functions such as abort(3) because they will
     * try to flush IO streams and perform other activity that might fail. */

    __sync_or_and_fetch(aSigTrigger, 1);

    do
    {
        sigset_t sigSet;

        if (pthread_sigmask(SIG_SETMASK, 0, &sigSet))
            break;

        if (1 == sigismember(&sigSet, aSigNum))
        {
            if (sigdelset(&sigSet, aSigNum))
                break;

            if (pthread_sigmask(SIG_SETMASK, &sigSet, 0))
                break;
        }

        for (unsigned ix = 0; 10 > ix; ++ix)
        {
            struct sigaction sigAction = { .sa_handler = SIG_DFL };

            if (sigaction(aSigNum, &sigAction, 0))
                break;

            /* There is a window here for another thread to configure the
             * signal to be ignored, or handled. So when the signal is raised,
             * it might not actually cause the process to abort. */

            if ( ! testAction(TestLevelRace))
            {
                if (raise(aSigNum))
                    break;
            }

            sigset_t pendingSet;

            if (sigpending(&pendingSet))
                break;

            int pendingSignal = sigismember(&pendingSet, aSigNum);
            if (-1 == pendingSignal)
                break;

            if (pendingSignal)
                monotonicSleep(
                    Duration(NSECS(MilliSeconds(100))));
        }
    }
    while (0);

    /* There was an error trying to deliver the signal to the process, so
     * try one last time, then resort to killing the process. */

    if ( ! testAction(TestLevelRace))
        raise(aSigNum);

    while (1)
    {
        monotonicSleep(
            Duration(NSECS(Seconds(1))));

        if ( ! testAction(TestLevelRace))
            raise(SIGKILL);
    }
}

/* -------------------------------------------------------------------------- */
void
abortProcess(void)
{
    killProcess_(SIGABRT, &processAbort_);
}

void
quitProcess(void)
{
    killProcess_(SIGQUIT, &processQuit_);
}

/* -------------------------------------------------------------------------- */
const char *
ownProcessName(void)
{
    extern const char *__progname;

    return programName_ ? programName_ : __progname;
}

/* -------------------------------------------------------------------------- */
struct Pid
ownProcessParentId(void)
{
    return Pid(getppid());
}

/* -------------------------------------------------------------------------- */
struct Pid
ownProcessId(void)
{
    return Pid(getpid());
}

/* -------------------------------------------------------------------------- */
struct Pgid
ownProcessGroupId(void)
{
    return Pgid(getpgid(0));
}

/* -------------------------------------------------------------------------- */
struct Pgid
fetchProcessGroupId(struct Pid aPid)
{
    ensure(aPid.mPid);

    return Pgid(getpgid(aPid.mPid));
}

/* -------------------------------------------------------------------------- */
struct ExitCode
extractProcessExitStatus(int aStatus, struct Pid aPid)
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
    }
    else if (WIFSIGNALED(aStatus))
    {
        struct ProcessSignalName sigName;

        debug(
            0,
            "process pid %" PRId_Pid " terminated by %s",
            FMTd_Pid(aPid),
            formatProcessSignalName(&sigName, WTERMSIG(aStatus)));

        exitCode.mStatus = 128 + WTERMSIG(aStatus);
        if (255 < exitCode.mStatus)
            exitCode.mStatus = 255;
    }

    debug(0,
          "process pid %" PRId_Pid " exit code %" PRId_ExitCode,
          FMTd_Pid(aPid),
          FMTd_ExitCode(exitCode));

    return exitCode;
}

/* -------------------------------------------------------------------------- */
struct Duration
ownProcessElapsedTime(void)
{
    return
        Duration(
            NanoSeconds(
                processTimeBase_.monotonic.ns
                ? monotonicTime().monotonic.ns - processTimeBase_.monotonic.ns
                : 0));
}

/* -------------------------------------------------------------------------- */
struct MonotonicTime
ownProcessBaseTime(void)
{
    return processTimeBase_;
}

/* -------------------------------------------------------------------------- */
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

    walkFileList(FileVisitor(
                     &numFds,
                     LAMBDA(
                         int, (unsigned *aNumFds, const struct File *aFile),
                         {
                             ++(*aNumFds);

                             return 0;
                         })));

    /* Create the whitelist of file descriptors by copying the fds
     * from each of the explicitly created file descriptors. */

    int whiteList[numFds];

    {
        struct ProcessFdWhiteList
        {
            int     *mList;
            unsigned mLen;

        } fdWhiteList =
        {
            .mList = whiteList,
            .mLen  = 0,
        };

        for (unsigned jx = 0; NUMBEROF(stdfds) > jx; ++jx)
            fdWhiteList.mList[fdWhiteList.mLen++] = stdfds[jx];

        walkFileList(
            FileVisitor(
                &fdWhiteList,
                LAMBDA(
                    int, (struct ProcessFdWhiteList *aWhiteList,
                          const struct File         *aFile),
                    {
                        aWhiteList->mList[aWhiteList->mLen++] = aFile->mFd;

                        return 0;
                    })));

        ensure(fdWhiteList.mLen == numFds);
    }

    ERROR_IF(
        closeFdDescriptors(whiteList, NUMBEROF(whiteList)));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
prepareFork_(void)
{
    /* Acquire processFork_.mMutex to allow only one thread to
     * use the shared process fork structure instance at a time. */

    processFork_.mMutex = lockMutex(&processFork_.mMutex_);

    /* Note that processLock_.mMutex is recursive, meaning that
     * it might already be held by this thread on entry to this function. */

    processFork_.mLock = lockThreadSigMutex(processLock_.mMutex);

    ensure(
        0 < ownThreadSigMutexLocked(processLock_.mMutex));

    /* Acquire the processSigVecLock_ for writing to ensure that there
     * are no other signal vector activity in progress. The purpose here
     * is to prevent the signal mutexes from being held while a fork is
     * in progress, since those locked mutexes will then be transferred
     * into the child process. */

    processFork_.mSigVecLock = createRWMutexWriter(
        &processFork_.mSigVecLock_, &processSignals_.mVectorLock);

    /* Acquire the processSigMutex_ to ensure that there is no other
     * signal handler activity while the fork is in progress. */

    processFork_.mSigLock = lockThreadSigMutex(&processSignals_.mSignalMutex);

    processFork_.mParentPid = ownProcessId();

    debug(1, "prepare fork");
}

static void
completeFork_(void)
{
    TEST_RACE
    ({
        /* This function is called in the context of both parent and child
         * process immediately after the fork completes. Both processes
         * release the resources acquired when preparations were made
         * immediately preceding the fork. */

        processFork_.mSigLock =
            unlockThreadSigMutex(processFork_.mSigLock);

        processFork_.mSigVecLock =
            destroyRWMutexWriter(processFork_.mSigVecLock);

        processFork_.mLock = unlockThreadSigMutex(processLock_.mMutex);

        pthread_mutex_t *lock = processFork_.mMutex;

        processFork_.mMutex = 0;

        lock = unlockMutex(lock);
    });
}

static void
postForkParent_(void)
{
    /* This function is called in the context of the parent process
     * immediately after the fork completes. */

    debug(1, "groom forked parent");

    ensure(ownProcessId().mPid == processFork_.mParentPid.mPid);

    completeFork_();
}

static void
postForkChild_(void)
{
    /* This function is called in the context of the child process
     * immediately after the fork completes, at which time it will
     * be the only thread running in the new process. The application
     * lock is recursive in the parent, and hence also in the child.
     * The parent holds the application lock, so the child must acquire
     * the lock to ensure that the recursive semantics in the child
     * are preserved. */

    if (processLock_.mLock)
        forkProcessLock_(processLock_.mLock);

    debug(1, "groom forked child");

    /* Do not check the parent pid here because it is theoretically possible
     * that the parent will have terminated and the pid reused by the time
     * the child gets around to checking. */

    completeFork_();
}

static struct ProcessForkChildLock_ *processAtForkLock_;

static void
prepareProcessFork_(void)
{
    processAtForkLock_ = acquireProcessForkChildLock_(&processForkChildLock_);

    if (moduleInit_)
        prepareFork_();
}

static void
postProcessForkParent_(void)
{
    if (moduleInit_)
        postForkParent_();

    struct ProcessForkChildLock_ *lock = processAtForkLock_;

    processAtForkLock_ = 0;

    lock = releaseProcessForkChildLock_(lock);
}

static void
postProcessForkChild_(void)
{
    if (moduleInit_)
        postForkChild_();

    struct ProcessForkChildLock_ *lock = processAtForkLock_;

    processAtForkLock_ = 0;

    lock = releaseProcessForkChildLock_(lock);
}

/* -------------------------------------------------------------------------- */
int
Process_init(struct ProcessModule *self, const char *aArg0)
{
    int rc = -1;

    bool hookedSignals = false;

    self->mModule      = self;
    self->mErrorModule = 0;

    ensure( ! moduleInit_);

    processArg0_ = aArg0;

    programName_ = strrchr(processArg0_, '/');
    programName_ = programName_ ? programName_ + 1 : processArg0_;

    /* Open file descriptors to overlay stdin, stdout and stderr
     * to prevent other files inadvertently taking on these personas. */

    ERROR_IF(
        openStdFds());

    /* Ensure that the recorded time base is non-zero to allow it
     * to be distinguished from the case that it was not recorded
     * at all, and also ensure that the measured elapsed process
     * time is always non-zero. */

    if ( ! moduleInitOnce_)
    {
        processTimeBase_ = monotonicTime();
        do
            --processTimeBase_.monotonic.ns;
        while ( ! processTimeBase_.monotonic.ns);

        moduleInitOnce_ = true;
    }

    if ( ! moduleInitAtFork_)
    {
        /* Ensure that the synchronisation and signal functions are
         * prepared when a fork occurs so that they will be available
         * for use in the child process. Be aware that once functions
         * are registered, there is no way to deregister them. */

        ERROR_IF(
            errno = pthread_atfork(
                prepareProcessFork_,
                postProcessForkParent_,
                postProcessForkChild_));

        moduleInitAtFork_ = true;
    }

    ERROR_IF(
        Error_init(&self->mErrorModule_));
    self->mErrorModule = &self->mErrorModule_;

    ERROR_IF(
        errno = pthread_sigmask(SIG_BLOCK, 0, &processSigMask_));

    ensure( ! processLock_.mLock);

    ERROR_IF(
        createProcessLock_(&processLock_.mLock_));
    processLock_.mLock = &processLock_.mLock_;

    ERROR_IF(
        hookProcessSigCont_());
    ERROR_IF(
        hookProcessSigStop_());
    hookedSignals = true;

    ++moduleInit_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            if (hookedSignals)
            {
                ABORT_IF(
                    unhookProcessSigStop_());
                ABORT_IF(
                    unhookProcessSigCont_());
            }

            struct ProcessLock *processLock = processLock_.mLock;
            processLock_.mLock = 0;

            closeProcessLock_(processLock);

            self->mErrorModule = Error_exit(self->mErrorModule);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct ProcessModule *
Process_exit(struct ProcessModule *self)
{
    if (self)
    {
        ensure( ! --moduleInit_);

        ABORT_IF(
            unhookProcessSigStop_());
        ABORT_IF(
            unhookProcessSigCont_());

        struct ProcessLock *processLock = processLock_.mLock;
        processLock_.mLock = 0;

        ensure(processLock);
        closeProcessLock_(processLock);

        ABORT_IF(
            errno = pthread_sigmask(SIG_SETMASK, &processSigMask_, 0));

        self->mErrorModule = Error_exit(self->mErrorModule);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
