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
#include "socketpair_.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <inttypes.h>

#include <sys/resource.h>

extern char **_dl_argv;

/* -------------------------------------------------------------------------- */
enum EnvKind
{
    ENV_LD_PRELOAD,
    ENV_K9_SO,
    ENV_K9_FD,
    ENV_K9_PID,
    ENV_K9_TIME,
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
struct WatchThread
{
    intmax_t          mStack_[PTHREAD_STACK_MIN / sizeof(intmax_t)];
    intmax_t         *mStack;
    int               mFd;
    struct SocketPair mSync;
    char              mSyncBuf[1];
};

#if 0
static void
printErr(int aErr, const char *aFmt, ...)
{
    va_list argp;

    va_start(argp, aFmt);
    vdprintf(STDERR_FILENO, aFmt, argp);
    va_end(argp);
    if (aErr)
        dprintf(STDERR_FILENO, "- error %d\n", aErr);
    else
        dprintf(STDERR_FILENO, "\n");
}
#endif

#if 0
static void
watchTether(const char *aFd)
{
    (void) printErr;
}
#else

static int
watchTether_(void *aWatchThread)
{
    /* The watchThread structure is owned by the parent thread, so
     * ensure that the child makes no attempt to modify the structure
     * here. */

    const struct WatchThread *watchThread = aWatchThread;

    char syncBuf[sizeof(watchThread->mSyncBuf)];

    if (1 != readFile(watchThread->mSync.mChildFile,
                      syncBuf,
                      sizeof(syncBuf)))
        terminate(
            errno,
            "Unable to synchronise umbilical thread");

    if ( ! ownFdValid(watchThread->mFd))
        terminate(
            errno,
            "Umbilical file descriptor is not valid %d", watchThread->mFd);

    if (1 != writeFile(watchThread->mSync.mChildFile,
                       syncBuf,
                       sizeof(syncBuf)))
        terminate(
            errno,
            "Unable to synchronise umbilical thread");

    /* Since the child has its own file descriptor space, close
     * all unnecessary file descriptors so that the child will not
     * inadvertently corrupt or pollute the file descriptors of the
     * child process.
     *
     * Note that this will close the file descriptors in watchThread->mSync
     * so no attempt should be made to them after this point. */

    struct rlimit noFile;
    if (getrlimit(RLIMIT_NOFILE, &noFile))
        terminate(
            errno,
            "Unable to obtain file descriptor limit");

    for (int fd = 0; fd < noFile.rlim_cur; ++fd)
    {
        if (  ! stdFd(fd) && fd != watchThread->mFd)
            (void) close(fd);
    }

    warn(0, "*** RUNNING CHILD ***");

    return 0;
}

static void
watchTether(const char *aFd)
{
    static struct WatchThread watchThread;

    while (aFd)
    {
        //dprintf(2, "********* START %d\n", getpid());
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

        watchThread.mFd = fd;

        /* The stack is allocated statically to keep the footprint
         * of the shared library contained. Dynamically allocating
         * the stack (eg using mmap(2)) would create another
         * visible artifact indicating the presence of this library. */

        uintptr_t frameChild  = (uintptr_t) __builtin_frame_address(0);
        uintptr_t frameParent = (uintptr_t) __builtin_frame_address(1);

        if (frameChild == frameParent)
            terminate(
                0,
                "Unable to ascertain direction of stack growth");

        watchThread.mStack = watchThread.mStack_;

        if (frameChild < frameParent)
            watchThread.mStack += NUMBEROF(watchThread.mStack_);

        memset(watchThread.mSyncBuf, 0, sizeof(watchThread.mSyncBuf));

        if (createSocketPair(&watchThread.mSync))
            terminate(
                errno,
                "Unable to create synchronisation pipe");

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

        if (-1 == clone(
                watchTether_,
                watchThread.mStack,
                CLONE_VM | CLONE_SIGHAND | CLONE_THREAD | CLONE_FS,
                &watchThread, 0, 0, 0, 0, 0))
        {
            terminate(
                errno,
                "Unable to create umbilical thread");
        }

        if (closeFd(&fd))
            terminate(
                errno,
                "Unable to close umbilical file descriptor %d", fd);

        warn(0, "********* SYNCRHONISING");

        if (1 != writeFile(watchThread.mSync.mParentFile,
                           watchThread.mSyncBuf,
                           sizeof(watchThread.mSyncBuf)))
            terminate(
                errno,
                "Unable to synchronise umbilical thread");

        if (1 != readFile(watchThread.mSync.mParentFile,
                          watchThread.mSyncBuf,
                          sizeof(watchThread.mSyncBuf)))
            terminate(
                errno,
                "Unable to synchronise umbilical thread");

        if (closeSocketPair(&watchThread.mSync))
            terminate(
                errno,
                "Unable to close synchronisation pipe");

        warn(0, "********* DONE");

        break;
    }
}
#endif

/* -------------------------------------------------------------------------- */
static void  __attribute__((constructor))
libk9_init()
{
    if (Error_init())
        terminate(
            0,
            "Unable to initialise error module");

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

                watchTether(env[ENV_K9_FD].mEnv);
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
