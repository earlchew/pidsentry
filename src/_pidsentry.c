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

#include "_pidsentry.h"

#include "agent.h"
#include "command.h"

#include "env_.h"
#include "macros_.h"
#include "pipe_.h"
#include "socketpair_.h"
#include "bellsocketpair_.h"
#include "unixsocket_.h"
#include "stdfdfiller_.h"
#include "pidfile_.h"
#include "thread_.h"
#include "error_.h"
#include "pollfd_.h"
#include "test_.h"
#include "fd_.h"
#include "dl_.h"
#include "type_.h"
#include "process_.h"
#include "jobcontrol_.h"

#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/un.h>

/* TODO
 *
 * On receiving SIGABRT, trigger gdb
 * Dump /proc/../task/stack after SIGSTOP, just before delivering SIGABRT
 */

/* -------------------------------------------------------------------------- */
static struct ExitCode
cmdRunCommand(const char *aPidFileName, char **aCmd)
{
    struct ExitCode exitCode = { EXIT_FAILURE };

    struct Command  command_;
    struct Command *command = 0;
    ERROR_IF(
        createCommand(&command_, aPidFileName));
    command = &command_;

    ERROR_IF(
        runCommand(command, aCmd));

    ERROR_IF(
        reapCommand(command, &exitCode));

Finally:

    FINALLY
    ({
        closeCommand(command);
    });

    return exitCode;
}

/* -------------------------------------------------------------------------- */
static struct ExitCode
cmdMonitorChild(char **aCmd)
{
    struct ExitCode exitCode = { EXIT_FAILURE };

    ensure(aCmd);

    debug(0,
          "watchdog process pid %" PRId_Pid " pgid %" PRId_Pgid,
          FMTd_Pid(ownProcessId()),
          FMTd_Pgid(ownProcessGroupId()));

    ABORT_IF(
        ignoreProcessSigPipe(),
        {
            terminate(
                errno,
                "Unable to ignore SIGPIPE");
        });

    struct Agent  agent_;
    struct Agent *agent = 0;

    ERROR_IF(
        createAgent(&agent_, aCmd));
    agent = &agent_;

    ERROR_IF(
        runAgent(agent, &exitCode));

Finally:

    FINALLY
    ({
        closeAgent(agent);
    });

    return exitCode;
}

/* -------------------------------------------------------------------------- */
static int
main_(void *self, int argc, char **argv)
{
    /* This code will run in a slave thread with signal delivery masked,
     * leaving the main thread to handle delivery of signals. This is
     * primarily to work around behaviour in valgrind (Bug 361615). */

    struct ExitCode exitCode = { EXIT_FAILURE };

    char **args;

    ERROR_IF(
        processOptions(argc, argv, &args),
        {
            if (EINVAL != errno)
                message(errno,
                        "Unable to parse command line");
        });

    if (gOptions.mCommand)
        exitCode = cmdRunCommand(gOptions.mPidFile, args);
    else
        exitCode = cmdMonitorChild(args);

Finally:

    FINALLY({});

    return exitCode.mStatus;
}

int
main(int argc, char **argv)
{
    struct ExitCode exitCode = { EXIT_FAILURE };

    struct Program  program_;
    struct Program *program = 0;

    ABORT_IF(
        Test_init("PIDSENTRY_TEST_ERROR"),
        {
            terminate(
                0,
                "Unable to initialise test module");
        });

    ABORT_IF(
        Timekeeping_init(),
        {
            terminate(
                0,
                "Unable to initialise timekeeping module");
        });

    ABORT_IF(
        Process_init(&program_,
                     IntIntCharPtrPtrMethod(main_, 0),
                     argv[0],
                     argc,
                     argv),
        {
            terminate(
                errno,
                "Unable to initialise process state");
        });
    program = &program_;

    exitCode = program->mExitCode;

Finally:

    Process_exit(program);

    Timekeeping_exit();

    if (testMode(TestLevelError))
        dprintf(STDERR_FILENO, "%" PRIu64 "\n", testErrorLevel());
    Test_exit();

    return exitCode.mStatus;
}

/* -------------------------------------------------------------------------- */
