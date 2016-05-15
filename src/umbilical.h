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
#ifndef UMBILICAL_H
#define UMBILICAL_H

#include "int_.h"
#include "pollfd_.h"
#include "pid_.h"

#include <poll.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/un.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SocketPair;
struct PidFile;
struct ChildProcess;
struct PidServer;

/* -------------------------------------------------------------------------- */
struct UmbilicalProcess
{
    struct Pid           mPid;
    struct Pid           mChildAnchor;
    struct Pid           mWatchdogAnchor;
    struct Pid           mWatchdogPid;
    struct Pgid          mWatchdogPgid;
    struct ChildProcess *mChildProcess;
    struct SocketPair   *mSocket;
    struct PidServer    *mPidServer;
};

/* -------------------------------------------------------------------------- */
enum PollFdMonitorKind
{
    POLL_FD_MONITOR_UMBILICAL,
    POLL_FD_MONITOR_PIDSERVER,
    POLL_FD_MONITOR_PIDCLIENT,
    POLL_FD_MONITOR_KINDS
};

enum PollFdMonitorTimerKind
{
    POLL_FD_MONITOR_TIMER_UMBILICAL,
    POLL_FD_MONITOR_TIMER_KINDS
};

struct UmbilicalMonitor
{
    struct
    {
        struct File *mFile;
        unsigned     mCycleCount;
        unsigned     mCycleLimit;
        struct Pid   mParentPid;
        bool         mClosed;
    } mUmbilical;

    struct PidServer *mPidServer;

    struct pollfd             mPollFds[POLL_FD_MONITOR_KINDS];
    struct PollFdAction       mPollFdActions[POLL_FD_MONITOR_KINDS];
    struct PollFdTimerAction  mPollFdTimerActions[POLL_FD_MONITOR_TIMER_KINDS];
};

/* -------------------------------------------------------------------------- */
INT
createUmbilicalMonitor(
    struct UmbilicalMonitor *self,
    int                      aStdinFd,
    struct Pid               aParentPid,
    struct PidServer        *aPidServer);

INT
synchroniseUmbilicalMonitor(struct UmbilicalMonitor *self);

INT
runUmbilicalMonitor(struct UmbilicalMonitor *self);

bool
ownUmbilicalMonitorClosedOrderly(const struct UmbilicalMonitor *self);

/* -------------------------------------------------------------------------- */
INT
createUmbilicalProcess(struct UmbilicalProcess *self,
                       struct ChildProcess     *aChildProcess,
                       struct SocketPair       *aUmbilicalSocket,
                       struct PidServer        *aPidServer);

INT
stopUmbilicalProcess(struct UmbilicalProcess *self);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* UMBILICAL_H */
