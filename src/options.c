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

#include "options.h"
#include "macros.h"
#include "error.h"
#include "process.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <getopt.h>

struct Options gOptions;

#define DEFAULT_TIMEOUT 30

/* -------------------------------------------------------------------------- */
static const char sUsage[] =
"usage : %s [ options ] cmd ...\n"
"        %s { --pidfile file | -p file }\n"
"\n"
"options:\n"
"  --debug | -d\n"
"      Print debug information.\n"
"  --fd N | -f N\n"
"      Tether child using file descriptor N in the child process, and\n"
"      copy received data to stdout of the watchdog. Specify N as - to\n"
"      allocate a new file descriptor. [Default: N = 1 (stdout) ].\n"
"  --setpgid | -s\n"
"      Place the child process into its own process group. This is\n"
"      useful if the child will create its own family of processes\n"
"      and the watchdog is not itself being supervised.\n"
"      [Default: Do not place child in its own process group\n"
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
"      Terminate the child process if this process ever becomes a child\n"
"      of init(8). This option is only useful if the parent of this\n"
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
"  --timeout N | -t N\n"
"      Specify the timeout N in seconds for activity on tether from\n"
"      the child process. Set N to 0 to avoid imposing any timeout at\n"
"      all. [Default: N = " STRINGIFY(DEFAULT_TIMEOUT) "]\n"
"  --untethered | -u\n"
"      Run child process without a tether and only watch for termination.\n"
"      [Default: Tether child process]\n"
"";

static const char sShortOptions[] =
    "df:in:oP:p:qsTt:u";

static struct option sLongOptions[] =
{
    { "debug",      0, 0, 'd' },
    { "fd",         1, 0, 'f' },
    { "identify",   0, 0, 'i' },
    { "name",       1, 0, 'n' },
    { "orphaned",   0, 0, 'o' },
    { "pid",        1, 0, 'P' },
    { "pidfile",    1, 0, 'p' },
    { "quiet",      0, 0, 'q' },
    { "test",       0, 0, 'T' },
    { "timeout",    1, 0, 't' },
    { "untethered", 0, 0, 'u' },
    { 0 },
};

/* -------------------------------------------------------------------------- */
static void
showUsage_(void)
{
    const char *arg0 = ownProcessName();

    dprintf(STDERR_FILENO, sUsage, arg0, arg0);
    _exit(1);
}

/* -------------------------------------------------------------------------- */
static unsigned long long
parseUnsignedLongLong_(const char *aArg)
{
    unsigned long long arg;

    do
    {
        if (isdigit((unsigned char) *aArg))
        {
            char *endptr = 0;

            errno = 0;
            arg   = strtoull(aArg, &endptr, 10);

            if (!*endptr && (ULLONG_MAX != arg || ERANGE != errno))
            {
                errno = 0;
                break;
            }
        }

        errno = ERANGE;
        arg   = ULLONG_MAX;

    } while (0);

    return arg;
}
/* -------------------------------------------------------------------------- */
int
parseInt(const char *aArg, int *aValue)
{
    int rc = -1;

    unsigned long long value = parseUnsignedLongLong_(aArg);

    if ( ! errno)
    {
        *aValue = value;

        if ( ! (*aValue - value))
            rc = 0;
    }

    return rc;
}

/* -------------------------------------------------------------------------- */
int
parsePid(const char *aArg, pid_t *aValue)
{
    int rc = -1;

    unsigned long long value = parseUnsignedLongLong_(aArg);

    if ( ! errno)
    {
        *aValue = value;

        if ( !   (*aValue - value) &&
             0 <= *aValue)
        {
            rc = 0;
        }
    }

    return rc;
}

/* -------------------------------------------------------------------------- */
char **
parseOptions(int argc, char **argv)
{
    int pidFileOnly = 0;

    gOptions.mTimeout   = DEFAULT_TIMEOUT;
    gOptions.mTetherFd  = STDOUT_FILENO;
    gOptions.mTether    = &gOptions.mTetherFd;

    while (1)
    {
        int longOptIndex = 0;

        int opt = getopt_long(
                argc, argv, sShortOptions, sLongOptions, &longOptIndex);

        if (-1 == opt)
            break;

        switch (opt)
        {
        default:
            terminate(0, "Unrecognised option %d ('%c')", opt, opt);
            break;

        case '?':
            showUsage_();
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
                if (parseInt(
                        optarg,
                        &gOptions.mTetherFd) || 0 > gOptions.mTetherFd)
                {
                    terminate(0, "Badly formed fd - '%s'", optarg);
                }
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
            else if (parsePid(optarg, &gOptions.mPid))
                terminate(0, "Badly formed pid - '%s'", optarg);
            break;

        case 'n':
            pidFileOnly = -1;
            if ( ! optarg[0])
                terminate(0, "Empty environment or argument name");
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

        case 's':
            pidFileOnly = -1;
            gOptions.mSetPgid = true;
            break;

        case 'T':
            gOptions.mTest = true;
            break;

        case 't':
            pidFileOnly = -1;
            if (parseInt(optarg, &gOptions.mTimeout) || 0 > gOptions.mTimeout)
                terminate(0, "Badly formed timeout - '%s'", optarg);
            break;

        case 'u':
            pidFileOnly = -1;
            gOptions.mTether = 0;
            break;
        }
    }

    if (0 >= pidFileOnly)
    {
        if (optind >= argc)
            terminate(0, "Missing command for execution");
    }

    return optind < argc ? argv + optind : 0;
}

/* -------------------------------------------------------------------------- */
