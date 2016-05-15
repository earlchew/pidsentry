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
static int
reapJobControl_(struct JobControl *self)
{
    int rc = -1;

    if ( ! ownIntMethodNil(self->mReap.mMethod))
        ERROR_IF(
            callIntMethod(self->mReap.mMethod));

    rc = 0;

Finally:

    FINALLY({});

    return 0;
}

/* -------------------------------------------------------------------------- */
static int
raiseJobControlSignal_(struct JobControl *self, int aSigNum)
{
    int rc = -1;

    if ( ! ownIntIntMethodNil(self->mRaise.mMethod))
        ERROR_IF(
            callIntIntMethod(self->mRaise.mMethod, aSigNum));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
raiseJobControlSigStop_(struct JobControl *self)
{
    int rc = -1;

    if ( ! ownIntMethodNil(self->mStop.mPauseMethod))
        ERROR_IF(
            callIntMethod(self->mStop.mPauseMethod));

    ERROR_IF(
        raise(SIGSTOP),
        {
            warn(
                errno,
                "Unable to stop process pid %" PRId_Pid,
                FMTd_Pid(ownProcessId()));
        });

    if ( ! ownIntMethodNil(self->mStop.mResumeMethod))
        ERROR_IF(
            callIntMethod(self->mStop.mResumeMethod));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
raiseJobControlSigCont_(struct JobControl *self)
{
    int rc = -1;

    if ( ! ownIntMethodNil(self->mContinue.mMethod))
        ERROR_IF(
            callIntMethod(self->mContinue.mMethod));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
createJobControl(struct JobControl *self)
{
    int rc = -1;

    self->mRaise.mMethod      = IntIntMethodNil();
    self->mReap.mMethod       = IntMethodNil();
    self->mStop.mPauseMethod  = IntMethodNil();
    self->mStop.mResumeMethod = IntMethodNil();
    self->mContinue.mMethod   = IntMethodNil();

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
watchJobControlSignals(struct JobControl  *self,
                       struct IntIntMethod aRaiseMethod)
{
    int rc = -1;

    ERROR_IF(
        ownIntIntMethodNil(aRaiseMethod),
        {
            errno = EINVAL;
        });

    ERROR_UNLESS(
        ownIntIntMethodNil(self->mRaise.mMethod),
        {
            errno = EPERM;
        });

    self->mRaise.mMethod = aRaiseMethod;

    ERROR_IF(
        watchProcessSignals(IntIntMethod(raiseJobControlSignal_, self)),
        {
            self->mRaise.mMethod = IntIntMethodNil();
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
        ownIntIntMethodNil(self->mRaise.mMethod),
        {
            errno = EPERM;
        });

    ERROR_IF(
        unwatchProcessSignals());

    self->mRaise.mMethod = IntIntMethodNil();

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
watchJobControlDone(struct JobControl *self,
                    struct IntMethod   aReapMethod)
{
    int rc = -1;

    ERROR_IF(
        ownIntMethodNil(aReapMethod),
        {
            errno = EINVAL;
        });

    ERROR_UNLESS(
        ownIntMethodNil(self->mReap.mMethod),
        {
            errno = EPERM;
        });

    self->mReap.mMethod = aReapMethod;

    ERROR_IF(
        watchProcessChildren(IntMethod(reapJobControl_, self)),
        {
            self->mReap.mMethod = IntMethodNil();
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
        ownIntMethodNil(self->mReap.mMethod),
        {
            errno = EPERM;
        });

    ERROR_IF(
        unwatchProcessChildren());

    self->mReap.mMethod = IntMethodNil();

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
watchJobControlStop(struct JobControl *self,
                    struct IntMethod   aPauseMethod,
                    struct IntMethod   aResumeMethod)
{
    int rc = -1;

    ERROR_IF(
        ownIntMethodNil(aPauseMethod) &&
        ownIntMethodNil(aResumeMethod),
        {
            errno = EINVAL;
        });

    ERROR_UNLESS(
        ownIntMethodNil(self->mStop.mPauseMethod) &&
        ownIntMethodNil(self->mStop.mResumeMethod),
        {
            errno = EPERM;
        });

    self->mStop.mPauseMethod  = aPauseMethod;
    self->mStop.mResumeMethod = aResumeMethod;

    ERROR_IF(
        watchProcessSigStop(IntMethod(raiseJobControlSigStop_, self)),
        {
            self->mStop.mPauseMethod  = IntMethodNil();
            self->mStop.mResumeMethod = IntMethodNil();
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
        ownIntMethodNil(self->mStop.mPauseMethod) &&
        ownIntMethodNil(self->mStop.mResumeMethod),
        {
            errno = EPERM;
        });

    ERROR_IF(
        unwatchProcessSigStop());

    self->mStop.mPauseMethod  = IntMethodNil();
    self->mStop.mResumeMethod = IntMethodNil();

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
watchJobControlContinue(struct JobControl *self,
                        struct IntMethod   aContinueMethod)
{
    int rc = -1;

    ERROR_IF(
        ownIntMethodNil(aContinueMethod),
        {
            errno = EINVAL;
        });

    ERROR_UNLESS(
        ownIntMethodNil(self->mContinue.mMethod),
        {
            errno = EPERM;
        });

    self->mContinue.mMethod  = aContinueMethod;

    ERROR_IF(
        watchProcessSigCont(IntMethod(raiseJobControlSigCont_, self)),
        {
            self->mContinue.mMethod = IntMethodNil();
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
        ownIntMethodNil(self->mContinue.mMethod),
        {
            errno = EPERM;
        });

    ERROR_IF(
        unwatchProcessSigCont());

    self->mContinue.mMethod  = IntMethodNil();

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
