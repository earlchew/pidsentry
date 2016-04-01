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
#include "pid_.h"

#include <limits.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timespec;

struct BootClockTime;
struct Pipe;
struct File;
struct ProcessAppLock;

#define PRId_ExitCode "d"
#define FMTd_ExitCode(ExitCode) ((ExitCode).mStatus)
struct ExitCode
{
    int mStatus;
};

enum ForkProcessOption
{
    ForkProcessShareProcessGroup,
    ForkProcessSetProcessGroup,
};

#define PRIs_ProcessState "c"
#define FMTs_ProcessState(State) ((State).mState)
struct ProcessState
{
    enum
    {
        ProcessStateError    = '#',
        ProcessStateRunning  = 'R',
        ProcessStateSleeping = 'S',
        ProcessStateWaiting  = 'W',
        ProcessStateZombie   = 'Z',
        ProcessStateStopped  = 'T',
        ProcessStateTraced   = 'D',
        ProcessStateDead     = 'X'
    } mState;
};

#define PRIs_ChildProcessState "c.%d"
#define FMTs_ChildProcessState(State) \
    ((State).mChildState), ((State).mChildState)
struct ChildProcessState
{
    enum
    {
        ChildProcessStateError     = '#',
        ChildProcessStateRunning   = 'r',
        ChildProcessStateExited    = 'x',
        ChildProcessStateKilled    = 'k',
        ChildProcessStateDumped    = 'd',
        ChildProcessStateStopped   = 's',
        ChildProcessStateTrapped   = 't',
    } mChildState;

    int mChildStatus;
};

/* -------------------------------------------------------------------------- */
struct ProcessSigContTracker
ProcessSigContTracker_(void);

struct ProcessSigContTracker
{
#ifdef __cplusplus
    ProcessSigContTracker()
    { *this = ProcessSigContTracker_(); }
#endif

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
#define PROCESS_DIRNAME_FMT_  "/proc/%" PRId_Pid

struct ProcessDirName
{
    char mDirName[sizeof(PROCESS_DIRNAME_FMT_) + sizeof(pid_t) * CHAR_BIT];
};

void
initProcessDirName(struct ProcessDirName *self, struct Pid aPid);

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
#ifndef __cplusplus
static inline struct ProcessSigContTracker
ProcessSigContTracker(void)
{
    return ProcessSigContTracker_();
}
#endif

bool
checkProcessSigContTracker(struct ProcessSigContTracker *self);

/* -------------------------------------------------------------------------- */
struct Pid
forkProcess(enum ForkProcessOption aOption, struct Pgid aPgid);

struct Pid
forkProcessDaemon(void);

int
reapProcess(struct Pid aPid, int *aStatus);

struct ChildProcessState
monitorProcessChild(struct Pid aPid);

struct ExitCode
extractProcessExitStatus(int aStatus, struct Pid aPid);

int
execProcess(const char *aCmd, char **aArgv);

void
exitProcess(int aStatus) __attribute__((__noreturn__));

void
abortProcess(void) __attribute__((__noreturn__));

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

const struct File *
ownProcessLockFile(void);

struct Duration
ownProcessElapsedTime(void);

struct MonotonicTime
ownProcessBaseTime(void);

const char*
ownProcessName(void);

struct Pid
ownProcessId(void);

struct Pgid
ownProcessGroupId(void);

/* -------------------------------------------------------------------------- */
int
fetchProcessSignature(struct Pid aPid, char **aSignature);

struct ProcessState
fetchProcessState(struct Pid aPid);

struct Pgid
fetchProcessGroupId(struct Pid aPid);

/* -------------------------------------------------------------------------- */
int
Process_init(const char *aArg0);

void
Process_exit(void);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* PROCESS_H */
