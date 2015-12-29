/* -*- c-basic-offset:4; indent-tabs-mode:nil -*- vi: set sw=4 et: */
/*
// Copyright (c) 2013, Earl Chew
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

#include <inttypes.h>
#include <sys/types.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timespec;

struct Pipe;

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
watchProcessChildren(const struct Pipe *aTermPipe);

int
unwatchProcessChildren(void);

int
watchProcessSignals(const struct Pipe *aSigPipe);

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

/* -------------------------------------------------------------------------- */
int
lockProcessLock(void);

int
unlockProcessLock(void);

const char *
ownProcessLockPath(void);

struct Duration
ownProcessElapsedTime(void);

struct MonotonicTime
ownProcessBaseTime(void);

const char*
ownProcessName(void);

struct timespec
findProcessStartTime(pid_t aPid);

struct ExitCode
extractProcessExitStatus(int aStatus);

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
