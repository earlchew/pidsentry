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

#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern char **_dl_argv;

/* -------------------------------------------------------------------------- */
enum EnvKind
{
    ENV_LD_PRELOAD,
    ENV_K9_SO,
    ENV_K9_FD,
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
static void  __attribute__((constructor))
libk9_init()
{
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
    };

    initEnv(env, NUMBEROF(env), envp);

    stripEnvPreload(&env[ENV_LD_PRELOAD], env[ENV_K9_SO].mEnv);
}

/* -------------------------------------------------------------------------- */
static void __attribute__((destructor))
libk9_exit()
{ }

/* -------------------------------------------------------------------------- */
