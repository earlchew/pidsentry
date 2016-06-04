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
static const char programUsage_[] =
"usage : %s [ monitoring-options | general-options ] cmd ...\n"
"        %s { --pidfile file | -p file } [ general-options ]\n"
"        %s { --pidfile file | -p file } [ general-options ] -c cmd ... \n"
"\n"
"mode:\n"
" --command | -c\n"
"      Execute a command against a running child process. Run as a shell\n"
"      command if cmd comprises a single word that contains any whitespace\n"
"      and whose first character is alphanumeric  This option\n"
"      requires --pidfile to also be specified. [Default: No command]\n"
"\n"
"general options:\n"
"  --debug | -d\n"
"      Print debug information. Specify the option multiple times to\n"
"      increase the debug level.\n"
"  --pidfile file | -p file\n"
"      The pid of the child is stored in the specified file, and the files\n"
"      is removed when the child terminates. [Default: No pidfile]\n"
"\n"
"monitoring options:\n"
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
"        W  timeout in seconds to drain data from the tether, zero to disable\n"
"      [Default: T,U,V,W = "
    STRINGIFY(DEFAULT_TETHER_TIMEOUT_S) ","
    STRINGIFY(DEFAULT_UMBILICAL_TIMEOUT_S) ","
    STRINGIFY(DEFAULT_SIGNAL_PERIOD_S) ","
    STRINGIFY(DEFAULT_DRAIN_TIMEOUT_S) "]\n"
"  --untethered | -u\n"
"      Run child process without a tether and only watch for termination.\n"
"      [Default: Tether child process]\n"
"";

static const char shortOptions_[] =
    "+cD:df:iL::n:op:qTt:u";

enum OptionKind
{
    OptionTest = CHAR_MAX + 1,
};

static struct option longOptions_[] =
{
    { "command",    no_argument,       0, 'c' },
    { "debug",      no_argument,       0, 'd' },
    { "fd",         required_argument, 0, 'f' },
    { "identify",   no_argument,       0, 'i' },
    { "name",       required_argument, 0, 'n' },
    { "orphaned",   no_argument,       0, 'o' },
    { "pidfile",    required_argument, 0, 'p' },
    { "quiet",      no_argument,       0, 'q' },
    { "test",       required_argument, 0, OptionTest },
    { "timeout",    required_argument, 0, 't' },
    { "untethered", no_argument,       0, 'u' },
    { 0 },
};

/* -------------------------------------------------------------------------- */
enum OptionMode
{
    OptionModeError   = -1,
    OptionModeUnknown = 0,

    OptionModeMonitorChild,
    OptionModePrintPid,
    OptionModeRunCommand
};

static CHECKED enum OptionMode
setOptionMode(enum OptionMode  self,
              enum OptionMode  aMode,
              const char      *aLongOpt,
              char             aShortOpt)
{
    enum OptionMode rc = OptionModeError;

    if (OptionModeError != self)
    {
        if (OptionModeUnknown == self || aMode == self)
            rc = aMode;
        else
        {
            if (aLongOpt)
                message(0, "Incompatible option --%s", aLongOpt);
            else
                message(0, "Incompatible option -%c", aShortOpt);
        }
    }

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
showUsage_(void)
{
    const char *arg0 = ownProcessName();

    dprintf(STDERR_FILENO, programUsage_, arg0, arg0, arg0);
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
}

/* -------------------------------------------------------------------------- */
static CHECKED int
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
            argList = closeParseArgList(argList);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
processOptions(int argc, char **argv, const char * const **args)
{
    int rc = -1;

    initOptions();

    enum OptionMode mode = OptionModeUnknown;

    while (1)
    {
        ERROR_IF(OptionModeError == mode);

        int longOptIndex = NUMBEROF(longOptions_) - 1;

        int opt;
        ERROR_IF(
            (opt = getopt_long(
                argc, argv, shortOptions_, longOptions_, &longOptIndex),
             '?' == opt),
            {
                showUsage_();
                errno = EINVAL;
            });

        const char *longOptName = longOptions_[longOptIndex].name;

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
            mode = setOptionMode(
                mode, OptionModeRunCommand, longOptName, opt);
            gOptions.mCommand = true;
            break;

        case 'd':
            ++gOptions.mDebug;
            break;

        case 'f':
            mode = setOptionMode(
                mode, OptionModeMonitorChild, longOptName, opt);
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
            mode = setOptionMode(
                mode, OptionModeMonitorChild, longOptName, opt);
            gOptions.mIdentify = true;
            break;

        case 'o':
            mode = setOptionMode(
                mode, OptionModeMonitorChild, longOptName, opt);
            gOptions.mOrphaned = true;
            break;

        case 'n':
            mode = setOptionMode(
                mode, OptionModeMonitorChild, longOptName, opt);
            ERROR_UNLESS(
                optarg[0],
                {
                    errno = EINVAL;
                    message(0, "Empty environment or argument name");
                });
            gOptions.mName = optarg;
            break;

        case 'p':
            gOptions.mPidFile = optarg;
            break;

        case 'q':
            mode = setOptionMode(
                mode, OptionModeMonitorChild, longOptName, opt);
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
            mode = setOptionMode(
                mode, OptionModeMonitorChild, longOptName, opt);
            ERROR_IF(
                processTimeoutOption(optarg),
                {
                    errno = EINVAL;
                    message(0, "Badly formed timeout - '%s'", optarg);
                });
            break;

        case 'u':
            mode = setOptionMode(
                mode, OptionModeMonitorChild, longOptName, opt);
            gOptions.mTether = 0;
            break;
        }
    }

    /* If no option has been detected that specifically sets the mode
     * of operation, force the default mode of operation. */

    if (OptionModeUnknown == mode)
        mode = OptionModeMonitorChild;

    switch (mode)
    {
    default:
        break;

    case OptionModeRunCommand:
    case OptionModeMonitorChild:
        ERROR_IF(
            optind >= argc,
            {
                errno = EINVAL;
                message(0, "Missing command for execution");
            });
        break;
    }

    *args = (const char * const *) (optind < argc ? argv + optind : 0);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
