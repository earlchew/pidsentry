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

#include "options_.h"

#include "ert/pollfd.h"
#include "ert/jobcontrol.h"

#include <stdlib.h>
#include <unistd.h>


/* TODO
 *
 * On receiving SIGABRT, trigger gdb
 * Dump /proc/../task/stack after SIGSTOP, just before delivering SIGABRT
 */

/* -------------------------------------------------------------------------- */
static int
cmdRunCommand(const char          *aPidFileName,
              const char * const  *aCmd,
              struct Ert_ExitCode *aExitCode)
{
    int rc = -1;

    struct Ert_ExitCode exitCode = { EXIT_FAILURE };

    struct Command  command_;
    struct Command *command = 0;

    enum CommandStatus status;
    ERT_ERROR_IF(
        (status = createCommand(&command_, aPidFileName),
         CommandStatusError == status));

    switch (status)
    {
    default:
        ert_ensure(0);

    case CommandStatusOk:
        command = &command_;
        ERT_ERROR_IF(
            runCommand(command, aCmd));

        ERT_ERROR_IF(
            reapCommand(command, &exitCode));
        break;

    case CommandStatusUnreachablePidFile:
        ert_message(0, "Unable to reach pidfile '%s'", aPidFileName);
        break;

    case CommandStatusNonexistentPidFile:
        ert_message(0, "Unable to find pidfile '%s'", aPidFileName);
        break;

    case CommandStatusMalformedPidFile:
        ert_message(0, "Malformed pidfile '%s'", aPidFileName);
        break;

    case CommandStatusInaccessiblePidFile:
        ert_message(0, "Unable to access pidfile '%s'", aPidFileName);
        break;

    case CommandStatusZombiePidFile:
        ert_message(0, "Dead process named in pidfile '%s'", aPidFileName);
        break;
    }

    *aExitCode = exitCode;

    rc = 0;

Ert_Finally:

    ERT_FINALLY
    ({
        command = closeCommand(command);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
cmdMonitorChild(const char * const *aCmd, struct Ert_ExitCode *aExitCode)
{
    int rc = -1;

    struct Agent  agent_;
    struct Agent *agent = 0;

    struct Ert_ExitCode exitCode = { EXIT_FAILURE };

    ert_ensure(aCmd);

    ert_debug(
        0,
        "watchdog process pid %" PRId_Ert_Pid " pgid %" PRId_Ert_Pgid,
        FMTd_Ert_Pid(ert_ownProcessId()),
        FMTd_Ert_Pgid(ert_ownProcessGroupId()));

    ERT_ERROR_IF(
        ert_ignoreProcessSigPipe());

    ERT_ERROR_IF(
        createAgent(&agent_, aCmd));
    agent = &agent_;

    ERT_ERROR_IF(
        runAgent(agent, &exitCode));

    ERT_ERROR_IF(
        ert_resetProcessSigPipe());

    *aExitCode = exitCode;

    rc = 0;

Ert_Finally:

    ERT_FINALLY
    ({
        agent = closeAgent(agent);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
main(int argc, char **argv)
{
    struct Ert_ExitCode exitCode = { EXIT_FAILURE };

    struct Ert_TestModule  testModule_;
    struct Ert_TestModule *testModule = 0;

    struct Ert_TimeKeepingModule  timeKeepingModule_;
    struct Ert_TimeKeepingModule *timeKeepingModule = 0;

    struct Ert_ProcessModule  processModule_;
    struct Ert_ProcessModule *processModule = 0;

    ERT_ABORT_IF(
        Ert_Test_init(&testModule_, "PIDSENTRY_TEST_ERROR"));
    testModule = &testModule_;

    ERT_ABORT_IF(
        Ert_Timekeeping_init(&timeKeepingModule_));
    timeKeepingModule = &timeKeepingModule_;

    ERT_ABORT_IF(
        Ert_Process_init(&processModule_, argv[0]));
    processModule = &processModule_;

    const char * const *args;
    ERT_ERROR_IF(
        processOptions(argc, argv, &args),
        {
            if (EINVAL != errno)
                ert_message(errno,
                        "Unable to parse command line");
        });

    ert_ensure(
        ( ! gOptions.mClient.mActive &&   gOptions.mServer.mActive ) ||
        (   gOptions.mClient.mActive && ! gOptions.mServer.mActive ) );

    if (gOptions.mClient.mActive)
        ERT_ABORT_IF(
            cmdRunCommand(gOptions.mClient.mPidFile, args, &exitCode),
            {
                ert_terminate(errno,
                          "Failed to run command: %s", args[0]);
            });
    else
        ERT_ABORT_IF(
            cmdMonitorChild(args, &exitCode),
            {
                ert_terminate(errno,
                          "Failed to supervise program: %s", args[0]);
            });

Ert_Finally:

    ERT_FINALLY({});

    processModule     = Ert_Process_exit(processModule);
    timeKeepingModule = Ert_Timekeeping_exit(timeKeepingModule);

    if (ert_testMode(Ert_TestLevelError))
    {
        /* Wait for all the outstanding children to ensure that stderr is
         * not polluted for output as those processes are torn down. */

        while (1)
        {
            struct Ert_Pid pid;

            ERT_ABORT_IF(
                (pid = ert_waitProcessChildren(),
                 -1 == pid.mPid),
                {
                    ert_terminate(errno,
                              "Failed to wait for child processes in test");
                });

            if ( ! pid.mPid)
                break;

            int status;
            ERT_ABORT_IF(
                ert_reapProcessChild(pid, &status),
                {
                    ert_terminate(
                        errno,
                        "Unable to reap "
                        "child process %" PRId_Ert_Pid " in test",
                        FMTd_Ert_Pid(pid));
                });

            (void) ert_extractProcessExitStatus(status, pid);
        }

        dprintf(STDERR_FILENO, "%" PRIu64 "\n", ert_testErrorLevel());
    }

    testModule = Ert_Test_exit(testModule);

    return exitCode.mStatus;
}

/* -------------------------------------------------------------------------- */
