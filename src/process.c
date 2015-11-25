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

#include "process.h"
#include "macros.h"
#include "pathname.h"
#include "file.h"
#include "test.h"
#include "error.h"
#include "timespec.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include <sys/file.h>
#include <sys/wait.h>

struct ProcessLock
{
    struct PathName        mPathName_;
    struct PathName       *mPathName;
    struct FileDescriptor  mFile_;
    struct FileDescriptor *mFile;
    int                    mLock;
};

static struct ProcessLock  sProcessLock_[2];
static struct ProcessLock *sProcessLock[2];
static unsigned            sActiveProcessLock;

static unsigned    sSigContext;
static const char *sArg0;
static uint64_t    sTimeBase;

/* -------------------------------------------------------------------------- */
void
initProcessDirName(struct ProcessDirName *self, pid_t aPid)
{
    sprintf(self->mDirName, PROCESS_DIRNAME_FMT_, (intmax_t) aPid);
}

/* -------------------------------------------------------------------------- */
static uint64_t
monotonicTime_(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        terminate(
            errno,
            "Unable to fetch monotonic time");

    uint64_t ns = ts.tv_sec;

    return ns * 1000 * 1000 * 1000 + ts.tv_nsec;
}

/* -------------------------------------------------------------------------- */
struct timespec
findProcessStartTime(pid_t aPid)
{
    struct ProcessDirName processDirName;

    initProcessDirName(&processDirName, aPid);

    struct timespec startTime = { 0 };

    struct stat procStatus;

    if (stat(processDirName.mDirName, &procStatus))
        startTime.tv_nsec = ENOENT == errno ?  UTIME_NOW : UTIME_OMIT;
    else
        startTime = earliestTime(&procStatus.st_mtim, &procStatus.st_ctim);

    return startTime;
}

/* -------------------------------------------------------------------------- */
static int
createProcessLock_(struct ProcessLock *self)
{
    int rc = -1;

    self->mFile     = 0;
    self->mLock     = LOCK_UN;
    self->mPathName = 0;

    if (createPathName(&self->mPathName_, "/proc/self"))
        goto Finally;
    self->mPathName = &self->mPathName_;

    if (createFileDescriptor(
            &self->mFile_,
            openPathName(self->mPathName, O_RDONLY | O_CLOEXEC, 0)))
        goto Finally;
    self->mFile = &self->mFile_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            closeFileDescriptor(self->mFile);
            closePathName(self->mPathName);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
closeProcessLock_(struct ProcessLock *self)
{
    int rc = -1;

    if (self)
    {
        if (closeFileDescriptor(self->mFile))
            goto Finally;

        if (closePathName(self->mPathName))
            goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
lockProcessLock_(struct ProcessLock *self)
{
    int rc = -1;

    if (self)
    {
        ensure(LOCK_UN == self->mLock);

        if (flock(self->mFile->mFd, LOCK_EX))
            goto Finally;

        self->mLock = LOCK_EX;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
unlockProcessLock_(struct ProcessLock *self)
{
    int rc = -1;

    if (self)
    {
        ensure(LOCK_UN != self->mLock);

        if (flock(self->mFile->mFd, LOCK_UN))
            goto Finally;

        self->mLock = LOCK_UN;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
initProcess(const char *aArg0)
{
    ensure( ! sProcessLock[sActiveProcessLock]);

    int rc = -1;

    sArg0     = aArg0;
    sTimeBase = monotonicTime_();

    srandom(getpid());

    if (createProcessLock_(&sProcessLock_[sActiveProcessLock]))
        goto Finally;
    sProcessLock[sActiveProcessLock] = &sProcessLock_[sActiveProcessLock];

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
exitProcess(void)
{
    struct ProcessLock *processLock = sProcessLock[sActiveProcessLock];

    ensure(processLock);

    int rc = -1;

    if (closeProcessLock_(processLock))
        goto Finally;
    sProcessLock[sActiveProcessLock] = 0;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
lockProcessLock(void)
{
    int rc = -1;

    if ( ! sSigContext)
    {
        struct ProcessLock *processLock = sProcessLock[sActiveProcessLock];

        if (processLock && lockProcessLock_(processLock))
            goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
unlockProcessLock(void)
{
    int rc = -1;

    if ( ! sSigContext)
    {
        struct ProcessLock *processLock = sProcessLock[sActiveProcessLock];

        if (processLock && unlockProcessLock_(processLock))
            goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
pid_t
forkProcess(enum ForkProcessOption aOption)
{
    ensure(
        sProcessLock[sActiveProcessLock] == &sProcessLock_[sActiveProcessLock]);

    pid_t rc = -1;

    /* The child process needs separate process lock. It cannot share
     * the process lock with the parent because flock(2) distinguishes
     * locks by file descriptor table entry. Create the process lock
     * in the parent first so that the child process is guaranteed to
     * be able to synchronise its messages. */

    unsigned activeProcessLock   = 0 + sActiveProcessLock;
    unsigned inactiveProcessLock = 1 - activeProcessLock;

    ensure(NUMBEROF(sProcessLock_) > activeProcessLock);
    ensure(NUMBEROF(sProcessLock_) > inactiveProcessLock);

    ensure( ! sProcessLock[inactiveProcessLock]);

    if (createProcessLock_(&sProcessLock_[inactiveProcessLock]))
        goto Finally;
    sProcessLock[inactiveProcessLock] = &sProcessLock_[inactiveProcessLock];

    /* Note that the fork() will complete and launch the child process
     * before the child pid is recorded in the local variable. This
     * is an important consideration for propagating signals to
     * the child process. */

    pid_t childPid;

    RACE
    ({
        childPid = fork();
    });

    switch (childPid)
    {
    default:
        /* Forcibly set the process group of the child to avoid
         * the race that would occur if only the child attempts
         * to set its own process group */

        if (ForkProcessSetProcessGroup == aOption)
        {
            if (setpgid(childPid, childPid))
                goto Finally;
        }
        break;

    case -1:
        break;

    case 0:
        /* Switch the process lock first in case the child process
         * needs to emit diagnostic messages so that the messages
         * will not be garbled. */

        sActiveProcessLock  = inactiveProcessLock;
        inactiveProcessLock = activeProcessLock;

        if (ForkProcessSetProcessGroup == aOption)
        {
            if (setpgid(0, 0))
                terminate(
                    errno,
                    "Unable to set process group");
        }
        break;
    }

    rc = childPid;

Finally:

    FINALLY
    ({
        if (closeProcessLock_(sProcessLock[inactiveProcessLock]))
            terminate(
                errno,
                "Unable to close process lock");
        sProcessLock[inactiveProcessLock] = 0;
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
ownProcessName(void)
{
    return sArg0;
}

/* -------------------------------------------------------------------------- */
struct ExitCode
extractProcessExitStatus(int aStatus)
{
    /* Taking guidance from OpenGroup:
     *
     * http://pubs.opengroup.org/onlinepubs/009695399/
     *      utilities/xcu_chap02.html#tag_02_08_02
     *
     * Use exit codes above 128 to indicate signals, and codes below
     * 128 to indicate exit status. */

    struct ExitCode exitCode = { 255 };

    if (WIFEXITED(aStatus))
    {
        exitCode.mStatus = WEXITSTATUS(aStatus);
        if (128 < exitCode.mStatus)
            exitCode.mStatus = 128;
    }
    else if (WIFSIGNALED(aStatus))
    {
        exitCode.mStatus = 128 + WTERMSIG(aStatus);
        if (255 < exitCode.mStatus)
            exitCode.mStatus = 255;
    }

    return exitCode;
}

/* -------------------------------------------------------------------------- */
uint64_t
ownProcessElapsedTime(void)
{
    return monotonicTime_() - sTimeBase;
}

/* -------------------------------------------------------------------------- */
void
initProcessSignalHandler(void)
{
    ++sSigContext;
}

/* -------------------------------------------------------------------------- */
void
exitProcessSignalHandler(void)
{
    --sSigContext;
}

/* -------------------------------------------------------------------------- */
