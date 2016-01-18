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

#include "libk9.h"
#include "macros_.h"
#include "parse_.h"
#include "error_.h"
#include "fd_.h"
#include "pollfd_.h"
#include "env_.h"
#include "thread_.h"
#include "unixsocket_.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#include <asm/ldt.h>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/un.h>

#include <linux/futex.h>

/* -------------------------------------------------------------------------- */
enum FdKind
{
    FD_UMBILICAL,
    FD_KINDS
};

static const char *sPollFdNames[FD_KINDS] =
{
    [FD_UMBILICAL] = "umbilical",
};

/* -------------------------------------------------------------------------- */
enum FdTimerKind
{
    FD_TIMER_UMBILICAL,
    FD_TIMER_KINDS
};

static const char *sPollFdTimerNames[FD_TIMER_KINDS] =
{
    [FD_TIMER_UMBILICAL] = "umbilical",
};

/* -------------------------------------------------------------------------- */
enum EnvKind
{
    ENV_LD_PRELOAD,
    ENV_K9_SO,
    ENV_K9_ADDR,
    ENV_K9_PID,
    ENV_K9_PPID,
    ENV_K9_TIME,
    ENV_K9_TIMEOUT,
    ENV_KINDS
};

struct Env
{
    const char *mName;
    const char *mValue;
};

/* -------------------------------------------------------------------------- */
static int
rankEnv(const void *aLhs, const void *aRhs)
{
    const struct Env * const *lhs = aLhs;
    const struct Env * const *rhs = aRhs;

    return strcmp((*lhs)->mName, (*rhs)->mName);
}

struct EnvEntry
{
    const char *mEnv;
    size_t      mNameLen;
    const char *mValue;
};

static int
rankEnvEntry(const void *aLhs, const void *aRhs)
{
    const struct EnvEntry *lhs = aLhs;
    const struct EnvEntry *rhs = aRhs;

    size_t namelen =
        lhs->mNameLen < rhs->mNameLen ? lhs->mNameLen : rhs->mNameLen;

    int rank = strncmp(lhs->mEnv, rhs->mEnv, namelen);

    if ( ! rank)
    {
        if (lhs->mNameLen < rhs->mNameLen)
            rank = -1;
        else if (lhs->mNameLen > rhs->mNameLen)
            rank = +1;
    }

    return rank;
}

static int
rankEnvName(const struct Env *aLhs, const struct EnvEntry *aRhs)
{
    int rank = strncmp(aLhs->mName, aRhs->mEnv, aRhs->mNameLen);

    return ! rank && aLhs->mName[aRhs->mNameLen] ? 1 : rank;
}

static void
initEnv(struct Env *aEnv, size_t aEnvLen, char **envp)
{
    /* Create two sorted lists of environment variables: the list of
     * requested variables, and the list of available variables. Once
     * formed perform a single pass through both lists to locate
     * the requested variables that are available. */

    struct Env *sortedEnv[aEnvLen];

    for (size_t ex = 0; ex < aEnvLen; ++ex)
        sortedEnv[ex] = &aEnv[ex];

    qsort(sortedEnv, aEnvLen, sizeof(sortedEnv[0]), rankEnv);

    size_t environLen = 0;
    while (envp[environLen])
        ++environLen;

    struct EnvEntry sortedEnviron[environLen];

    for (size_t ex = 0; envp[ex]; ++ex)
    {
        char *eqPtr = strchr(envp[ex], '=');

        if ( ! eqPtr)
            eqPtr = strchr(envp[ex], 0);

        sortedEnviron[ex].mEnv     = envp[ex];
        sortedEnviron[ex].mNameLen = eqPtr - envp[ex];
        sortedEnviron[ex].mValue   = *eqPtr ? eqPtr+1 : eqPtr;
    }

    qsort(sortedEnviron, environLen, sizeof(sortedEnviron[0]), rankEnvEntry);

    size_t envix     = 0;
    size_t environix = 0;

    while (envix < NUMBEROF(sortedEnv) && environix < NUMBEROF(sortedEnviron))
    {
        int rank = rankEnvName(sortedEnv[envix],
                               &sortedEnviron[environix]);

        if (rank < 0)
            ++envix;
        else if (rank > 0)
            ++environix;
        else
        {
            sortedEnv[envix]->mValue = sortedEnviron[environix].mValue;

            debug(1,
                  "Env - %s '%s'",
                  sortedEnv[envix]->mName,
                  sortedEnv[envix]->mValue);

            ++envix;
            ++environix;
        }
    }
}

/* -------------------------------------------------------------------------- */
static void
purgeEnv(void)
{
    static const char envPrefix[] = "K9_";

    /* Count the number of matching environment variables, then record
     * a pointer to each of the matching variables because the array
     * will likely mutate as each is purged. Once recorded, purge each
     * of the matching variables. */

    size_t envLen = 0;

    for (unsigned ix = 0; environ[ix]; ++ix)
    {
        if ( ! strncmp(envPrefix, environ[ix], sizeof(envPrefix)-1))
            ++envLen;
    }

    const char *env[envLen];

    for (size_t ix = 0, ex = 0; environ[ix]; ++ix)
    {
        if ( ! strncmp(envPrefix, environ[ix], sizeof(envPrefix)-1))
            env[ex++] = environ[ix];
    }

    for (size_t ex = 0; ex < envLen; ++ex)
    {
        const char *eqPtr = strchr(env[ex], '=');

        if ( ! eqPtr)
            continue;

        size_t nameLen = 1 + eqPtr - env[ex];

        char name[nameLen];
        memcpy(name, env[ex], nameLen);
        name[nameLen-1] = 0;

        if (deleteEnv(name))
            terminate(
                errno, "Unable to remove environment variable '%s'", name);
    }
}

/* -------------------------------------------------------------------------- */
static void
stripEnvPreload(struct Env *aPreloadEnv, const char *aLibrary)
{
    while (aLibrary)
    {
        const char *sopath = aLibrary;

        while (*sopath && *sopath == ' ')
            ++sopath;

        if ( ! *sopath)
            break;

        size_t sopathlen = strlen(sopath);

        if ( ! aPreloadEnv->mValue)
            break;

        do
        {
            char sopreloadtext[1 + strlen(aPreloadEnv->mValue)];

            char *sopreload = sopreloadtext;

            strcpy(sopreload, aPreloadEnv->mValue);

            while (*sopreload && (*sopreload == ' ' || *sopreload == ':'))
                ++sopreload;

            if ( ! *sopreload)
                break;

            if ( ! strncmp(sopreload, sopath, sopathlen) &&
                 ( ! sopreload[sopathlen] ||
                   sopreload[sopathlen] == ' ' ||
                   sopreload[sopathlen] == ':'))
            {
                size_t tail = sopathlen;

                while (sopreload[tail] &&
                       (sopreload[tail] == ' ' || sopreload[tail] == ':'))
                    ++tail;

                for (unsigned ix = 0; ; ++ix)
                {
                    sopreload[ix] = sopreload[tail + ix];
                    if ( ! sopreload[ix])
                        break;
                }

                const char *strippedPreload =
                    setEnvString(aPreloadEnv->mName, sopreloadtext);

                if ( ! strippedPreload)
                    terminate(
                        errno,
                        "Unable to prune %s '%s'",
                        aPreloadEnv->mName,
                        aPreloadEnv->mValue);

                aPreloadEnv->mValue = strippedPreload;
            }

        } while (0);

        break;
    }
}

/* -------------------------------------------------------------------------- */
enum UmbilicalThreadState
{
    UMBILICAL_STOPPED,
    UMBILICAL_STARTING,
    UMBILICAL_STARTED,
    UMBILICAL_STOPPING,
};

struct UmbilicalThread
{
    pthread_t                  mThread;
    pthread_mutex_t            mMutex;
    pthread_cond_t             mCond;
    enum UmbilicalThreadState  mState;
    intmax_t                   mStack_[PTHREAD_STACK_MIN / sizeof(intmax_t)];
    intmax_t                  *mStack;
    struct UnixSocket          mSock;
    struct Duration            mTimeout;
    pid_t                      mWatchdogPid;
    int                        mStatus;
    int                       *mErrno;
};

struct UmbilicalPoll
{
    enum FdKind mKind;

    struct UmbilicalThread *mThread;

    struct File              *mFile;
    struct pollfd            *mPollFd;
    struct PollFdTimerAction *mTimer;
    unsigned                  mCycleCount;
    unsigned                  mCycleLimit;
};

/* -------------------------------------------------------------------------- */
static void
pollumbilical(void                        *self_,
              struct pollfd               *aPollFds,
              const struct EventClockTime *aPollTime)
{
    struct UmbilicalPoll *self = self_;

    ensure(FD_UMBILICAL == self->mKind);

    char buf[1];

    ssize_t rdlen = read(self->mFile->mFd, buf, sizeof(buf));

    if (-1 == rdlen)
    {
        if (EINTR != errno)
            terminate(
                errno,
                "Unable to read umbilical connection");
    }
    else if ( ! rdlen)
    {
        warn(0, "Broken umbilical connection");
        self->mPollFd->events = 0;
    }
    else
    {
        lapTimeRestart(&self->mTimer->mSince, aPollTime);
        self->mCycleCount = 0;
    }
}

/* -------------------------------------------------------------------------- */
static void
polltimerumbilical(void                        *self_,
                   struct PollFdTimerAction    *aPollFdTimer,
                   const struct EventClockTime *aPollTime)
{
    struct UmbilicalPoll *self = self_;

    ensure(FD_UMBILICAL == self->mKind);

    /* If nothing is available from the umbilical socket after
     * the timeout period expires, then assume that the watchdog
     * itself is stuck. */

    if (ProcessStateStopped == findProcessState(self->mThread->mWatchdogPid))
    {
        debug(0, "umbilical timeout deferred");

        self->mCycleCount = 0;
    }
    else if (++self->mCycleCount >= self->mCycleLimit)
    {
        warn(0, "Umbilical connection timed out");

        self->mPollFd->events             = 0;
        self->mTimer->mPeriod.duration.ns = 0;
    }
}

/* -------------------------------------------------------------------------- */
static bool
pollumbilicalcompletion(void                     *self_,
                        struct pollfd            *aPollFds,
                        struct PollFdTimerAction *aPollFdTimer)
{
    struct UmbilicalPoll *self = self_;

    ensure(FD_UMBILICAL == self->mKind);

    return ! self->mPollFd->events;
}

/* -------------------------------------------------------------------------- */
static int
watchUmbilical_(void *self_)
{
    /* The umbilicalThread structure is owned by the parent thread, so
     * ensure that the child makes no attempt to modify the structure
     * here. */

    struct UmbilicalThread *self = self_;

    if (self->mErrno != &errno)
        terminate(
            0,
            "Umbilical thread context mismatched %p vs %p",
            (void *) self->mErrno,
            (void *) &errno);

    /* Capture the umbilical file descriptor here because although this
     * thread shares the same memory space as the enclosing process,
     * it has a separate file descriptor space. */

    struct File umbilicalFile;
    if (dupFile(&umbilicalFile, self->mSock.mFile))
        terminate(
            errno,
            "Unable to dup umbilical thread file descriptor %d",
            self->mSock.mFile->mFd);

    lockMutex(&self->mMutex);
    {
        while (UMBILICAL_STARTING != self->mState)
            waitCond(&self->mCond, &self->mMutex);

        if ( ! ownFdValid(self->mSock.mFile->mFd))
            terminate(
                errno,
                "Umbilical file descriptor is not valid %d",
                self->mSock.mFile->mFd);

        self->mState = UMBILICAL_STARTED;
    }
    unlockMutexSignal(&self->mMutex, &self->mCond);

    /* Since the child has its own file descriptor space, close
     * all unnecessary file descriptors so that the child will not
     * inadvertently corrupt or pollute the file descriptors of the
     * child process.
     *
     * Only leave stderr and the umbilical file descriptor. In particular,
     * both stdin and stdout are closed so that the monitored application
     * can control and redirect these standard file descriptors as
     * it sees fit.
     *
     * Note that this will close the file descriptors in self->mSync
     * so no attempt should be made to them after this point. */

    struct rlimit noFile;
    if (getrlimit(RLIMIT_NOFILE, &noFile))
        terminate(
            errno,
            "Unable to obtain file descriptor limit");

    for (int fd = 0; fd < noFile.rlim_cur; ++fd)
    {
        if (STDERR_FILENO != fd && fd != umbilicalFile.mFd)
            (void) close(fd);
    }

    struct UmbilicalPoll umbilicalpoll =
    {
        .mKind       = FD_UMBILICAL,
        .mFile       = &umbilicalFile,
        .mCycleLimit = 2
    };

    struct pollfd pollfds[FD_KINDS] =
    {
        [FD_UMBILICAL] = { .fd     = umbilicalpoll.mFile->mFd,
                           .events = POLL_INPUTEVENTS },
    };

    struct PollFdAction pollfdactions[FD_KINDS] =
    {
        [FD_UMBILICAL] = { pollumbilical },
    };

    struct PollFdTimerAction pollfdtimeractions[FD_TIMER_KINDS] =
    {
        [FD_TIMER_UMBILICAL] =
        { polltimerumbilical,
          Duration(
              NanoSeconds(
                  self->mTimeout.duration.ns / umbilicalpoll.mCycleLimit)) },
    };

    umbilicalpoll.mThread = self;
    umbilicalpoll.mTimer  = &pollfdtimeractions[FD_TIMER_UMBILICAL];
    umbilicalpoll.mPollFd = &pollfds[FD_UMBILICAL];

    struct PollFd pollfd;
    if (createPollFd(
            &pollfd,
            pollfds, pollfdactions, sPollFdNames, FD_KINDS,
            pollfdtimeractions, sPollFdTimerNames, FD_TIMER_KINDS,
            pollumbilicalcompletion, &umbilicalpoll))
        terminate(
            errno,
            "Unable to initialise polling loop");

    if (runPollFdLoop(&pollfd))
        terminate(
            errno,
            "Unable to run polling loop");

    if (closePollFd(&pollfd))
        terminate(
            errno,
            "Unable to close polling loop");

    lockMutex(&self->mMutex);
    {
        self->mState = UMBILICAL_STOPPING;
    }
    unlockMutex(&self->mMutex);

    if (closeFile(&umbilicalFile))
        terminate(
            errno,
            "Unable to close umbilical file descriptor %d",
            umbilicalFile.mFd);

    return 0;
}

/* -------------------------------------------------------------------------- */
static void *
umbilicalMain_(void *aUmbilicalThread)
{
    struct UmbilicalThread *umbilicalThread = aUmbilicalThread;

    /* Create the umbilical thread and ensure that it is ready before
     * proceeding. This is important partly because the library
     * code is largely single threaded, and also to ensure that
     * the umbilical thread is functional.
     *
     * CLONE_THREAD semantics are required in order to ensure that
     * the umbilical thread is reaped when the process executes
     * execve() et al. By implication, CLONE_THREAD requires
     * CLONE_SIGHAND, and CLONE_SIGHAND in turn requires
     * CLONE_VM.
     *
     * CLONE_FILE is not used so that the umbilical file descriptor
     * can be used exclusively by the umbilical thread. Apart from
     * the umbilical thread, the rest of the child process cannot
     * manipulate or close the umbilical file descriptor, allowing
     * it to close all file descriptors without disrupting the
     * operation of the umbilical thread. */

#ifdef __i386__
    struct user_desc  tls_;
    struct user_desc *tls = &tls_;

    {
        unsigned gs;
        __asm ("movw %%gs, %w0" : "=q" (gs));

        gs &= 0xffff;
        tls->entry_number = gs >> 3;

        if (syscall(SYS_get_thread_area, tls))
            terminate(
                errno,
                "Unable to find thread area 0x%x", gs);
    }
#endif

    /* Use an umbilical slave thread so that it can operate with
     * an isolated set of file descriptors. It is expected that
     * watched processes (especially servers) will close all file
     * descriptors in which they have no active interest, and thus
     * would close the umbilical file descriptor if the umbilical
     * thread shared the same file descriptor space. */

    umbilicalThread->mErrno = &errno;

    pid_t slavetid;
    pid_t slavepid = clone(
        watchUmbilical_,
        umbilicalThread->mStack,
        CLONE_VM | CLONE_SIGHAND | CLONE_THREAD | CLONE_FS |
        CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID,
        umbilicalThread, &slavetid, tls, &slavetid);

    if (-1 == slavepid)
        terminate(
            errno,
            "Unable to create umbilical slave thread");

    while (slavetid)
    {
        switch (syscall(SYS_futex, &slavetid, FUTEX_WAIT, slavepid, 0, 0, 0))
        {
        case 0:
            continue;

        default:
            if (EINTR == errno || EWOULDBLOCK == errno)
                continue;
            terminate(
                errno,
                "Unable to wait for umbilical slave thread");
        }
    }

    lockMutex(&umbilicalThread->mMutex);
    {
        ensure(UMBILICAL_STOPPING == umbilicalThread->mState);
    }
    unlockMutex(&umbilicalThread->mMutex);

    debug(0, "umbilical thread %jd terminated", (intmax_t) slavepid);

    /* Do not exit until the umbilical slave thread has completed because
     * it shares the same pthread resources. Once the umbilical slave
     * thread completes, it is safe to release the pthread resources.
     *
     * With the umbilical broken, kill the process group that contains
     * the process being monitored. Try politely, then more aggressively.
     */

    if (kill(0, SIGTERM))
        terminate(
            errno,
            "Unable to send SIGTERM to process group");

    monotonicSleep(Duration(NSECS(Seconds(30))));

    if (kill(0, SIGKILL))
        terminate(
            errno,
            "Unable to send SIGKILL to process group");

    _exit(1);

    return umbilicalThread;
}

/* -------------------------------------------------------------------------- */
static void
watchUmbilical(const char            *aAddr,
               pid_t                  aWatchdogPid,
               const struct Duration *aTimeout)
{
    static struct UmbilicalThread umbilicalThread;

    while (aAddr)
    {
        debug(0, "umbilical thread initialising");

        size_t addrLen = strlen(aAddr);

        struct sockaddr_un umbilicalAddr;

        if (sizeof(umbilicalAddr.sun_path) <= addrLen)
            terminate(
                0,
                "Umbilical socket address too long '%s'", aAddr);

        memcpy(&umbilicalAddr.sun_path[1], aAddr, addrLen);
        umbilicalAddr.sun_path[0] = 0;

        /* Use a detached thread to monitor the umbilical from the
         * watchdog. Using a thread allows the main thread of control
         * to continue performing the main activities of the process. */

        static const pthread_mutex_t mutexInit = PTHREAD_MUTEX_INITIALIZER;
        static const pthread_cond_t  condInit  = PTHREAD_COND_INITIALIZER;

        umbilicalThread.mTimeout     = *aTimeout;
        umbilicalThread.mWatchdogPid = aWatchdogPid;
        umbilicalThread.mMutex       = mutexInit;
        umbilicalThread.mCond        = condInit;
        umbilicalThread.mState       = UMBILICAL_STOPPED;
        umbilicalThread.mErrno       = 0;

        if (connectUnixSocket(
                &umbilicalThread.mSock, umbilicalAddr.sun_path, addrLen+1))
            terminate(
                errno,
                "Failed to connect umbilical socket to '%.*s'",
                sizeof(umbilicalAddr.sun_path) - 1,
                &umbilicalAddr.sun_path[1]);

        /* The stack is allocated statically to avoid creating another
         * anonymous mmap(2) region unnecessarily. */

        uintptr_t frameChild  = (uintptr_t) __builtin_frame_address(0);
        uintptr_t frameParent = (uintptr_t) __builtin_frame_address(1);

        if (frameChild == frameParent)
            terminate(
                0,
                "Unable to ascertain direction of stack growth");

        umbilicalThread.mStack = umbilicalThread.mStack_;

        if (frameChild < frameParent)
            umbilicalThread.mStack += NUMBEROF(umbilicalThread.mStack_);

        {
            /* When creatng the umbilical thread, ensure that it
             * is not a target of any signals intended for the process
             * being monitored. */

            struct PushedProcessSigMask pushedSigMask;

            if (pushProcessSigMask(&pushedSigMask, ProcessSigMaskBlock, 0))
                terminate(
                    errno,
                    "Unable to push process signal mask");

            pthread_attr_t umbilicalThreadAttr;

            createThreadAttr(&umbilicalThreadAttr);
            setThreadAttrDetachState(
                &umbilicalThreadAttr, PTHREAD_CREATE_DETACHED);

            createThread(&umbilicalThread.mThread,
                         &umbilicalThreadAttr,
                         umbilicalMain_, &umbilicalThread);

            destroyThreadAttr(&umbilicalThreadAttr);

            if (popProcessSigMask(&pushedSigMask))
                terminate(
                    errno,
                    "Unable to pop process signal mask");
        }

        debug(0, "umbilical thread starting");

        lockMutex(&umbilicalThread.mMutex);
        {
            umbilicalThread.mState = UMBILICAL_STARTING;
        }
        unlockMutexSignal(&umbilicalThread.mMutex, &umbilicalThread.mCond);

        lockMutex(&umbilicalThread.mMutex);
        {
            while (UMBILICAL_STARTING == umbilicalThread.mState)
                waitCond(&umbilicalThread.mCond, &umbilicalThread.mMutex);
        }
        unlockMutex(&umbilicalThread.mMutex);

        if (closeUnixSocket(&umbilicalThread.mSock))
            terminate(
                errno,
                "Unable to close umbilical socket");

        debug(0, "umbilical thread started");

        break;
    }
}

/* -------------------------------------------------------------------------- */
static void  __attribute__((constructor))
libk9_init(void)
{
    if (Timekeeping_init())
        terminate(
            0,
            "Unable to initialise timekeeping module");

    if (Error_init())
        terminate(
            0,
            "Unable to initialise error module");

    initOptions();

    /* Now that the environment variables are available, find
     * the environment variables that pertain to the watchdog. */

    struct Env env[ENV_KINDS] =
    {
        [ENV_LD_PRELOAD] = { "LD_PRELOAD" },
        [ENV_K9_SO]      = { "K9_SO" },
        [ENV_K9_ADDR]    = { "K9_ADDR" },
        [ENV_K9_PID]     = { "K9_PID" },
        [ENV_K9_PPID]    = { "K9_PPID" },
        [ENV_K9_TIME]    = { "K9_TIME" },
        [ENV_K9_TIMEOUT] = { "K9_TIMEOUT" },
    };

    initEnv(env, NUMBEROF(env), environ);

    if (env[ENV_K9_PID].mValue)
    {
        pid_t pid;
        if (parsePid(env[ENV_K9_PID].mValue, &pid))
            terminate(
                errno,
                "Unable to parse pid '%s'",
                env[ENV_K9_PID].mValue);

        if (pid == getpid())
        {
            /* This is the child process to be monitored. The
             * child process might exec() another program, so
             * leave the environment variables in place to
             * monitor the new program . */

            if ( ! env[ENV_K9_TIMEOUT].mValue)
                terminate(
                    0,
                    "No umbilical timeout specified");

            unsigned umbilicalTimeout_s;
            if (parseUInt(env[ENV_K9_TIMEOUT].mValue, &umbilicalTimeout_s))
                terminate(
                    errno,
                    "Unable to parse umbilical timeout: %s",
                    env[ENV_K9_TIMEOUT].mValue);

            struct Duration umbilicalTimeout = Duration(
                NSECS(Seconds(umbilicalTimeout_s)));

            if ( ! env[ENV_K9_PPID].mValue)
                terminate(
                    0,
                    "No watchdog pid specified");

            pid_t watchdogPid;
            if (parsePid(env[ENV_K9_PPID].mValue, &watchdogPid))
                terminate(
                    errno,
                    "Unable to parse watchdog pid '%s'",
                    env[ENV_K9_PPID].mValue);

            watchUmbilical(env[ENV_K9_ADDR].mValue,
                           watchdogPid,
                           &umbilicalTimeout);
        }
        else
        {
            /* This is a grandchild process. Any descendant
             * processes or programs do not need the parasite
             * library. */

            purgeEnv();
            stripEnvPreload(&env[ENV_LD_PRELOAD], env[ENV_K9_SO].mValue);
        }
    }
}

/* -------------------------------------------------------------------------- */
static void __attribute__((destructor))
libk9_exit()
{
    if (Error_exit())
        terminate(
            0,
            "Unable to finalise error module");

    if (Timekeeping_exit())
        terminate(
            0,
            "Unable to finalise timekeeping module");
}

/* -------------------------------------------------------------------------- */
