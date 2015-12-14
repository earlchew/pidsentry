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

#include "libk9.h"
#include "macros_.h"
#include "parse_.h"
#include "error_.h"
#include "fd_.h"
#include "thread_.h"
#include "process_.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <inttypes.h>
#include <pthread.h>

#include <asm/ldt.h>

#include <sys/resource.h>
#include <sys/syscall.h>

#include <linux/futex.h>

extern char **_dl_argv;

/* -------------------------------------------------------------------------- */
enum EnvKind
{
    ENV_LD_PRELOAD,
    ENV_K9_SO,
    ENV_K9_FD,
    ENV_K9_PID,
    ENV_K9_TIME,
    ENV_K9_DEBUG,
    ENV_KINDS
};

struct Env
{
    const char *mName;
    char       *mEnv;
};

/* -------------------------------------------------------------------------- */
int
k9so()
{
    return 0;
}

/* -------------------------------------------------------------------------- */
static void
initArgv(char ***argv, int *argc, char ***envp)
{
    /* Find the environment variables for this process. Unfortunately
     * __environ is not available, but _dl_argv is accessible. */

    *argv = _dl_argv;

    for (*argc = 0; ; ++(*argc))
    {
        if ( ! (*argv)[*argc])
        {
            *envp = *argv + *argc + 1;
            break;
        }
    }
}

/* -------------------------------------------------------------------------- */
static void
initEnv(struct Env *aEnv, size_t aEnvLen, char **envp)
{
    for (unsigned ex = 0; envp[ex]; ++ex)
    {
        char *eqptr = strchr(envp[ex], '=');

        if ( ! eqptr)
            continue;

        const char *name    = envp[ex];
        size_t      namelen = eqptr - envp[ex];

        for (unsigned ix = 0; aEnvLen > ix; ++ix)
        {
            if ( ! strncmp(aEnv[ix].mName, name, namelen) &&
                 ! aEnv[ix].mName[namelen])
            {
                aEnv[ix].mEnv = eqptr + 1;
                break;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
static void
stripEnvPreload(struct Env *aPreload, const char *aLibrary)
{
    while (aLibrary)
    {
        const char *sopath = aLibrary;

        while (*sopath && *sopath == ' ')
            ++sopath;

        if ( ! *sopath)
            break;

        size_t sopathlen = strlen(sopath);

        char *preloadname = aPreload->mEnv;

        if ( ! preloadname)
            break;

        do
        {
            while (*preloadname && (*preloadname == ' ' || *preloadname == ':'))
                ++preloadname;

            if ( ! *preloadname)
                break;

            if ( ! strncmp(preloadname, sopath, sopathlen) &&
                 ( ! preloadname[sopathlen] ||
                   preloadname[sopathlen] == ' ' ||
                   preloadname[sopathlen] == ':'))
            {
                size_t tail = sopathlen;

                while (preloadname[tail] &&
                       (preloadname[tail] == ' ' || preloadname[tail] == ':'))
                    ++tail;

                for (unsigned ix = 0; ; ++ix)
                {
                    preloadname[ix] = preloadname[tail + ix];
                    if ( ! preloadname[ix])
                        break;
                }
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
};

struct UmbilicalThread
{
    pthread_t                  mThread;
    pthread_mutex_t            mMutex;
    pthread_cond_t             mCond;
    enum UmbilicalThreadState  mState;
    intmax_t                   mStack_[PTHREAD_STACK_MIN / sizeof(intmax_t)];
    intmax_t                  *mStack;
    int                        mFd;
    int                        mStatus;
};

/* -------------------------------------------------------------------------- */
static int
watchUmbilical_(void *aUmbilicalThread)
{
    /* The umbilicalThread structure is owned by the parent thread, so
     * ensure that the child makes no attempt to modify the structure
     * here. */

    struct UmbilicalThread *umbilicalThread = aUmbilicalThread;

    lockMutex(&umbilicalThread->mMutex);
    {
        while (UMBILICAL_STARTING != umbilicalThread->mState)
            waitCond(&umbilicalThread->mCond, &umbilicalThread->mMutex);

        if ( ! ownFdValid(umbilicalThread->mFd))
            terminate(
                errno,
                "Umbilical file descriptor is not valid %d",
                umbilicalThread->mFd);

        umbilicalThread->mState = UMBILICAL_STARTED;
    }
    unlockMutexSignal(&umbilicalThread->mMutex, &umbilicalThread->mCond);

    /* Since the child has its own file descriptor space, close
     * all unnecessary file descriptors so that the child will not
     * inadvertently corrupt or pollute the file descriptors of the
     * child process.
     *
     * Note that this will close the file descriptors in umbilicalThread->mSync
     * so no attempt should be made to them after this point. */

    struct rlimit noFile;
    if (getrlimit(RLIMIT_NOFILE, &noFile))
        terminate(
            errno,
            "Unable to obtain file descriptor limit");

    for (int fd = 0; fd < noFile.rlim_cur; ++fd)
    {
        if (  ! stdFd(fd) && fd != umbilicalThread->mFd)
            (void) close(fd);
    }

    warn(0, "*** RUNNING CHILD ***");

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

    pid_t tid;
    pid_t pid = clone(
            watchUmbilical_,
            umbilicalThread->mStack,
            CLONE_VM | CLONE_SIGHAND | CLONE_THREAD | CLONE_FS |
            CLONE_SETTLS | CLONE_CHILD_SETTID,
            umbilicalThread, 0, tls, &tid);

    if (-1 == pid)
        terminate(
            errno,
            "Unable to create umbilical thread");

    while (tid)
    {
        switch (syscall(SYS_futex, &tid, FUTEX_WAIT, pid, 0, 0, 0))
        {
        case 0:
            continue;

        default:
            if (EINTR == errno || EWOULDBLOCK == errno)
                continue;
            terminate(
                errno,
                "Unable to wait for umbilical thread");
        }
    }

    /* Do not exit until the umbilical slave thread has completed because
     * it shares the same pthread resources. Once the umbilical slave
     * thread completes, it is safe to release the pthread resources. */

    debug(0, "**** Umbilical thread %jd terminated", (intmax_t) pid);

    return umbilicalThread;
}

/* -------------------------------------------------------------------------- */
static void
watchUmbilical(const char *aFd)
{
    static struct UmbilicalThread umbilicalThread;

    while (aFd)
    {
        warn(0, "********* START");

        int fd;
        if (parseInt(aFd, &fd) || 0 > fd)
            terminate(
                errno,
                "Unable to parse umbilical file descriptor %s", aFd);

        if (stdFd(fd) || 0 > fd)
            terminate(
                0,
                "Unexpected value for umbilical file descriptor %d", fd);

        if ( ! ownFdValid(fd))
            terminate(
                errno,
                "Umbilical file descriptor %d is not valid", fd);

        /* Use a detached thread to monitor the umbilical from the
         * watchdog. Using a thread allows the main thread of control
         * to continue performing the main activities of the process. */

        static const pthread_mutex_t mutexInit = PTHREAD_MUTEX_INITIALIZER;
        static const pthread_cond_t  condInit  = PTHREAD_COND_INITIALIZER;

        umbilicalThread.mMutex = mutexInit;
        umbilicalThread.mCond  = condInit;
        umbilicalThread.mState = UMBILICAL_STOPPED;
        umbilicalThread.mFd    = fd;

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
            pthread_attr_t umbilicalThreadAttr;

            createThreadAttr(&umbilicalThreadAttr);
            setThreadAttrDetachState(
                &umbilicalThreadAttr, PTHREAD_CREATE_DETACHED);

            createThread(&umbilicalThread.mThread,
                         &umbilicalThreadAttr,
                         umbilicalMain_, &umbilicalThread);

            destroyThreadAttr(&umbilicalThreadAttr);
        }

        warn(0, "********* SYNCRHONISING");

        lockMutex(&umbilicalThread.mMutex);
        {
            umbilicalThread.mState = UMBILICAL_STARTING;
        }
        unlockMutexSignal(&umbilicalThread.mMutex, &umbilicalThread.mCond);

        lockMutex(&umbilicalThread.mMutex);
        {
            while (UMBILICAL_STARTED != umbilicalThread.mState)
                waitCond(&umbilicalThread.mCond, &umbilicalThread.mMutex);
        }
        unlockMutex(&umbilicalThread.mMutex);

        if (closeFd(&fd))
            terminate(
                errno,
                "Unable to close umbilical file descriptor %d", fd);

        warn(0, "********* DONE");

        break;
    }
}

/* -------------------------------------------------------------------------- */
static void  __attribute__((constructor))
libk9_init()
{
    if (Error_init())
        terminate(
            0,
            "Unable to initialise error module");

    initOptions();

    char **argv;
    int    argc;
    char **envp;

    initArgv(&argv, &argc, &envp);

    /* Now that the environment variables are available, find
     * the environment variables that pertain to the watchdog. */

    struct Env env[ENV_KINDS] =
    {
        [ENV_LD_PRELOAD] = { "LD_PRELOAD" },
        [ENV_K9_SO]      = { "K9_SO" },
        [ENV_K9_FD]      = { "K9_FD" },
        [ENV_K9_PID]     = { "K9_PID" },
        [ENV_K9_TIME]    = { "K9_TIME" },
        [ENV_K9_DEBUG]   = { "K9_DEBUG" },
    };

    initEnv(env, NUMBEROF(env), envp);

    if (env[ENV_K9_PID].mEnv)
    {
        pid_t pid;
        if ( ! parsePid(env[ENV_K9_PID].mEnv, &pid))
        {
            if (pid == getpid())
            {
                /* This is the child process to be monitored. The
                 * child process might exec() another program, so
                 * leave the environment variables in place to
                 * monitor the new program . */

                watchUmbilical(env[ENV_K9_FD].mEnv);
            }
            else
            {
                /* This is a grandchild process. Any descendant
                 * processes or programs do not need the parasite
                 * library. */

                env[ENV_K9_FD].mEnv[0] = 0;

                stripEnvPreload(&env[ENV_LD_PRELOAD], env[ENV_K9_SO].mEnv);
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
static void __attribute__((destructor))
libk9_exit()
{ }

/* -------------------------------------------------------------------------- */
