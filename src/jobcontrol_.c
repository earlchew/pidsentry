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

#include "jobcontrol_.h"

#include "error_.h"
#include "thread_.h"
#include "process_.h"

#include <sys/signal.h>

/* -------------------------------------------------------------------------- */
static void
reapJobControl_(struct JobControl *self)
{
    if ( ! ownJobControlMethodNil(self->mReap.mMethod))
        callJobControlMethod(self->mReap.mMethod);
}

/* -------------------------------------------------------------------------- */
static void
raiseJobControlSignal_(struct JobControl *self, int aSigNum)
{
    if ( ! ownJobControlSignalMethodNil(self->mRaise.mMethod))
        callJobControlSignalMethod(self->mRaise.mMethod, aSigNum);
}

/* -------------------------------------------------------------------------- */
static void
raiseJobControlSigStop_(struct JobControl *self)
{
    if ( ! ownJobControlMethodNil(self->mStop.mPauseMethod))
        callJobControlMethod(self->mStop.mPauseMethod);

    ABORT_IF(
        raise(SIGSTOP),
        {
            terminate(
                errno,
                "Unable to stop process pid %" PRId_Pid,
                FMTd_Pid(ownProcessId()));
        });

    if ( ! ownJobControlMethodNil(self->mStop.mResumeMethod))
        callJobControlMethod(self->mStop.mResumeMethod);
}

/* -------------------------------------------------------------------------- */
static void
raiseJobControlSigCont_(struct JobControl *self)
{
    if ( ! ownJobControlMethodNil(self->mContinue.mMethod))
        callJobControlMethod(self->mContinue.mMethod);
}

/* -------------------------------------------------------------------------- */
int
createJobControl(struct JobControl *self)
{
    int rc = -1;

    self->mRaise.mMethod      = JobControlSignalMethodNil();
    self->mReap.mMethod       = JobControlMethodNil();
    self->mStop.mPauseMethod  = JobControlMethodNil();
    self->mStop.mResumeMethod = JobControlMethodNil();
    self->mContinue.mMethod   = JobControlMethodNil();

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closeJobControl(struct JobControl *self)
{
    if (self)
    {
        ABORT_IF(unwatchProcessSigCont());
        ABORT_IF(unwatchProcessSigStop());
        ABORT_IF(unwatchProcessSignals());
        ABORT_IF(unwatchProcessChildren());
    }
}

/* -------------------------------------------------------------------------- */
int
watchJobControlSignals(struct JobControl            *self,
                       struct JobControlSignalMethod aRaiseMethod)
{
    int rc = -1;

    ERROR_IF(
        ownJobControlSignalMethodNil(aRaiseMethod),
        {
            errno = EINVAL;
        });

    ERROR_UNLESS(
        ownJobControlSignalMethodNil(self->mRaise.mMethod),
        {
            errno = EPERM;
        });

    self->mRaise.mMethod = aRaiseMethod;

    ERROR_IF(
        watchProcessSignals(VoidIntMethod(raiseJobControlSignal_, self)),
        {
            self->mRaise.mMethod = JobControlSignalMethodNil();
        });

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
unwatchJobControlSignals(struct JobControl *self)
{
    int rc = -1;

    ERROR_IF(
        ownJobControlSignalMethodNil(self->mRaise.mMethod),
        {
            errno = EPERM;
        });

    ERROR_IF(
        unwatchProcessSignals());

    self->mRaise.mMethod = JobControlSignalMethodNil();

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
watchJobControlDone(struct JobControl       *self,
                    struct JobControlMethod aReapMethod)
{
    int rc = -1;

    ERROR_IF(
        ownJobControlMethodNil(aReapMethod),
        {
            errno = EINVAL;
        });

    ERROR_UNLESS(
        ownJobControlMethodNil(self->mReap.mMethod),
        {
            errno = EPERM;
        });

    self->mReap.mMethod = aReapMethod;

    ERROR_IF(
        watchProcessChildren(VoidMethod(reapJobControl_, self)),
        {
            self->mReap.mMethod = JobControlMethodNil();
        });

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
unwatchJobControlDone(struct JobControl *self)
{
    int rc = -1;

    ERROR_IF(
        ownJobControlMethodNil(self->mReap.mMethod),
        {
            errno = EPERM;
        });

    ERROR_IF(
        unwatchProcessChildren());

    self->mReap.mMethod = JobControlMethodNil();

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
watchJobControlStop(struct JobControl      *self,
                    struct JobControlMethod aPauseMethod,
                    struct JobControlMethod aResumeMethod)
{
    int rc = -1;

    ERROR_IF(
        ownJobControlMethodNil(aPauseMethod) &&
        ownJobControlMethodNil(aResumeMethod),
        {
            errno = EINVAL;
        });

    ERROR_UNLESS(
        ownJobControlMethodNil(self->mStop.mPauseMethod) &&
        ownJobControlMethodNil(self->mStop.mResumeMethod),
        {
            errno = EPERM;
        });

    self->mStop.mPauseMethod  = aPauseMethod;
    self->mStop.mResumeMethod = aResumeMethod;

    ERROR_IF(
        watchProcessSigStop(VoidMethod(raiseJobControlSigStop_, self)),
        {
            self->mStop.mPauseMethod  = JobControlMethodNil();
            self->mStop.mResumeMethod = JobControlMethodNil();
        });

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
unwatchJobControlStop(struct JobControl *self)
{
    int rc = -1;

    ERROR_IF(
        ownJobControlMethodNil(self->mStop.mPauseMethod) &&
        ownJobControlMethodNil(self->mStop.mResumeMethod),
        {
            errno = EPERM;
        });

    ERROR_IF(
        unwatchProcessSigStop());

    self->mStop.mPauseMethod  = JobControlMethodNil();
    self->mStop.mResumeMethod = JobControlMethodNil();

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
watchJobControlContinue(struct JobControl      *self,
                        struct JobControlMethod aContinueMethod)
{
    int rc = -1;

    ERROR_IF(
        ownJobControlMethodNil(aContinueMethod),
        {
            errno = EINVAL;
        });

    ERROR_UNLESS(
        ownJobControlMethodNil(self->mContinue.mMethod),
        {
            errno = EPERM;
        });

    self->mContinue.mMethod  = aContinueMethod;

    ERROR_IF(
        watchProcessSigCont(VoidMethod(raiseJobControlSigCont_, self)),
        {
            self->mContinue.mMethod = JobControlMethodNil();
        });

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
unwatchJobControlContinue(struct JobControl *self)
{
    int rc = -1;

    ERROR_IF(
        ownJobControlMethodNil(self->mContinue.mMethod),
        {
            errno = EPERM;
        });

    ERROR_IF(
        unwatchProcessSigCont());

    self->mContinue.mMethod  = JobControlMethodNil();

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
