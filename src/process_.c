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

#include <valgrind/valgrind.h>

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
static unsigned               sProcessAbort;

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

static const char *sSignalNames[NSIG] =
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
static unsigned __thread sProcessSignalContext;

static struct ProcessSignalVector
{
    struct sigaction mAction;
    pthread_mutex_t  mMutex_;
    pthread_mutex_t *mMutex;
} sSignalVectors[NSIG];

static void
dispatchSigAbort_(void)
{
    if (sProcessAbort)
    {
        /* A handler for SIGABRT is established, but abortProcess() was
         * invoked. Abort the process from here. */

        abortProcess();
    }
}

static void
dispatchSigAction_(int aSigNum, siginfo_t *aSigInfo, void *aSigContext)
{
    FINALLY
    ({
        struct ProcessSignalVector *sv = &sSignalVectors[aSigNum];

        struct ProcessSignalName sigName;
        debug(1,
              "dispatch signal %s",
              formatProcessSignalName(&sigName, aSigNum));

        lockMutex(sv->mMutex);
        {
            if (SIGABRT == aSigNum)
                dispatchSigAbort_();

            if (SIG_DFL != sv->mAction.sa_handler &&
                SIG_IGN != sv->mAction.sa_handler)
            {
                ++sProcessSignalContext;

                enum ErrorFrameStackKind stackKind =
                    switchErrorFrameStack(ErrorFrameStackSignal);

                struct ErrorFrameSequence frameSequence =
                    pushErrorFrameSequence();

                sv->mAction.sa_sigaction(aSigNum, aSigInfo, aSigContext);

                popErrorFrameSequence(frameSequence);

                switchErrorFrameStack(stackKind);

                --sProcessSignalContext;
            }
        }
        unlockMutex(sv->mMutex);
    });
}

static void
dispatchSigHandler_(int aSigNum)
{
    FINALLY
    ({
        struct ProcessSignalVector *sv = &sSignalVectors[aSigNum];

        struct ProcessSignalName sigName;
        debug(1,
              "dispatch signal %s",
              formatProcessSignalName(&sigName, aSigNum));

        lockMutex(sv->mMutex);
        {
            if (SIGABRT == aSigNum)
                dispatchSigAbort_();

            if (SIG_DFL != sv->mAction.sa_handler &&
                SIG_IGN != sv->mAction.sa_handler)
            {
                ++sProcessSignalContext;

                enum ErrorFrameStackKind stackKind =
                    switchErrorFrameStack(ErrorFrameStackSignal);

                struct ErrorFrameSequence frameSequence =
                    pushErrorFrameSequence();

                sv->mAction.sa_handler(aSigNum);

                popErrorFrameSequence(frameSequence);

                switchErrorFrameStack(stackKind);

                --sProcessSignalContext;
            }
        }
        unlockMutex(sv->mMutex);
    });
}

static int
changeSigAction_(unsigned          aSigNum,
                 struct sigaction  aNewAction,
                 struct sigaction *aOldAction)
{
    int rc = -1;

    ensure(NUMBEROF(sSignalVectors) > aSigNum);

    struct ThreadSigMask  threadSigMask_;
    struct ThreadSigMask *threadSigMask = 0;

    pthread_mutex_t *lock = 0;

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

    lockThreadSigMutex(&sProcessSigMutex);
    if ( ! sSignalVectors[aSigNum].mMutex)
        sSignalVectors[aSigNum].mMutex =
            createMutex(&sSignalVectors[aSigNum].mMutex_);
    unlockThreadSigMutex(&sProcessSigMutex);

    int sigList[] = { aSigNum, 0 };

    pushThreadSigMask(&threadSigMask_, ThreadSigMaskBlock, sigList);

    threadSigMask = &threadSigMask_;

    lock = lockMutex(sSignalVectors[aSigNum].mMutex);

    struct sigaction prevAction;
    ERROR_IF(
        sigaction(aSigNum, &nextAction, &prevAction));

    /* Do not overwrite the output result unless the underlying
     * sigaction() call succeeds. */

    if (aOldAction)
        *aOldAction = prevAction;

    sSignalVectors[aSigNum].mAction = aNewAction;

    rc = 0;

Finally:

    FINALLY
    ({
        lock = unlockMutex(lock);

        popThreadSigMask(threadSigMask);
    });

    return rc;
}

unsigned
ownProcessSignalContext(void)
{
    return sProcessSignalContext;
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

    ERROR_IF(
        changeSigAction_(
            SIGPIPE,
            (struct sigaction) { .sa_handler = SIG_IGN },
            &sSigPipeAction));

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
        ERROR_IF(
            changeSigAction_(SIGPIPE, sSigPipeAction, 0));

        sSigPipeAction.sa_handler = SIG_ERR;
        sSigPipeAction.sa_flags   = 0;
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

    __sync_add_and_fetch(&sSigCont.mCount, 2);

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

    ERROR_IF(
        updateProcessSigContMethod_(VoidMethod(0, 0)));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
watchProcessSigCont(struct VoidMethod aMethod)
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

    return 1 | sSigCont.mCount;
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

        ABORT_IF(
            raise(SIGSTOP),
            {
                terminate(errno, "Unable to stop process");
            });
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

    ERROR_IF(
        updateProcessSigStopMethod_(VoidMethod(0, 0)));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

int
watchProcessSigStop(struct VoidMethod aMethod)
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

    ERROR_IF(
        changeSigAction_(
            SIGCHLD,
            (struct sigaction) { .sa_handler = SIG_DFL },
            0));

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

    struct VoidMethod sigChldMethod = sSigChldMethod;

    sSigChldMethod = aMethod;

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
            sSigChldMethod = sigChldMethod;
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

        ERROR_IF(
            setitimer(ITIMER_REAL, &disableClock, 0));

        ERROR_IF(
            changeSigAction_(SIGALRM, sClockTickSigAction, 0));

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

    struct VoidMethod clockMethod = sClockMethod;

    sClockMethod = aMethod;

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

    clockTimer.it_value    = timeValFromNanoSeconds(sClockTickPeriod.duration);
    clockTimer.it_interval = clockTimer.it_value;

    ERROR_IF(
        setitimer(ITIMER_REAL, &clockTimer, 0));

    sClockTickSigAction = *prevAction;
    sClockTickPeriod    = aClockPeriod;

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

            sClockMethod = clockMethod;
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
            for (unsigned ix = 0; NUMBEROF(sWatchedSignals) > ix; ++ix)
            {
                struct SignalWatch *watchedSig = sWatchedSignals + ix;

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

    if (0 <= aSigNum && NUMBEROF(sSignalNames) > aSigNum)
        self->mSignalName = sSignalNames[aSigNum];

    if ( ! self->mSignalName)
    {
        sprintf(self->mSignalText_, PROCESS_SIGNALNAME_FMT_, aSigNum);
        self->mSignalName = self->mSignalText_;
    }

    return self->mSignalName;
}

/* -------------------------------------------------------------------------- */
enum ProcessState
fetchProcessState(struct Pid aPid)
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

    ERROR_IF(
        (statfd = open(processStatFileName, O_RDONLY),
         -1 == statfd));

    ssize_t statlen;
    ERROR_IF(
        (statlen = readFdFully(statfd, &statbuf, 0),
         -1 == statlen));

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
                    ERROR_IF(
                        true,
                        {
                            errno = ENOSYS;
                        });

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
        closeFd(&statfd);

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

    static const char pathFmt[] = "/proc/%" PRId_Pid "/.";

    char path[sizeof(pathFmt) + sizeof(pid_t) * CHAR_BIT];

    ERROR_IF(
        0 > sprintf(path, pathFmt, FMTd_Pid(ownProcessId())));

    ERROR_UNLESS(
        (self->mFileName = strdup(path)));

    ERROR_IF(
        createFile(
            &self->mFile_,
            open(self->mFileName, O_RDONLY | O_CLOEXEC)));
    self->mFile = &self->mFile_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            closeFile(self->mFile);

            free(self->mFileName);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
closeProcessLock_(struct ProcessLock *self)
{
    if (self)
    {
        free(self->mFileName);

        closeFile(self->mFile);
    }
}

/* -------------------------------------------------------------------------- */
static int
lockProcessLock_(struct ProcessLock *self)
{
    int rc = -1;

    if (self)
    {
        ensure(LOCK_UN == self->mLock);

        ERROR_IF(
            lockFile(self->mFile, LOCK_EX));

        self->mLock = LOCK_EX;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
unlockProcessLock_(struct ProcessLock *self)
{
    if (self)
    {
        ensure(LOCK_UN != self->mLock);

        ABORT_IF(unlockFile(self->mFile));

        self->mLock = LOCK_UN;
    }
}

/* -------------------------------------------------------------------------- */
static void *
signalThread_(void *self_)
{
    struct ProcessSignalThread *self = self_;

    /* This is a spare thread which will always be available as a context
     * in which signals can be delivered in the case that all other
     * threads are unable accept signals. */

    popThreadSigMask(&sSigMask);

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

        srandom(ownProcessId().mPid);

        ERROR_IF(
            createProcessLock_(&sProcessLock_[sActiveProcessLock]));
        sProcessLock[sActiveProcessLock] = &sProcessLock_[sActiveProcessLock];

        ERROR_IF(
            Error_init());

        pushThreadSigMask(&sSigMask, ThreadSigMaskBlock, 0);

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
void
Process_exit(void)
{
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

        Error_exit();

        closeProcessLock_(processLock);
        sProcessLock[sActiveProcessLock] = 0;
    }
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

        ERROR_IF(
            processLock && lockProcessLock_(processLock));
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

        if (processLock)
            unlockProcessLock_(processLock);
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

    ABORT_IF(
        acquireProcessAppLock(),
        {
            terminate(errno, "Unable to acquire application lock");
        });

    return self;
}

/* -------------------------------------------------------------------------- */
void
destroyProcessAppLock(struct ProcessAppLock *self)
{
    if (self)
    {
        ensure(&sProcessAppLock == self);

        ABORT_IF(
            releaseProcessAppLock(),
            {
                terminate(errno, "Unable to release application lock");
            });
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
reapProcess(struct Pid aPid, int *aStatus)
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
enum ProcessStatus
monitorProcess(struct Pid aPid)
{
    enum ProcessStatus rc = ProcessStatusError;

    siginfo_t siginfo;

    siginfo.si_pid = 0;
    ERROR_IF(
        waitid(P_PID, aPid.mPid, &siginfo,
               WEXITED | WSTOPPED | WCONTINUED | WNOHANG | WNOWAIT));

    if (siginfo.si_pid != aPid.mPid)
        rc = ProcessStatusRunning;
    else
    {
        switch (siginfo.si_code)
        {
        default:
            ERROR_IF(
                true,
                {
                    errno = EINVAL;
                });

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
struct Pid
forkProcess(enum ForkProcessOption aOption, struct Pgid aPgid)
{
    pid_t rc = -1;

    const char            *err  = 0;
    struct ThreadSigMutex *lock = 0;

    unsigned activeProcessLock   = 0 + sActiveProcessLock;
    unsigned inactiveProcessLock = 1 - activeProcessLock;

    ensure(NUMBEROF(sProcessLock_) > activeProcessLock);
    ensure(NUMBEROF(sProcessLock_) > inactiveProcessLock);

    ensure( ! sProcessLock[inactiveProcessLock]);

    pid_t pgid = aPgid.mPgid;

    ensure(ForkProcessSetProcessGroup == aOption || ! pgid);

#ifdef __linux__
    long clocktick;
    ERROR_IF(
        (clocktick = sysconf(_SC_CLK_TCK),
         -1 == clocktick));
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

        ERROR_IF(
            createProcessLock_(&sProcessLock_[inactiveProcessLock]));
        sProcessLock[inactiveProcessLock] = &sProcessLock_[inactiveProcessLock];
    }

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

        /* Switch the process lock first in case the child process
         * needs to emit diagnostic messages so that the messages
         * will not be garbled. */

        sActiveProcessLock  = inactiveProcessLock;
        inactiveProcessLock = activeProcessLock;

        if (ForkProcessSetProcessGroup == aOption)
        {
            ERROR_IF(
                setpgid(0, pgid),
                {
                    err = "Unable to set process group";
                });
        }

        /* Reset all the signals so that the child will not attempt
         * to catch signals. After that, reset the signal mask so
         * that the child will receive signals. */

        ERROR_IF(
            resetSignals_(),
            {
                err = "Unable to reset signal handlers";
            });

        /* This is the only thread running in the new process so
         * it is safe to release the process mutex here. */

        lock = unlockThreadSigMutex(lock);

        break;
    }

    rc = childPid;

Finally:

    FINALLY
    ({
        int errcode = errno;

        closeProcessLock_(sProcessLock[inactiveProcessLock]);
        sProcessLock[inactiveProcessLock] = 0;

        lock = unlockThreadSigMutex(lock);

        ABORT_IF(
            err,
            {
                terminate(errcode, "%s", err);
            });
    });

    return Pid(rc);
}

/* -------------------------------------------------------------------------- */
int
execProcess(const char *aCmd, char **aArgv)
{
    int rc = -1;

    popThreadSigMask(&sSigMask);

    /* Call resetProcessSigPipe_() here to ensure that SIGPIPE will be
     * delivered to the new program. Note that it is possible that there
     * was no previous call to forkProcess(), though this is normally
     * the case. */

    ERROR_IF(
        resetProcessSigPipe_());

    execvp(aCmd, aArgv);

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
void
exitProcess(int aStatus)
{
    exit(aStatus);

    while (1)
        raise(SIGKILL);
}

/* -------------------------------------------------------------------------- */
void
quitProcess(int aStatus)
{
    _exit(aStatus);

    while (1)
        raise(SIGKILL);
}

/* -------------------------------------------------------------------------- */
void
abortProcess(void)
{
    /* When running under valgrind, do not abort() because it causes the
     * program to behave as if it received SIGKILL. Instead, exit the
     * program immediately and allow valgrind to survey the program for
     * for leaks. */

    if (RUNNING_ON_VALGRIND)
        _exit(128 + SIGABRT);

    /* Other threads might be attaching or have attached a handler for SIGABRT,
     * and the SIGABRT signal might be blocked.
     *
     * Also, multiple threads might call this function at the same time.
     *
     * Try to raise SIGABRT in this thread, but also mark sProcessAbort
     * which will be noticed if a handler is already attached.
     *
     * Do not call back into any application libraries to avoid the risk
     * of infinite recursion, however be aware that dispatchSigAbort_()
     * might end up calling into this function recursively.
     *
     * Do not call abort(3) because that will try to flush IO streams
     * and perform other activity that might fail. */

    __sync_or_and_fetch(&sProcessAbort, 1);

    do
    {
        sigset_t sigSet;

        if (pthread_sigmask(SIG_SETMASK, 0, &sigSet))
            break;

        if (1 == sigismember(&sigSet, SIGABRT))
        {
            if (sigdelset(&sigSet, SIGABRT))
                break;

            if (pthread_sigmask(SIG_SETMASK, &sigSet, 0))
                break;
        }

        for (unsigned ix = 0; 10 > ix; ++ix)
        {
            struct sigaction sigAction = { .sa_handler = SIG_DFL };

            if (sigaction(SIGABRT, &sigAction, 0))
                break;

            /* There is a window here for another thread to configure SIGABRT
             * to be ignored, or handled. So when SIGABRT is raised, it might
             * not actually cause the process to abort. */

            if ( ! testAction(TestLevelRace))
            {
                if (raise(SIGABRT))
                    break;
            }

            sigset_t pendingSet;

            if (sigpending(&pendingSet))
                break;

            int pendingSignal = sigismember(&pendingSet, SIGABRT);
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

    /* There was an error trying to deliver SIGABRT to the process, so
     * try one last time, then resort to killing the process. */

    if ( ! testAction(TestLevelRace))
        raise(SIGABRT);

    while (1)
    {
        struct timespec sleepTime = { .tv_sec = 1 };

        nanosleep(&sleepTime, 0);

        if ( ! testAction(TestLevelRace))
            raise(SIGKILL);
    }
}

/* -------------------------------------------------------------------------- */
const char *
ownProcessName(void)
{
    extern const char *__progname;

    return sProgramName ? sProgramName : __progname;
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
        debug(
            0,
            "process pid %" PRId_Pid " exited %d",
            FMTd_Pid(aPid),
            WEXITSTATUS(aStatus));

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

        static const char sProcessStatFileNameFmt[] = "%s/stat";

        char processStatFileName[strlen(processDirName.mDirName) +
                                 sizeof(sProcessStatFileNameFmt)];

        sprintf(processStatFileName,
                sProcessStatFileNameFmt, processDirName.mDirName);

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
        closeFd(&fd);

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
