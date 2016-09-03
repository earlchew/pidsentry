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

#include "compiler_.h"
#include "timescale_.h"
#include "method_.h"
#include "pid_.h"
#include "uid_.h"
#include "error_.h"
#include "method_.h"

#include <limits.h>

#include <sys/types.h>

/* -------------------------------------------------------------------------- */
BEGIN_C_SCOPE;

struct FdSet;

struct PreForkProcess
{
    struct FdSet *mBlacklistFds;
    struct FdSet *mWhitelistFds;
};

END_C_SCOPE;

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_PreForkProcessMethod    int
#define METHOD_CONST_PreForkProcessMethod
#define METHOD_ARG_LIST_PreForkProcessMethod  (const \
                                               struct PreForkProcess *aPreFork_)
#define METHOD_CALL_LIST_PreForkProcessMethod (aPreFork_)

#define METHOD_NAME      PreForkProcessMethod
#define METHOD_RETURN    METHOD_RETURN_PreForkProcessMethod
#define METHOD_CONST     METHOD_CONST_PreForkProcessMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_PreForkProcessMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_PreForkProcessMethod
#include "method_.h"

#define PreForkProcessMethod(Method_, Object_)     \
    METHOD_TRAMPOLINE(                             \
        Method_, Object_,                          \
        PreForkProcessMethod_,                     \
        METHOD_RETURN_PreForkProcessMethod,        \
        METHOD_CONST_PreForkProcessMethod,         \
        METHOD_ARG_LIST_PreForkProcessMethod,      \
        METHOD_CALL_LIST_PreForkProcessMethod)

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_PostForkParentProcessMethod    int
#define METHOD_CONST_PostForkParentProcessMethod
#define METHOD_ARG_LIST_PostForkParentProcessMethod  (struct Pid aChildPid_)
#define METHOD_CALL_LIST_PostForkParentProcessMethod (aChildPid_)

#define METHOD_NAME      PostForkParentProcessMethod
#define METHOD_RETURN    METHOD_RETURN_PostForkParentProcessMethod
#define METHOD_CONST     METHOD_CONST_PostForkParentProcessMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_PostForkParentProcessMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_PostForkParentProcessMethod
#include "method_.h"

#define PostForkParentProcessMethod(Method_, Object_)     \
    METHOD_TRAMPOLINE(                                    \
        Method_, Object_,                                 \
        PostForkParentProcessMethod_,                     \
        METHOD_RETURN_PostForkParentProcessMethod,        \
        METHOD_CONST_PostForkParentProcessMethod,         \
        METHOD_ARG_LIST_PostForkParentProcessMethod,      \
        METHOD_CALL_LIST_PostForkParentProcessMethod)

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_PostForkChildProcessMethod    int
#define METHOD_CONST_PostForkChildProcessMethod
#define METHOD_ARG_LIST_PostForkChildProcessMethod  ()
#define METHOD_CALL_LIST_PostForkChildProcessMethod ()

#define METHOD_NAME      PostForkChildProcessMethod
#define METHOD_RETURN    METHOD_RETURN_PostForkChildProcessMethod
#define METHOD_CONST     METHOD_CONST_PostForkChildProcessMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_PostForkChildProcessMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_PostForkChildProcessMethod
#include "method_.h"

#define PostForkChildProcessMethod(Method_, Object_)     \
    METHOD_TRAMPOLINE(                                   \
        Method_, Object_,                                \
        PostForkChildProcessMethod_,                     \
        METHOD_RETURN_PostForkChildProcessMethod,        \
        METHOD_CONST_PostForkChildProcessMethod,         \
        METHOD_ARG_LIST_PostForkChildProcessMethod,      \
        METHOD_CALL_LIST_PostForkChildProcessMethod)

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_ForkProcessMethod    int
#define METHOD_CONST_ForkProcessMethod
#define METHOD_ARG_LIST_ForkProcessMethod  ()
#define METHOD_CALL_LIST_ForkProcessMethod ()

#define METHOD_NAME      ForkProcessMethod
#define METHOD_RETURN    METHOD_RETURN_ForkProcessMethod
#define METHOD_CONST     METHOD_CONST_ForkProcessMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_ForkProcessMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_ForkProcessMethod
#include "method_.h"

#define ForkProcessMethod(Method_, Object_)     \
    METHOD_TRAMPOLINE(                          \
        Method_, Object_,                       \
        ForkProcessMethod_,                     \
        METHOD_RETURN_ForkProcessMethod,        \
        METHOD_CONST_ForkProcessMethod,         \
        METHOD_ARG_LIST_ForkProcessMethod,      \
        METHOD_CALL_LIST_ForkProcessMethod)

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_WatchProcessMethod    int
#define METHOD_CONST_WatchProcessMethod
#define METHOD_ARG_LIST_WatchProcessMethod  ()
#define METHOD_CALL_LIST_WatchProcessMethod ()

#define METHOD_NAME      WatchProcessMethod
#define METHOD_RETURN    METHOD_RETURN_WatchProcessMethod
#define METHOD_CONST     METHOD_CONST_WatchProcessMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_WatchProcessMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_WatchProcessMethod
#include "method_.h"

#define WatchProcessMethod(Method_, Object_)    \
    METHOD_TRAMPOLINE(                          \
        Method_, Object_,                       \
        WatchProcessMethod_,                    \
        METHOD_RETURN_WatchProcessMethod,       \
        METHOD_CONST_WatchProcessMethod,        \
        METHOD_ARG_LIST_WatchProcessMethod,     \
        METHOD_CALL_LIST_WatchProcessMethod)

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_WatchProcessSignalMethod    int
#define METHOD_CONST_WatchProcessSignalMethod
#define METHOD_ARG_LIST_WatchProcessSignalMethod  (int aSigNum_,     \
                                                   struct Pid aPid_, \
                                                   struct Uid aUid_)
#define METHOD_CALL_LIST_WatchProcessSignalMethod (aSigNum_, aPid_, aUid_)

#define METHOD_NAME      WatchProcessSignalMethod
#define METHOD_RETURN    METHOD_RETURN_WatchProcessSignalMethod
#define METHOD_CONST     METHOD_CONST_WatchProcessSignalMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_WatchProcessSignalMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_WatchProcessSignalMethod
#include "method_.h"

#define WatchProcessSignalMethod(Method_, Object_)      \
    METHOD_TRAMPOLINE(                                  \
        Method_, Object_,                               \
        WatchProcessSignalMethod_,                      \
        METHOD_RETURN_WatchProcessSignalMethod,         \
        METHOD_CONST_WatchProcessSignalMethod,          \
        METHOD_ARG_LIST_WatchProcessSignalMethod,       \
        METHOD_CALL_LIST_WatchProcessSignalMethod)

/* -------------------------------------------------------------------------- */
BEGIN_C_SCOPE;

struct timespec;

struct BootClockTime;
struct Pipe;
struct File;
struct ProcessAppLock;

struct ProcessModule
{
    struct ProcessModule *mModule;

    struct ErrorModule  mErrorModule_;
    struct ErrorModule *mErrorModule;
};

#define PRId_ExitCode "d"
#define FMTd_ExitCode(ExitCode) ((ExitCode).mStatus)
struct ExitCode
{
    int mStatus;
};

enum ForkProcessOption
{
    ForkProcessInheritProcessGroup,
    ForkProcessSetProcessGroup,
    ForkProcessSetSessionLeader,
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

struct Program
{
    struct ExitCode mExitCode;
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

CHECKED int
initProcessDirName(struct ProcessDirName *self, struct Pid aPid);

/* -------------------------------------------------------------------------- */
CHECKED int
purgeProcessOrphanedFds(void);

/* -------------------------------------------------------------------------- */
unsigned
ownProcessSignalContext(void);

CHECKED int
watchProcessChildren(struct WatchProcessMethod aMethod);

CHECKED int
unwatchProcessChildren(void);

CHECKED int
watchProcessSignals(struct WatchProcessSignalMethod aMethod);

CHECKED int
unwatchProcessSignals(void);

CHECKED int
ignoreProcessSigPipe(void);

CHECKED int
resetProcessSigPipe(void);

CHECKED int
watchProcessSigCont(struct WatchProcessMethod aMethod);

CHECKED int
unwatchProcessSigCont(void);

CHECKED int
watchProcessSigStop(struct WatchProcessMethod aMethod);

CHECKED int
unwatchProcessSigStop(void);

CHECKED int
watchProcessClock(struct WatchProcessMethod aMethod, struct Duration aPeriod);

CHECKED int
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
CHECKED struct Pid
forkProcessChildX(enum ForkProcessOption             aOption,
                  struct Pgid                        aPgid,
                  struct PreForkProcessMethod        aPreForkMethod,
                  struct PostForkChildProcessMethod  aPostForkChildMethod,
                  struct PostForkParentProcessMethod aPostForkParentMethod,
                  struct ForkProcessMethod           aMethod);

CHECKED struct Pid
forkProcessChild(enum ForkProcessOption   aOption,
                 struct Pgid              aPgid,
                 struct ForkProcessMethod aMethod);

CHECKED struct Pid
forkProcessDaemon(struct ForkProcessMethod aMethod);

CHECKED int
reapProcessChild(struct Pid aPid, int *aStatus);

CHECKED struct ChildProcessState
waitProcessChild(struct Pid aPid);

CHECKED struct ChildProcessState
monitorProcessChild(struct Pid aPid);

struct ExitCode
extractProcessExitStatus(int aStatus, struct Pid aPid);

void
execProcess(const char *aCmd, const char * const *aArgv);

void
execShell(const char *aCmd);

CHECKED int
signalProcessGroup(struct Pgid aPgid, int aSignal);

void
exitProcess(int aStatus) NORETURN;

void
abortProcess(void) NORETURN;

void
quitProcess(void) NORETURN;

/* -------------------------------------------------------------------------- */
CHECKED int
acquireProcessAppLock(void);

CHECKED int
releaseProcessAppLock(void);

CHECKED struct ProcessAppLock *
createProcessAppLock(void);

CHECKED struct ProcessAppLock *
destroyProcessAppLock(struct ProcessAppLock *self);

unsigned
ownProcessAppLockCount(void);

const struct File *
ownProcessAppLockFile(const struct ProcessAppLock *self);

/* -------------------------------------------------------------------------- */
struct Duration
ownProcessElapsedTime(void);

struct MonotonicTime
ownProcessBaseTime(void);

const char*
ownProcessName(void);

struct Pid
ownProcessParentId(void);

struct Pid
ownProcessId(void);

struct Pgid
ownProcessGroupId(void);

/* -------------------------------------------------------------------------- */
struct ProcessState
fetchProcessState(struct Pid aPid);

struct Pgid
fetchProcessGroupId(struct Pid aPid);

/* -------------------------------------------------------------------------- */
CHECKED int
Process_init(struct ProcessModule *self, const char *aArg0);

CHECKED struct ProcessModule *
Process_exit(struct ProcessModule *self);

/* -------------------------------------------------------------------------- */

END_C_SCOPE;

#endif /* PROCESS_H */
