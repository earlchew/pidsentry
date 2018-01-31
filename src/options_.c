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

#include "ert/compiler.h"
#include "ert/parse.h"
#include "ert/process.h"

#include <string.h>
#include <unistd.h>
#include <getopt.h>

struct Options gOptions;

#define DEFAULT_TETHER_TIMEOUT_S    30
#define DEFAULT_UMBILICAL_TIMEOUT_S 30
#define DEFAULT_SIGNAL_PERIOD_S     30
#define DEFAULT_DRAIN_TIMEOUT_S     30

/* -------------------------------------------------------------------------- */
static const char programUsage_[] =
"usage : %s { --server | -s } [ monitoring-options | "
                               "general-options ] cmd ...\n"
"        %s { --client | -c } [ general-options ] file cmd ... \n"
"\n"
"mode:\n"
" --server | -s\n"
"      Start and monitor a server process using the specified command.\n"
" --client | -c\n"
"      Execute a client command against a running child process.\n"
"      The pid of the child process will be retrieved from the named file.\n"
"\n"
"      For both -s and -c, run as a shell command if cmd comprises\n"
"      a single word that contains any whitespace and whose first character\n"
"      is alphanumeric.\n"
"\n"
"general options:\n"
"  --debug | -d\n"
"      Print debug information. Specify the option multiple times to\n"
"      increase the debug level. [Default: No debug]\n"
"  --test N\n"
"      Run in test mode using a non-zero test level. [Default: No test]\n"
"\n"
"client options:\n"
"  --relaxed | -R\n"
"      Always run the client command, even if there is no child process\n"
"      running. The environment variable PIDSENTRY_PID to will only be set\n"
"      if there is a child process running.\n"
"\n"
"server options:\n"
"  --announce | -a\n"
"      Announce the name of program or the shell command running in the\n"
"      child process as it is started and when it has stopped.\n"
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
"  --pidfile file | -p file\n"
"      The pid of the child is stored in the specified file, and the files\n"
"      is removed when the child terminates. [Default: No pidfile]\n"
"  --quiet | -q\n"
"      Do not copy received data from tether to stdout. This is an\n"
"      alternative to closing stdout. [Default: Copy data from tether]\n"
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
    ERT_STRINGIFY(DEFAULT_TETHER_TIMEOUT_S) ","
    ERT_STRINGIFY(DEFAULT_UMBILICAL_TIMEOUT_S) ","
    ERT_STRINGIFY(DEFAULT_SIGNAL_PERIOD_S) ","
    ERT_STRINGIFY(DEFAULT_DRAIN_TIMEOUT_S) "]\n"
"  --untethered | -u\n"
"      Run child process without a tether and only watch for termination.\n"
"      [Default: Tether child process]\n"
"";

static const char shortOptions_[] =
    "+acD:df:iL::n:op:qRsTt:u";

enum OptionKind
{
    OptionTest = CHAR_MAX + 1,
};

static struct option longOptions_[] =
{
    { "announce",   no_argument,       0, 'a' },
    { "client",     no_argument,       0, 'c' },
    { "debug",      no_argument,       0, 'd' },
    { "fd",         required_argument, 0, 'f' },
    { "relaxed",    no_argument,       0, 'R' },
    { "identify",   no_argument,       0, 'i' },
    { "name",       required_argument, 0, 'n' },
    { "orphaned",   no_argument,       0, 'o' },
    { "pidfile",    required_argument, 0, 'p' },
    { "quiet",      no_argument,       0, 'q' },
    { "server",     no_argument,       0, 's' },
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
    OptionModeRunCommand
};

static ERT_CHECKED enum OptionMode
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
    const char *arg0 = ert_ownProcessName();

    dprintf(STDERR_FILENO, programUsage_, arg0, arg0);
}

/* -------------------------------------------------------------------------- */
void
initOptions()
{
    gOptions.mServer.mTimeout.mTether_s    = DEFAULT_TETHER_TIMEOUT_S;
    gOptions.mServer.mTimeout.mSignal_s    = DEFAULT_SIGNAL_PERIOD_S;
    gOptions.mServer.mTimeout.mUmbilical_s = DEFAULT_UMBILICAL_TIMEOUT_S;
    gOptions.mServer.mTimeout.mDrain_s     = DEFAULT_DRAIN_TIMEOUT_S;

    gOptions.mServer.mTetherFd = STDOUT_FILENO;
    gOptions.mServer.mTether   = &gOptions.mServer.mTetherFd;
}

/* -------------------------------------------------------------------------- */
static ERT_CHECKED int
processTimeoutOption(const char *aArg)
{
    int rc = -1;

    struct Ert_ParseArgList *argList = 0;

    struct Ert_ParseArgList argList_;
    ERROR_IF(
        ert_createParseArgListCSV(&argList_, aArg));
    argList = &argList_;

    ERROR_IF(
        1 > argList->mArgc || 4 < argList->mArgc,
        {
            errno = EINVAL;
        });

    ERROR_IF(
        ert_parseUInt(argList->mArgv[0], &gOptions.mServer.mTimeout.mTether_s));

    if (1 < argList->mArgc && *argList->mArgv[1])
    {
        ERROR_IF(
            ert_parseUInt(
                argList->mArgv[1], &gOptions.mServer.mTimeout.mUmbilical_s));
    }

    if (2 < argList->mArgc && *argList->mArgv[2])
    {
        ERROR_IF(
            ert_parseUInt(
                argList->mArgv[2], &gOptions.mServer.mTimeout.mSignal_s));
        ERROR_IF(
            0 >= gOptions.mServer.mTimeout.mSignal_s,
            {
                errno = EINVAL;
            });
    }

    if (3 < argList->mArgc && *argList->mArgv[3])
        ERROR_IF(
            ert_parseUInt(
                argList->mArgv[3], &gOptions.mServer.mTimeout.mDrain_s));

    rc = 0;

Finally:

    FINALLY
    ({
        if (argList)
            argList = ert_closeParseArgList(argList);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
processOptions(int argc, char **argv, const char * const **args)
{
    int rc = -1;

    struct Ert_Options options = { };

    initOptions();

    enum OptionMode mode = OptionModeUnknown;

    while (1)
    {
        ERROR_IF(
            OptionModeError == mode,
            {
                errno = EINVAL;
            });

        int longOptIndex = ERT_NUMBEROF(longOptions_) - 1;

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

        case 'a':
            mode = setOptionMode(
                mode, OptionModeMonitorChild, longOptName, opt);
            gOptions.mServer.mAnnounce = true;
            break;

        case 'c':
            mode = setOptionMode(
                mode, OptionModeRunCommand, longOptName, opt);
            gOptions.mClient.mActive = true;
            break;

        case 'd':
            ++options.mDebug;
            break;

        case 'f':
            mode = setOptionMode(
                mode, OptionModeMonitorChild, longOptName, opt);
            gOptions.mServer.mTether = &gOptions.mServer.mTetherFd;
            if ( ! strcmp(optarg, "-"))
            {
                gOptions.mServer.mTetherFd = -1;
            }
            else
            {
                ERROR_IF(
                    ert_parseInt(
                        optarg,
                        &gOptions.mServer.mTetherFd) ||
                    0 > gOptions.mServer.mTetherFd,
                    {
                        errno = EINVAL;
                        message(0, "Badly formed fd - '%s'", optarg);
                    });
            }
            break;

        case 'i':
            mode = setOptionMode(
                mode, OptionModeMonitorChild, longOptName, opt);
            gOptions.mServer.mIdentify = true;
            break;

        case 'o':
            mode = setOptionMode(
                mode, OptionModeMonitorChild, longOptName, opt);
            gOptions.mServer.mOrphaned = true;
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
            gOptions.mServer.mName = optarg;
            break;

        case 'p':
            mode = setOptionMode(
                mode, OptionModeMonitorChild, longOptName, opt);
            ERROR_UNLESS(
                optarg[0],
                {
                    errno = EINVAL;
                    message(0, "Empty pid file name");
                });
            gOptions.mServer.mPidFile = optarg;
            break;

        case 'q':
            mode = setOptionMode(
                mode, OptionModeMonitorChild, longOptName, opt);
            gOptions.mServer.mQuiet = true;
            break;

        case 'R':
            mode = setOptionMode(
                mode, OptionModeRunCommand, longOptName, opt);
            gOptions.mClient.mRelaxed = true;
            break;

        case 's':
            mode = setOptionMode(
                mode, OptionModeMonitorChild, longOptName, opt);
            gOptions.mServer.mActive = true;
            break;

        case OptionTest:
            ERROR_IF(
                ert_parseUInt(optarg, &options.mTest),
                {
                    errno = EINVAL;
                    message(0, "Badly formed test level - '%s'", optarg);
                });
            ERROR_UNLESS(
                options.mTest,
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
            gOptions.mServer.mTether = 0;
            break;
        }
    }

    ert_initOptions(&options);

    ERROR_IF(
        OptionModeUnknown == mode,
        {
            errno = EINVAL;
            message(0, "Unspecified mode of operation");
        });

    switch (mode)
    {
    default:
        ensure(false);
        break;

    case OptionModeRunCommand:
        ensure(   gOptions.mClient.mActive);
        ensure( ! gOptions.mServer.mActive);
        ERROR_IF(
            optind >= argc,
            {
                errno = EINVAL;
                message(0, "Missing pid file for command");
            });

        gOptions.mClient.mPidFile = argv[optind++];
        break;

    case OptionModeMonitorChild:
        ensure(   gOptions.mServer.mActive);
        ensure( ! gOptions.mClient.mActive);
        break;
    }

    ERROR_IF(
        optind >= argc,
        {
            errno = EINVAL;
            message(0, "Missing command for execution");
        });

    *args = (const char * const *) (optind < argc ? argv + optind : 0);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
