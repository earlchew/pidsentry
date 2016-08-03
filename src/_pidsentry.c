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
#include "process_.h"
#include "jobcontrol_.h"
#include "eintr_.h"

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
static int
cmdRunCommand(const char         *aPidFileName,
              const char * const *aCmd,
              struct ExitCode    *aExitCode)
{
    int rc = -1;

    struct ExitCode exitCode = { EXIT_FAILURE };

    struct Command  command_;
    struct Command *command = 0;

    enum CommandStatus status;
    ERROR_IF(
        (status = createCommand(&command_, aPidFileName),
         CommandStatusError == status));

    switch (status)
    {
    default:
        ensure(0);

    case CommandStatusOk:
        command = &command_;
        ERROR_IF(
            runCommand(command, aCmd));

        ERROR_IF(
            reapCommand(command, &exitCode));
        break;

    case CommandStatusUnreachablePidFile:
        message(0, "Unable to reach pidfile '%s'", aPidFileName);
        break;

    case CommandStatusNonexistentPidFile:
        message(0, "Unable to find pidfile '%s'", aPidFileName);
        break;

    case CommandStatusMalformedPidFile:
        message(0, "Malformed pidfile '%s'", aPidFileName);
        break;

    case CommandStatusInaccessiblePidFile:
        message(0, "Unable to access pidfile '%s'", aPidFileName);
        break;

    case CommandStatusZombiePidFile:
        message(0, "Dead process named in pidfile '%s'", aPidFileName);
        break;
    }

    *aExitCode = exitCode;

    rc = 0;

Finally:

    FINALLY
    ({
        command = closeCommand(command);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
cmdMonitorChild(const char * const *aCmd, struct ExitCode *aExitCode)
{
    int rc = -1;

    struct Agent  agent_;
    struct Agent *agent = 0;

    struct ExitCode exitCode = { EXIT_FAILURE };

    ensure(aCmd);

    debug(0,
          "watchdog process pid %" PRId_Pid " pgid %" PRId_Pgid,
          FMTd_Pid(ownProcessId()),
          FMTd_Pgid(ownProcessGroupId()));

    ERROR_IF(
        ignoreProcessSigPipe());

    ERROR_IF(
        createAgent(&agent_, aCmd));
    agent = &agent_;

    ERROR_IF(
        runAgent(agent, &exitCode));

    *aExitCode = exitCode;

    rc = 0;

Finally:

    FINALLY
    ({
        agent = closeAgent(agent);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
main(int argc, char **argv)
{
    struct ExitCode exitCode = { EXIT_FAILURE };

    struct EintrModule  eintrModule_;
    struct EintrModule *eintrModule = 0;

    struct TestModule  testModule_;
    struct TestModule *testModule = 0;

    struct TimeKeepingModule  timeKeepingModule_;
    struct TimeKeepingModule *timeKeepingModule = 0;

    struct ProcessModule  processModule_;
    struct ProcessModule *processModule = 0;

    ABORT_IF(
        Eintr_init(&eintrModule_));
    eintrModule = &eintrModule_;

    ABORT_IF(
        Test_init(&testModule_, "PIDSENTRY_TEST_ERROR"));
    testModule = &testModule_;

    ABORT_IF(
        Timekeeping_init(&timeKeepingModule_));
    timeKeepingModule = &timeKeepingModule_;

    ABORT_IF(
        Process_init(&processModule_, argv[0]));
    processModule = &processModule_;

    const char * const *args;
    ERROR_IF(
        processOptions(argc, argv, &args),
        {
            if (EINVAL != errno)
                message(errno,
                        "Unable to parse command line");
        });

    ensure(
        ( ! gOptions.mClient.mActive &&   gOptions.mServer.mActive ) ||
        (   gOptions.mClient.mActive && ! gOptions.mServer.mActive ) );

    if (gOptions.mClient.mActive)
        ABORT_IF(
            cmdRunCommand(gOptions.mClient.mPidFile, args, &exitCode),
            {
                terminate(errno,
                          "Failed to run command: %s", args[0]);
            });
    else
        ABORT_IF(
            cmdMonitorChild(args, &exitCode),
            {
                terminate(errno,
                          "Failed to supervise program: %s", args[0]);
            });

Finally:

    processModule     = Process_exit(processModule);
    timeKeepingModule = Timekeeping_exit(timeKeepingModule);

    if (testMode(TestLevelError))
        dprintf(STDERR_FILENO, "%" PRIu64 "\n", testErrorLevel());

    testModule = Test_exit(testModule);
    eintrModule = Eintr_exit(eintrModule);

    return exitCode.mStatus;
}

/* -------------------------------------------------------------------------- */
