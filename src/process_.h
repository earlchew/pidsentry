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

#include <limits.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timespec;

struct BootClockTime;
struct Pipe;
struct ProcessAppLock;

struct ExitCode
{
    int mStatus;
};

enum ForkProcessOption
{
    ForkProcessShareProcessGroup,
    ForkProcessSetProcessGroup,
};

enum ProcessSigMaskAction
{
    ProcessSigMaskUnblock = -1,
    ProcessSigMaskSet     = 0,
    ProcessSigMaskBlock   = +1,
};

struct PushedProcessSigMask
{
    sigset_t mSigSet;
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
int
pushProcessSigMask(
    struct PushedProcessSigMask *self,
    enum ProcessSigMaskAction    aAction,
    const int                   *aSigList);

int
popProcessSigMask(struct PushedProcessSigMask *self);

/* -------------------------------------------------------------------------- */
int
watchProcessChildren(const struct Pipe *aTermPipe,
                     void               aSigAction(void *aSigObserver),
                     void              *aSigObserver);

int
unwatchProcessChildren(void);

int
watchProcessSignals(const struct Pipe *aSigPipe,
                    void               aSigAction(void *aSigObserver,
                                                  int   aSigNum),
                    void              *aSigObserver);

int
unwatchProcessSignals(void);

int
ignoreProcessSigPipe(void);

int
resetProcessSigPipe(void);

int
watchProcessClock(const struct Pipe *aClockPipe,
                  struct Duration    aClockPeriod);

int
unwatchProcessClock(void);

/* -------------------------------------------------------------------------- */
pid_t
forkProcess(enum ForkProcessOption aOption);

int
reapProcess(pid_t aPid, int *aStatus);

enum ProcessStatus
monitorProcess(pid_t aPid);

struct ExitCode
extractProcessExitStatus(int aStatus);

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
fetchProcessStartTime_(pid_t aPid, struct BootClockTime *aBootClockTime);

struct timespec
fetchProcessStartTime(pid_t aPid);

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
