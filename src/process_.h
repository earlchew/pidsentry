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
#ifndef PROCESS_H
#define PROCESS_H

#include "timescale_.h"
#include "method_.h"

#include <limits.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timespec;

struct BootClockTime;
struct Pipe;
struct ProcessAppLock;

struct Pid
{
    pid_t mPid;
};

struct ExitCode
{
    int mStatus;
};

enum ForkProcessOption
{
    ForkProcessShareProcessGroup,
    ForkProcessSetProcessGroup,
};

enum ProcessState
{
    ProcessStateError    = -1,
    ProcessStateRunning  = 'R',
    ProcessStateSleeping = 'S',
    ProcessStateWaiting  = 'W',
    ProcessStateZombie   = 'Z',
    ProcessStateStopped  = 'T',
    ProcessStateTraced   = 'D',
    ProcessStateDead     = 'X'
};

enum ProcessStatus
{
    ProcessStatusError     = -1,
    ProcessStatusRunning   = 'r',
    ProcessStatusExited    = 'x',
    ProcessStatusKilled    = 'k',
    ProcessStatusDumped    = 'd',
    ProcessStatusStopped   = 's',
    ProcessStatusTrapped   = 't',
};

/* -------------------------------------------------------------------------- */
struct ProcessSigContTracker
{
    unsigned mCount;
};

/* -------------------------------------------------------------------------- */
#define PROCESS_SIGNALNAME_FMT_ "signal %d"

struct ProcessSignalName
{
    char mSignalText_[sizeof(PROCESS_SIGNALNAME_FMT_) +
                      sizeof("-") +
                      sizeof(int) * CHAR_BIT];

    const char *mSignalName;
};

const char *
formatProcessSignalName(struct ProcessSignalName *self, int aSigNum);

/* -------------------------------------------------------------------------- */
#define PROCESS_DIRNAME_FMT_  "/proc/%jd"

struct ProcessDirName
{
    char mDirName[sizeof(PROCESS_DIRNAME_FMT_) + sizeof(pid_t) * CHAR_BIT];
};

void
initProcessDirName(struct ProcessDirName *self, pid_t aPid);

/* -------------------------------------------------------------------------- */
int
purgeProcessOrphanedFds(void);

/* -------------------------------------------------------------------------- */
unsigned
ownProcessSignalContext(void);

int
watchProcessChildren(struct VoidMethod aMethod);

int
unwatchProcessChildren(void);

int
watchProcessSignals(struct VoidIntMethod aMethod);

int
unwatchProcessSignals(void);

int
ignoreProcessSigPipe(void);

int
resetProcessSigPipe(void);

int
watchProcessSigCont(struct VoidMethod aMethod);

int
unwatchProcessSigCont(void);

int
watchProcessSigStop(struct VoidMethod aMethod);

int
unwatchProcessSigStop(void);

int
watchProcessClock(struct VoidMethod aMethod, struct Duration aPeriod);

int
unwatchProcessClock(void);

/* -------------------------------------------------------------------------- */
struct ProcessSigContTracker
ProcessSigContTracker(void);

bool
checkProcessSigContTracker(struct ProcessSigContTracker *self);

/* -------------------------------------------------------------------------- */
pid_t
forkProcess(enum ForkProcessOption aOption, pid_t aPgid);

int
reapProcess(pid_t aPid, int *aStatus);

enum ProcessStatus
monitorProcess(pid_t aPid);

struct ExitCode
extractProcessExitStatus(int aStatus);

void
execProcess(const char *aCmd, char **aArgv);

/* -------------------------------------------------------------------------- */
int
acquireProcessAppLock(void);

int
releaseProcessAppLock(void);

struct ProcessAppLock *
createProcessAppLock(void);

void
destroyProcessAppLock(struct ProcessAppLock *self);

/* -------------------------------------------------------------------------- */
const char *
ownProcessLockPath(void);

struct Duration
ownProcessElapsedTime(void);

struct MonotonicTime
ownProcessBaseTime(void);

const char*
ownProcessName(void);

/* -------------------------------------------------------------------------- */
int
fetchProcessSignature(pid_t aPid, char **aSignature);

enum ProcessState
fetchProcessState(pid_t aPid);

/* -------------------------------------------------------------------------- */
int
Process_init(const char *aArg0);

int
Process_exit(void);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* PROCESS_H */
