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

#include "options_.h"
#include "macros_.h"
#include "error_.h"
#include "parse_.h"
#include "process_.h"
#include "env_.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

struct Options gOptions;

#define DEFAULT_TETHER_TIMEOUT_S    30
#define DEFAULT_UMBILICAL_TIMEOUT_S 30
#define DEFAULT_SIGNAL_PERIOD_S     30
#define DEFAULT_DRAIN_TIMEOUT_S     30

/* -------------------------------------------------------------------------- */
static const char sUsage[] =
"usage : %s [ options ] cmd ...\n"
"        %s { --pidfile file | -p file }\n"
"\n"
"options:\n"
"  --debug | -d\n"
"      Print debug information. Specify the option multiple times to\n"
"      increase the debug level.\n"
"  --cordless | -c\n"
"      When running with a tether, do not run the child with an umbilical\n"
"      connection. An umbilical cord allows the child terminate immediately\n"
"      the watchdog is absent. [Default: Use an umbilical cord]\n"
"  --fd N | -f N\n"
"      Tether child using file descriptor N in the child process, and\n"
"      copy received data to stdout of the watchdog. Specify N as - to\n"
"      allocate a new file descriptor. [Default: N = 1 (stdout) ].\n"
"  --identify | -i\n"
"      Print the pid of the child process on stdout before starting\n"
"      the child program. [Default: Do not print the pid of the child]\n"
"  --name N | -n N\n"
"      Name the fd of the tether. If N matches [A-Z][A-Z0-9_]*, then\n"
"      create an environment variable of that name and set is value to\n"
"      the fd of the tether. Otherwise replace the first command\n"
"      line argument with a substring that matches N with the fd\n"
"      of the tether. [Default: Do not advertise fd]\n"
"  --orphaned | -o\n"
"      If this process ever becomes a child of init(8), terminate the\n"
"      child process. This option is only useful if the parent of this\n"
"      process is not init(8). [Default: Allow this process to be orphaned]\n"
"  --pid N | -P N\n"
"      Specify value to write to pidfile. Set N to 0 to use pid of child,\n"
"      set N to -1 to use the pid of the watchdog, otherwise use N as the\n"
"      pid of the child. [Default: Use the pid of child]\n"
"  --pidfile file | -p file\n"
"      Write the pid of the child to the specified file, and remove the\n"
"      file when the child terminates. [Default: No pidfile]\n"
"  --quiet | -q\n"
"      Do not copy received data from tether to stdout. This is an\n"
"      alternative to closing stdout. [Default: Copy data from tether]\n"
"  --test N\n"
"      Run in test mode using a non-zero test level. [Default: No test]\n"
"  --timeout L | -t L\n"
"      Specify the timeout list L. The list L comprises up to four\n"
"      comma separated values: T, U, V and W. Each of the values is either\n"
"      empty, in which case the value is not changed, or a non-negative\n"
"      indicating a new value.\n"
"        T  timeout in seconds for activity on the tether, zero to disable\n"
"        U  timeout in seconds for activity on the umbilical, zero to disable\n"
"        V  delay in seconds between signals to terminate the child\n"
"        W  timeout in to drain data from the tether, zero to disable\n"
"      [Default: T,U,V,W = "
    STRINGIFY(DEFAULT_TETHER_TIMEOUT_S)
    STRINGIFY(DEFAULT_UMBILICAL_TIMEOUT_S)
    STRINGIFY(DEFAULT_SIGNAL_PERIOD_S)
    STRINGIFY(DEFAULT_DRAIN_TIMEOUT_S) "]\n"
"  --untethered | -u\n"
"      Run child process without a tether and only watch for termination.\n"
"      [Default: Tether child process]\n"
"";

static const char sShortOptions[] =
    "+cD:df:iL::n:oP:p:qTt:u";

enum OptionKind
{
    OptionTest = CHAR_MAX + 1,
};

static struct option sLongOptions[] =
{
    { "cordless",   no_argument,       0, 'c' },
    { "debug",      no_argument,       0, 'd' },
    { "fd",         required_argument, 0, 'f' },
    { "identify",   no_argument,       0, 'i' },
    { "name",       required_argument, 0, 'n' },
    { "orphaned",   no_argument,       0, 'o' },
    { "pid",        required_argument, 0, 'P' },
    { "pidfile",    required_argument, 0, 'p' },
    { "quiet",      no_argument,       0, 'q' },
    { "test",       required_argument, 0, OptionTest },
    { "timeout",    required_argument, 0, 't' },
    { "untethered", no_argument,       0, 'u' },
    { 0 },
};

/* -------------------------------------------------------------------------- */
static void
showUsage_(void)
{
    const char *arg0 = ownProcessName();

    dprintf(STDERR_FILENO, sUsage, arg0, arg0);
}

/* -------------------------------------------------------------------------- */
void
initOptions()
{
    gOptions.mTimeout.mTether_s    = DEFAULT_TETHER_TIMEOUT_S;
    gOptions.mTimeout.mSignal_s    = DEFAULT_SIGNAL_PERIOD_S;
    gOptions.mTimeout.mUmbilical_s = DEFAULT_UMBILICAL_TIMEOUT_S;
    gOptions.mTimeout.mDrain_s     = DEFAULT_DRAIN_TIMEOUT_S;

    gOptions.mTetherFd = STDOUT_FILENO;
    gOptions.mTether   = &gOptions.mTetherFd;

    if (getEnvUInt("K9_DEBUG", &gOptions.mDebug))
    {
        gOptions.mDebug = 0;

        if (ENOENT != errno)
            warn(
                errno,
                "Unable to configure debug setting %s",
                getenv("K9_DEBUG"));
    }
}

/* -------------------------------------------------------------------------- */
static int
processTimeoutOption(const char *aArg)
{
    int rc = -1;

    struct ParseArgList *argList = 0;

    struct ParseArgList argList_;
    ERROR_IF(
        createParseArgListCSV(&argList_, aArg));
    argList = &argList_;

    ERROR_IF(
        1 > argList->mArgc || 4 < argList->mArgc,
        {
            errno = EINVAL;
        });

    ERROR_IF(
        parseUInt(argList->mArgv[0], &gOptions.mTimeout.mTether_s));

    if (1 < argList->mArgc && *argList->mArgv[1])
    {
        ERROR_IF(
            parseUInt(argList->mArgv[1], &gOptions.mTimeout.mUmbilical_s));
    }

    if (2 < argList->mArgc && *argList->mArgv[2])
    {
        ERROR_IF(
            parseUInt(argList->mArgv[2], &gOptions.mTimeout.mSignal_s));
        ERROR_IF(
            0 >= gOptions.mTimeout.mSignal_s,
            {
                errno = EINVAL;
            });
    }

    if (3 < argList->mArgc && *argList->mArgv[3])
        ERROR_IF(
            parseUInt(argList->mArgv[3], &gOptions.mTimeout.mDrain_s));

    rc = 0;

Finally:

    FINALLY
    ({
        if (argList)
            closeParseArgList(argList);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
processOptions(int argc, char **argv, char ***args)
{
    int rc = -1;

    int pidFileOnly = 0;

    initOptions();

    while (1)
    {
        int longOptIndex = 0;

        int opt;
        ERROR_IF(
            (opt = getopt_long(
                argc, argv, sShortOptions, sLongOptions, &longOptIndex),
             '?' == opt),
            {
                showUsage_();
                errno = EINVAL;
            });

        if (-1 == opt)
            break;

        switch (opt)
        {
        default:
            ERROR_IF(
                true,
                {
                    errno = EINVAL;
                    message(0, "Unrecognised option %d ('%c')", opt, opt);
                });
            break;

        case 'c':
            pidFileOnly = -1;
            gOptions.mCordless = true;
            break;

        case 'd':
            ++gOptions.mDebug;
            break;

        case 'f':
            pidFileOnly = -1;
            gOptions.mTether = &gOptions.mTetherFd;
            if ( ! strcmp(optarg, "-"))
            {
                gOptions.mTetherFd = -1;
            }
            else
            {
                ERROR_IF(
                    parseInt(
                        optarg,
                        &gOptions.mTetherFd) || 0 > gOptions.mTetherFd,
                    {
                        errno = EINVAL;
                        message(0, "Badly formed fd - '%s'", optarg);
                    });
            }
            break;

        case 'i':
            pidFileOnly = -1;
            gOptions.mIdentify = true;
            break;

        case 'o':
            pidFileOnly = -1;
            gOptions.mOrphaned = true;
            break;

        case 'P':
            pidFileOnly = -1;
            if ( ! strcmp(optarg, "-1"))
                gOptions.mPid = -1;
            else
            {
                ERROR_IF(
                    parsePid(optarg, &gOptions.mPid),
                    {
                        errno = EINVAL;
                        message(0, "Badly formed pid - '%s'", optarg);
                    });
            }
            break;

        case 'n':
            pidFileOnly = -1;
            ERROR_UNLESS(
                optarg[0],
                {
                    errno = EINVAL;
                    message(0, "Empty environment or argument name");
                });
            gOptions.mName = optarg;
            break;

        case 'p':
            pidFileOnly = pidFileOnly ? pidFileOnly : 1;
            gOptions.mPidFile = optarg;
            break;

        case 'q':
            pidFileOnly = -1;
            gOptions.mQuiet = true;
            break;

        case OptionTest:
            ERROR_IF(
                parseUInt(optarg, &gOptions.mTest),
                {
                    errno = EINVAL;
                    message(0, "Badly formed test level - '%s'", optarg);
                });
            ERROR_UNLESS(
                gOptions.mTest,
                {
                    errno = EINVAL;
                    message(0, "Test level must be non-zero");
                });
            break;

        case 't':
            pidFileOnly = -1;
            ERROR_IF(
                processTimeoutOption(optarg),
                {
                    errno = EINVAL;
                    message(0, "Badly formed timeout - '%s'", optarg);
                });
            break;

        case 'u':
            pidFileOnly = -1;
            gOptions.mTether = 0;
            break;
        }
    }

    if (0 >= pidFileOnly)
    {
        ERROR_IF(
            optind >= argc,
            {
                errno = EINVAL;
                message(0, "Missing command for execution");
            });
    }

    *args = optind < argc ? argv + optind : 0;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
