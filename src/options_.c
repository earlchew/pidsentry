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

#include "options_.h"
#include "macros_.h"
#include "error_.h"
#include "parse_.h"
#include "process_.h"
#include "env_.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <getopt.h>

struct Options gOptions;

#define DEFAULT_TETHER_TIMEOUT_S 30
#define DEFAULT_EXIT_PACING_S    30

/* -------------------------------------------------------------------------- */
static const char sUsage[] =
"usage : %s [ options ] cmd ...\n"
"\n"
"options:\n"
"  --debug | -d\n"
"      Print debug information.\n"
"  --cordless | -c\n"
"      When running with a tether, do not run the child with an umbilical\n"
"      connection. An umbilical cord allows the child terminate immediately\n"
"      the watchdog is absent. [Default: Use an umbilical cord]\n"
"  --delay N | -D N\n"
"      Specify the delay N in seconds between signals when terminating\n"
"      the child process. [Default: " STRINGIFY(DEFAULT_EXIT_PACING_S) "]\n"
"  --fd N | -f N\n"
"      Tether child using file descriptor N in the child process, and\n"
"      copy received data to stdout of the watchdog. Specify N as - to\n"
"      allocate a new file descriptor. [Default: N = 1 (stdout) ].\n"
"  --identify | -i\n"
"      Print the pid of the child process on stdout before starting\n"
"      the child program. [Default: Do not print the pid of the child]\n"
"  --library [N] | -L [N]\n"
"      Specify the libk9.so to use when tethering the child. If N is not\n"
"      provided, do not attach libk9.so to the child process.\n"
"      [Default: Use installed libk9.so]\n"
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
"  --setpgid | -s\n"
"      Place the child process into its own process group. This is\n"
"      useful if the child will create its own family of processes\n"
"      and the watchdog is not itself being supervised.\n"
"      [Default: Do not place child in its own process group\n"
"  --timeout N | -t N\n"
"      Specify the timeout N in seconds for activity on tether from\n"
"      the child process. Set N to 0 to avoid imposing any timeout at\n"
"      all. [Default: N = " STRINGIFY(DEFAULT_TETHER_TIMEOUT) "]\n"
"  --untethered | -u\n"
"      Run child process without a tether and only watch for termination.\n"
"      [Default: Tether child process]\n"
"";

static const char sShortOptions[] =
    "cD:df:iL::n:oP:p:qsTt:u";

static struct option sLongOptions[] =
{
    { "cordless",   0, 0, 'c' },
    { "debug",      0, 0, 'd' },
    { "delay",      1, 0, 'D' },
    { "fd",         1, 0, 'f' },
    { "identify",   0, 0, 'i' },
    { "library",    1, 0, 'L' },
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

    dprintf(STDERR_FILENO, sUsage, arg0);
    _exit(1);
}

/* -------------------------------------------------------------------------- */
void
initOptions()
{
    gOptions.mTimeout_s = DEFAULT_TETHER_TIMEOUT_S;
    gOptions.mPacing_s  = DEFAULT_EXIT_PACING_S;
    gOptions.mTetherFd  = STDOUT_FILENO;
    gOptions.mTether    = &gOptions.mTetherFd;

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
char **
processOptions(int argc, char **argv)
{
    int pidFileOnly = 0;

    initOptions();

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

        case 'c':
            pidFileOnly = -1;
            gOptions.mCordless = true;
            break;

        case 'd':
            ++gOptions.mDebug;
            break;

        case 'D':
            pidFileOnly = -1;
            if (parseUInt(optarg,
                          &gOptions.mPacing_s) || 0 >= gOptions.mPacing_s)
                terminate(0, "Badly formed delay - '%s'", optarg);
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
            if (parseInt(optarg,
                         &gOptions.mTimeout_s) || 0 > gOptions.mTimeout_s)
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
