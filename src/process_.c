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
#include "socketpair_.h"
#include "bellsocketpair_.h"
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
#include <signal.h>

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

static pthread_rwlock_t      processSigVecLock_ = PTHREAD_RWLOCK_INITIALIZER;
static struct ThreadSigMutex processSigMutex_ = THREAD_SIG_MUTEX_INITIALIZER;

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
    .mMutex_ = THREAD_SIG_MUTEX_INITIALIZER,
    .mMutex  = &processLock_.mMutex_,
};

static struct
{
    pthread_mutex_t      mMutex;
    struct Pid           mParentPid;
    struct RWMutexWriter mForkLock;
} processFork_ =
{
    .mMutex = PTHREAD_MUTEX_INITIALIZER,
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

static struct ProcessSignalVector
{
    struct sigaction mAction;
    pthread_mutex_t  mMutex_;
    pthread_mutex_t *mMutex;
} processSignalVectors_[NSIG];

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
dispatchSigAction_(int aSigNum, siginfo_t *aSigInfo, void *aSigContext)
{
    FINALLY
    ({
        struct ProcessSignalVector *sv = &processSignalVectors_[aSigNum];

        struct ProcessSignalName sigName;
        debug(1,
              "dispatch signal %s",
              formatProcessSignalName(&sigName, aSigNum));

        struct RWMutexReader forkLock;

        createRWMutexReader(&forkLock, &processSigVecLock_);
        lockMutex(sv->mMutex);
        {
            dispatchSigExit_(aSigNum);

            if (SIG_DFL != sv->mAction.sa_handler &&
                SIG_IGN != sv->mAction.sa_handler)
            {
                ++processSignalContext_;

                enum ErrorFrameStackKind stackKind =
                    switchErrorFrameStack(ErrorFrameStackSignal);

                struct ErrorFrameSequence frameSequence =
                    pushErrorFrameSequence();

                sv->mAction.sa_sigaction(aSigNum, aSigInfo, aSigContext);

                popErrorFrameSequence(frameSequence);

                switchErrorFrameStack(stackKind);

                --processSignalContext_;
            }
        }
        unlockMutex(sv->mMutex);
        destroyRWMutexReader(&forkLock);
    });
}

static void
dispatchSigHandler_(int aSigNum)
{
    FINALLY
    ({
        struct ProcessSignalVector *sv = &processSignalVectors_[aSigNum];

        struct ProcessSignalName sigName;
        debug(1,
              "dispatch signal %s",
              formatProcessSignalName(&sigName, aSigNum));

        struct RWMutexReader forkLock;

        createRWMutexReader(&forkLock, &processSigVecLock_);
        lockMutex(sv->mMutex);
        {
            dispatchSigExit_(aSigNum);

            if (SIG_DFL != sv->mAction.sa_handler &&
                SIG_IGN != sv->mAction.sa_handler)
            {
                ++processSignalContext_;

                enum ErrorFrameStackKind stackKind =
                    switchErrorFrameStack(ErrorFrameStackSignal);

                struct ErrorFrameSequence frameSequence =
                    pushErrorFrameSequence();

                sv->mAction.sa_handler(aSigNum);

                popErrorFrameSequence(frameSequence);

                switchErrorFrameStack(stackKind);

                --processSignalContext_;
            }
        }
        unlockMutex(sv->mMutex);
        destroyRWMutexReader(&forkLock);
    });
}

static int
changeSigAction_(unsigned          aSigNum,
                 struct sigaction  aNewAction,
                 struct sigaction *aOldAction)
{
    int rc = -1;

    ensure(NUMBEROF(processSignalVectors_) > aSigNum);

    struct RWMutexReader  sigVecLock_;
    struct RWMutexReader *sigVecLock = 0;

    struct ThreadSigMask  threadSigMask_;
    struct ThreadSigMask *threadSigMask = 0;

    pthread_mutex_t *sigLock = 0;

    struct sigaction nextAction = aNewAction;

    if (SIG_DFL != nextAction.sa_handler && SIG_IGN != nextAction.sa_handler)
    {
        if (nextAction.sa_flags & SA_SIGINFO)
            nextAction.sa_sigaction = dispatchSigAction_;
        else
            nextAction.sa_handler = dispatchSigHandler_;

        /* Require that signal delivery is not recursive to avoid
         * having to deal with too many levels of re-entrancy. */

        sigset_t filledSigSet;
        ERROR_IF(
            sigfillset(&filledSigSet));

        nextAction.sa_mask   = filledSigSet;
        nextAction.sa_flags &= ~ SA_NODEFER;
    }

    lockThreadSigMutex(&processSigMutex_);
    if ( ! processSignalVectors_[aSigNum].mMutex)
        processSignalVectors_[aSigNum].mMutex =
            createMutex(&processSignalVectors_[aSigNum].mMutex_);
    unlockThreadSigMutex(&processSigMutex_);

    /* Block signal delivery into this thread to avoid the signal
     * dispatch attempting to acquire the dispatch mutex recursively
     * in the same thread context. */

    sigVecLock = createRWMutexReader(&sigVecLock_, &processSigVecLock_);

    pushThreadSigMask(
        &threadSigMask_, ThreadSigMaskBlock, (const int []) { aSigNum, 0 });

    threadSigMask = &threadSigMask_;

    sigLock = lockMutex(processSignalVectors_[aSigNum].mMutex);

    struct sigaction prevAction;
    ERROR_IF(
        sigaction(aSigNum, &nextAction, &prevAction));

    /* Do not overwrite the output result unless the underlying
     * sigaction() call succeeds. */

    if (aOldAction)
        *aOldAction = prevAction;

    processSignalVectors_[aSigNum].mAction = aNewAction;

    rc = 0;

Finally:

    FINALLY
    ({
        sigLock = unlockMutex(sigLock);

        popThreadSigMask(threadSigMask);

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

static int
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
    struct ThreadSigMutex mSigMutex;
    unsigned              mCount;
    struct IntMethod      mMethod;
} processSigCont_ =
{
    .mSigMutex = THREAD_SIG_MUTEX_INITIALIZER,
};

static void
sigCont_(int aSigNum)
{
    /* See the commentary in ownProcessSigContCount() to understand
     * the motivation for using a lock free update here. Other solutions
     * are possible, but a lock free approach is the most straightforward. */

    __sync_add_and_fetch(&processSigCont_.mCount, 2);

    lockThreadSigMutex(&processSigCont_.mSigMutex);

    if (ownIntMethodNil(processSigCont_.mMethod))
        debug(1, "detected SIGCONT");
    else
    {
        debug(1, "observed SIGCONT");
        ABORT_IF(
            callIntMethod(processSigCont_.mMethod));
    }

    unlockThreadSigMutex(&processSigCont_.mSigMutex);
}

static int
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

static int
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

static int
updateProcessSigContMethod_(struct IntMethod aMethod)
{
    lockThreadSigMutex(&processSigCont_.mSigMutex);
    processSigCont_.mMethod = aMethod;
    unlockThreadSigMutex(&processSigCont_.mSigMutex);

    return 0;
}

static int
resetProcessSigCont_(void)
{
    int rc = -1;

    ERROR_IF(
        updateProcessSigContMethod_(IntMethodNil()));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
watchProcessSigCont(struct IntMethod aMethod)
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
static unsigned
fetchProcessSigContTracker_(void)
{
    /* Because this function is called from lockMutex(), amongst other places,
     * do not use or cause lockMutex() to be used here to avoid introducing
     * the chance of infinite recursion. */

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
    struct ThreadSigMutex mSigMutex;
    struct IntMethod      mMethod;
} processSigStop_ =
{
    .mSigMutex = THREAD_SIG_MUTEX_INITIALIZER,
};

static void
sigStop_(int aSigNum)
{
    lockThreadSigMutex(&processSigStop_.mSigMutex);

    if (ownIntMethodNil(processSigStop_.mMethod))
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
            callIntMethod(processSigStop_.mMethod));
    }

    unlockThreadSigMutex(&processSigStop_.mSigMutex);
}

static int
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

static int
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

static int
updateProcessSigStopMethod_(struct IntMethod aMethod)
{
    lockThreadSigMutex(&processSigStop_.mSigMutex);
    processSigStop_.mMethod = aMethod;
    unlockThreadSigMutex(&processSigStop_.mSigMutex);

    return 0;
}

static int
resetProcessSigStop_(void)
{
    int rc = -1;

    ERROR_IF(
        updateProcessSigStopMethod_(IntMethodNil()));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
watchProcessSigStop(struct IntMethod aMethod)
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
static struct IntMethod processSigChldMethod_;

static void
sigChld_(int aSigNum)
{
    if ( ! ownIntMethodNil(processSigChldMethod_))
    {
        debug(1, "observed SIGCHLD");
        ABORT_IF(
            callIntMethod(processSigChldMethod_));
    }
}

static int
resetProcessChildrenWatch_(void)
{
    int rc = -1;

    ERROR_IF(
        changeSigAction_(
            SIGCHLD,
            (struct sigaction) { .sa_handler = SIG_DFL },
            0));

    processSigChldMethod_ = IntMethodNil();

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
watchProcessChildren(struct IntMethod aMethod)
{
    int rc = -1;

    struct IntMethod sigChldMethod = processSigChldMethod_;

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
static struct Duration  processClockTickPeriod_;
static struct IntMethod processClockMethod_;
static struct sigaction processClockTickSigAction_ =
{
    .sa_handler = SIG_ERR,
};

static void
clockTick_(int aSigNum)
{
    if (ownIntMethodNil(processClockMethod_))
        debug(1, "received clock tick");
    else
    {
        debug(1, "observed clock tick");
        ABORT_IF(
            callIntMethod(processClockMethod_));
    }
}

static int
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

        processClockMethod_ = IntMethodNil();

        processClockTickSigAction_.sa_handler = SIG_ERR;
        processClockTickSigAction_.sa_flags = 0;

        processClockTickPeriod_ = Duration(NanoSeconds(0));
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
watchProcessClock(struct IntMethod aMethod,
                  struct Duration  aClockPeriod)
{
    int rc = -1;

    struct IntMethod clockMethod = processClockMethod_;

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
static struct IntIntMethod processWatchedSignalMethod_;

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
caughtSignal_(int aSigNum)
{
    if ( ! ownIntIntMethodNil(processWatchedSignalMethod_))
    {
        struct ProcessSignalName sigName;

        debug(1, "observed %s", formatProcessSignalName(&sigName, aSigNum));

        callIntIntMethod(processWatchedSignalMethod_, aSigNum);
    }
}

int
watchProcessSignals(struct IntIntMethod aMethod)
{
    int rc = -1;

    processWatchedSignalMethod_ = aMethod;

    for (unsigned ix = 0; NUMBEROF(processWatchedSignals_) > ix; ++ix)
    {
        struct SignalWatch *watchedSig = processWatchedSignals_ + ix;

        ERROR_IF(
            changeSigAction_(
                watchedSig->mSigNum,
                (struct sigaction) { .sa_handler = caughtSignal_ },
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

            processWatchedSignalMethod_ = IntIntMethodNil();
        }
    });

    return rc;
}

static int
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

    processWatchedSignalMethod_ = IntIntMethodNil();

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
void
initProcessDirName(struct ProcessDirName *self, struct Pid aPid)
{
    sprintf(self->mDirName, PROCESS_DIRNAME_FMT_, FMTd_Pid(aPid));
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
        sprintf(self->mSignalText_, PROCESS_SIGNALNAME_FMT_, aSigNum);
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

    initProcessDirName(&processDirName, aPid);

    static const char processStatFileNameFmt_[] = "%s/stat";

    char processStatFileName[strlen(processDirName.mDirName) +
                             sizeof(processStatFileNameFmt_)];

    sprintf(processStatFileName,
            processStatFileNameFmt_, processDirName.mDirName);

    ERROR_IF(
        (statFd = open(processStatFileName, O_RDONLY),
         -1 == statFd));

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
static int
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
        closeFile(self->mFile);
}

/* -------------------------------------------------------------------------- */
static int
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

    struct ThreadSigMutex *lock = 0;

    lock = lockThreadSigMutex(processLock_.mMutex);

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

    switch (siginfo.si_code)
    {
    default:
        ERROR_IF(
            true,
            {
                errno = EINVAL;
            });

    case CLD_EXITED: rc.mChildState = ChildProcessStateExited;  break;
    case CLD_KILLED: rc.mChildState = ChildProcessStateKilled;  break;
    }

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

    siginfo.si_pid = 0;
    ERROR_IF(
        waitid(P_PID, aPid.mPid, &siginfo,
               WEXITED | WSTOPPED | WCONTINUED | WNOHANG | WNOWAIT));

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
static void
callForkMethod_(struct IntMethod aMethod)
{
    if ( ! ownIntMethodNil(aMethod))
    {
        int status;
        ABORT_IF(
            (status = callIntMethod(aMethod),
             -1 == status || (errno = 0, 0 > status || 255 < status)),
            {
                if (-1 != status)
                    terminate(
                        0,
                        "Out of range exit status %d", status);
            });

        exitProcess(status);
    }
}

/* -------------------------------------------------------------------------- */
struct Pid
forkProcessChild(enum ForkProcessOption aOption,
                 struct Pgid            aPgid,
                 struct IntMethod       aMethod)
{
    pid_t rc = -1;

    const char *err = 0;

    pid_t pgid = aPgid.mPgid;

    ensure(ForkProcessSetProcessGroup == aOption || ! pgid);

#ifdef __linux__
    long clocktick;
    ERROR_IF(
        (clocktick = sysconf(_SC_CLK_TCK),
         -1 == clocktick));
#endif

    /* Note that the fork() will complete and launch the child process
     * before the child pid is recorded in the local variable. This
     * is an important consideration for propagating signals to
     * the child process. */

    pid_t childPid;

    TEST_RACE
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
            ERROR_IF(
                setpgid(childPid, pgid ? pgid : childPid));

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

        /* Ensure that the behaviour of each child diverges from the
         * behaviour of the parent. This is primarily useful for
         * testing. */

        srandom(ownProcessId().mPid);

        if (ForkProcessSetSessionLeader == aOption)
        {
            ERROR_IF(
                -1 == setsid(),
                {
                    err = "Unable to set process session";
                });
        }
        else if (ForkProcessSetProcessGroup == aOption)
        {
            ERROR_IF(
                setpgid(0, pgid),
                {
                    err = "Unable to set process group";
                });
        }

        /* Reset all the signals so that the child will not attempt
         * to catch signals. The parent should have set the signal
         * mask appropriately. */

        ERROR_IF(
            resetSignals_(),
            {
                err = "Unable to reset signal handlers";
            });

        callForkMethod_(aMethod);

        break;
    }

    rc = childPid;

Finally:

    FINALLY
    ({
        int errcode = errno;

        ABORT_IF(
            err,
            {
                terminate(errcode, "%s", err);
            });
    });

    return Pid(rc);
}

/* -------------------------------------------------------------------------- */
struct ForkProcessDaemon
{
    unsigned mHangUp;
};

static int
forkProcessDaemonSignalHandler_(struct ForkProcessDaemon *self, int aSigNum)
{
    ++self->mHangUp;

    struct ProcessSignalName sigName;
    debug(1,
          "daemon received %s",
          formatProcessSignalName(&sigName, aSigNum));

    return 0;
}

struct Pid
forkProcessDaemon(struct IntMethod aForkMethod)
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

    struct Pid serverPid;
    ERROR_IF(
        (serverPid = forkProcessChild(ForkProcessInheritProcessGroup,
                                      Pgid(0),
                                      IntMethodNil()),
         -1 == serverPid.mPid));

    struct Pid daemonPid;

    if (serverPid.mPid)
    {
        closeSocketPairChild(syncSocket);

        int status;
        ERROR_IF(
            reapProcessChild(serverPid, &status));

        ERROR_UNLESS(
            WIFEXITED(status) && ! WEXITSTATUS(status));

        ERROR_UNLESS(
            sizeof(daemonPid.mPid) == recvUnixSocket(syncSocket->mParentSocket,
                                                     (void *) &daemonPid.mPid,
                                                     sizeof(daemonPid.mPid)));

        char buf[1] = { 0 };
        ERROR_UNLESS(
            sizeof(buf) == sendUnixSocket(
                syncSocket->mParentSocket, buf, sizeof(buf)));
    }
    else
    {
        closeSocketPairParent(syncSocket);

        struct BellSocketPair bellSocket;

        ABORT_IF(
            createBellSocketPair(&bellSocket, 0));

        ABORT_IF(
            (daemonPid = forkProcessChild(ForkProcessSetProcessGroup,
                                          Pgid(0),
                                          IntMethodNil()),
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

        if (daemonPid.mPid)
        {
            closeBellSocketPairChild(&bellSocket);
            ABORT_IF(
                waitBellSocketPairParent(&bellSocket, 0));
            closeBellSocketPair(&bellSocket);

            while (1)
            {
                ABORT_IF(
                    kill(daemonPid.mPid, SIGSTOP));

                monotonicSleep(Duration(NSECS(MilliSeconds(100))));

                struct ChildProcessState daemonStatus =
                    monitorProcessChild(daemonPid);
                if (ChildProcessStateStopped == daemonStatus.mChildState)
                    break;

                monotonicSleep(Duration(NSECS(Seconds(1))));
            }

            /* When running with valgrind, use execl() to prevent
             * valgrind performing a leak check on the intermediate
             * process. */

            if (RUNNING_ON_VALGRIND)
                ABORT_IF(
                    execl(
                        "/bin/true", "true", (char *) 0) || (errno = 0, true));

            exitProcess(EXIT_SUCCESS);
        }

        daemonPid = ownProcessId();

        struct ForkProcessDaemon processDaemon = { .mHangUp = 0 };

        ABORT_IF(
            watchProcessSignals(
                IntIntMethod(
                    &forkProcessDaemonSignalHandler_, &processDaemon)));

        closeBellSocketPairParent(&bellSocket);
        ABORT_IF(
            ringBellSocketPairChild(&bellSocket));
        closeBellSocketPair(&bellSocket);

        /* Once the signal handler is established to catch SIGHUP, allow
         * the parent to stop and then make the daemon process an orphan. */

        ABORT_IF(
            waitThreadSigMask((const int []) { SIGHUP, 0 }));

        debug(0, "daemon orphaned");

        ABORT_UNLESS(
            sizeof(daemonPid.mPid) == sendUnixSocket(syncSocket->mChildSocket,
                                                     (void *) &daemonPid.mPid,
                                                     sizeof(daemonPid.mPid)));

        char buf[1];
        ABORT_UNLESS(
            sizeof(buf) == recvUnixSocket(
                syncSocket->mChildSocket, buf, sizeof(buf)));

        callForkMethod_(aForkMethod);

        daemonPid = Pid(0);
    }

    rc = daemonPid.mPid;

Finally:

    FINALLY
    ({
        closeSocketPair(syncSocket);

        sigMask = popThreadSigMask(sigMask);
    });

    return Pid(rc);
}

/* -------------------------------------------------------------------------- */
void
execProcess(const char *aCmd, char **aArgv)
{
    /* Call resetProcessSigPipe_() here to ensure that SIGPIPE will be
     * delivered to the new program. Note that it is possible that there
     * was no previous call to forkProcess(), though this is normally
     * the case. */

    ERROR_IF(
        resetProcessSigPipe_());

    ERROR_IF(
        errno = pthread_sigmask(SIG_SETMASK, &processSigMask_, 0));

    execvp(aCmd, aArgv);

Finally:

    FINALLY({});
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
            {
                struct timespec sleepTime =
                {
                    .tv_sec  = 0,
                    .tv_nsec = 100 * 1000 * 1000,
                };

                if (-1 == nanosleep(&sleepTime, 0) && EINTR != errno)
                    break;
            }
        }
    }
    while (0);

    /* There was an error trying to deliver the signal to the process, so
     * try one last time, then resort to killing the process. */

    if ( ! testAction(TestLevelRace))
        raise(aSigNum);

    while (1)
    {
        struct timespec sleepTime = { .tv_sec = 1 };

        nanosleep(&sleepTime, 0);

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
fetchProcessSignature(struct Pid aPid, char **aSignature)
{
    int rc = -1;
    int fd = -1;

    char *buf       = 0;
    char *signature = 0;

    const char *incarnation;

    ERROR_UNLESS(
        (incarnation = fetchSystemIncarnation()));

    /* Note that it is expected that forkProcess() will guarantee that
     * the pid of the child process combined with its signature results
     * in a universally unique key. Because the pid is recycled over time
     * (as well as being reused after each reboot), the signature must
     * unambiguously qualify the pid. */

    do
    {
        struct ProcessDirName processDirName;

        initProcessDirName(&processDirName, aPid);

        static const char processStatFileNameFmt_[] = "%s/stat";

        char processStatFileName[strlen(processDirName.mDirName) +
                                 sizeof(processStatFileNameFmt_)];

        sprintf(processStatFileName,
                processStatFileNameFmt_, processDirName.mDirName);

        ERROR_IF(
            (fd = open(processStatFileName, O_RDONLY),
             -1 == fd));

    } while (0);

    ssize_t buflen;
    ERROR_IF(
        (buflen = readFdFully(fd, &buf, 0),
         -1 == buflen));
    ERROR_UNLESS(
        buflen,
        {
            errno = ERANGE;
        });

    char *bufend = buf + buflen;
    char *word;
    ERROR_UNLESS(
        (word = memrchr(buf, ')', buflen)),
        {
            errno = ERANGE;
        });

    for (unsigned ix = 2; 22 > ix; ++ix)
    {
        while (word != bufend && ! isspace((unsigned char) *word))
            ++word;

        ERROR_IF(
            word == bufend,
            {
                errno = ERANGE;
            });

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

        ERROR_UNLESS(
            (signature = malloc(signatureLen)));

        ERROR_IF(
            0 > sprintf(signature, signatureFmt, incarnation, timestamp));

    } while (0);

    *aSignature = signature;
    signature   = 0;

    rc = 0;

Finally:

    FINALLY
    ({
        fd = closeFd(fd);

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

    lockMutex(&processFork_.mMutex);

    /* Note that processLock_.mMutex is recursive, meaning that
     * it might already be held by this thread on entry to this function. */

    lockThreadSigMutex(processLock_.mMutex);

    ensure(
        0 < ownThreadSigMutexLocked(processLock_.mMutex));

    /* Acquire the processSigVecLock_ for writing to ensure that there
     * are no other signal vector activity in progress. The purpose here
     * is to prevent the signal mutexes from being held while a fork is
     * in progress, since those locked mutexes will then be transferred
     * into the child process. */

    createRWMutexWriter(&processFork_.mForkLock, &processSigVecLock_);

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

        destroyRWMutexWriter(&processFork_.mForkLock);

        unlockThreadSigMutex(processLock_.mMutex);

        unlockMutex(&processFork_.mMutex);
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

static void
prepareProcessFork_(void)
{
    if (moduleInit_)
        prepareFork_();
}

static void
postProcessForkParent_(void)
{
    if (moduleInit_)
        postForkParent_();
}

static void
postProcessForkChild_(void)
{
    if (moduleInit_)
        postForkChild_();
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

        srandom(ownProcessId().mPid);

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

    hookProcessSigCont_();
    hookProcessSigStop_();
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
                unhookProcessSigStop_();
                unhookProcessSigCont_();
            }

            struct ProcessLock *processLock = processLock_.mLock;
            processLock_.mLock = 0;

            closeProcessLock_(processLock);

            Error_exit(self->mErrorModule);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
void
Process_exit(struct ProcessModule *self)
{
    if (self)
    {
        ensure( ! --moduleInit_);

        unhookProcessSigStop_();
        unhookProcessSigCont_();

        struct ProcessLock *processLock = processLock_.mLock;
        processLock_.mLock = 0;

        ensure(processLock);
        closeProcessLock_(processLock);

        ABORT_IF(
            errno = pthread_sigmask(SIG_SETMASK, &processSigMask_, 0));

        Error_exit(self->mErrorModule);
    }
}

/* -------------------------------------------------------------------------- */
