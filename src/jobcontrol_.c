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
#include "type_.h"
#include "thread_.h"

#include <sys/signal.h>

static const struct Type * const jobControlType_ = TYPE("JobControl");

/* -------------------------------------------------------------------------- */
static void
reapJobControl_(void *self_)
{
    struct JobControl *self = self_;

    ensure(jobControlType_ == self->mType);

    lockMutex(&self->mReap.mMutex);
    if ( ! ownVoidMethodNil(self->mReap.mMethod))
        callVoidMethod(self->mReap.mMethod);
    unlockMutex(&self->mReap.mMutex);
}

/* -------------------------------------------------------------------------- */
static void
raiseJobControlSignal_(void *self_, int aSigNum)
{
    struct JobControl *self = self_;

    ensure(jobControlType_ == self->mType);

    lockMutex(&self->mRaise.mMutex);
    if ( ! ownVoidIntMethodNil(self->mRaise.mMethod))
        callVoidIntMethod(self->mRaise.mMethod, aSigNum);
    unlockMutex(&self->mRaise.mMutex);
}

/* -------------------------------------------------------------------------- */
static void
raiseJobControlSigStop_(void *self_)
{
    struct JobControl *self = self_;

    ensure(jobControlType_ == self->mType);

    lockMutex(&self->mStop.mMutex);
    if ( ! ownVoidMethodNil(self->mStop.mPauseMethod))
        callVoidMethod(self->mStop.mPauseMethod);

    ABORT_IF(
        raise(SIGSTOP),
        {
            terminate(
                errno,
                "Unable to stop process pid %" PRId_Pid,
                FMTd_Pid(ownProcessId()));
        });

    if ( ! ownVoidMethodNil(self->mStop.mResumeMethod))
        callVoidMethod(self->mStop.mResumeMethod);
    unlockMutex(&self->mStop.mMutex);
}

/* -------------------------------------------------------------------------- */
static void
raiseJobControlSigCont_(void *self_)
{
    struct JobControl *self = self_;

    ensure(jobControlType_ == self->mType);

    lockMutex(&self->mContinue.mMutex);
    if ( ! ownVoidMethodNil(self->mContinue.mMethod))
        callVoidMethod(self->mContinue.mMethod);
    unlockMutex(&self->mContinue.mMutex);
}

/* -------------------------------------------------------------------------- */
int
createJobControl(struct JobControl *self)
{
    int rc = -1;

    self->mType = jobControlType_;

    createMutex(&self->mRaise.mMutex);
    self->mRaise.mMethod  = VoidIntMethod(0, 0);

    createMutex(&self->mReap.mMutex);
    self->mReap.mMethod = VoidMethod(0, 0);

    createMutex(&self->mStop.mMutex);
    self->mStop.mPauseMethod  = VoidMethod(0, 0);
    self->mStop.mResumeMethod = VoidMethod(0, 0);

    createMutex(&self->mContinue.mMutex);
    self->mContinue.mMethod = VoidMethod(0, 0);

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            destroyMutex(&self->mContinue.mMutex);
            destroyMutex(&self->mStop.mMutex);
            destroyMutex(&self->mReap.mMutex);
            destroyMutex(&self->mRaise.mMutex);
        }
    });

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

        destroyMutex(&self->mContinue.mMutex);
        destroyMutex(&self->mStop.mMutex);
        destroyMutex(&self->mReap.mMutex);
        destroyMutex(&self->mRaise.mMutex);
    }
}

/* -------------------------------------------------------------------------- */
int
watchJobControlSignals(struct JobControl   *self,
                       struct VoidIntMethod aRaiseMethod)
{
    int rc = -1;

    pthread_mutex_t *lock = 0;

    ERROR_IF(
        ownVoidIntMethodNil(aRaiseMethod),
        {
            errno = EINVAL;
        });

    lock = lockMutex(&self->mRaise.mMutex);

    ERROR_IF(
        watchProcessSignals(VoidIntMethod(raiseJobControlSignal_, self)));

    self->mRaise.mMethod = aRaiseMethod;

    rc = 0;

Finally:

    FINALLY
    ({
        unlockMutex(lock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
unwatchJobControlSignals(struct JobControl *self)
{
    int rc = -1;

    lockMutex(&self->mRaise.mMutex);

    ERROR_IF(
        ownVoidIntMethodNil(self->mRaise.mMethod),
        {
            errno = EINVAL;
        });

    ERROR_IF(
        unwatchProcessSignals());

    self->mRaise.mMethod = VoidIntMethod(0, 0);

    rc = 0;

Finally:

    FINALLY
    ({
        unlockMutex(&self->mRaise.mMutex);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
watchJobControlDone(struct JobControl *self,
                    struct VoidMethod  aReapMethod)
{
    int rc = -1;

    pthread_mutex_t *lock = 0;

    ERROR_IF(
        ownVoidMethodNil(aReapMethod),
        {
            errno = EINVAL;
        });

    lock = lockMutex(&self->mReap.mMutex);

    ERROR_IF(
        watchProcessChildren(VoidMethod(reapJobControl_, self)));

    self->mReap.mMethod = aReapMethod;

    rc = 0;

Finally:

    FINALLY
    ({
        unlockMutex(lock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
unwatchJobControlDone(struct JobControl *self)
{
    int rc = -1;

    lockMutex(&self->mReap.mMutex);

    ERROR_IF(
        ownVoidMethodNil(self->mReap.mMethod),
        {
            errno = EINVAL;
        });

    ERROR_IF(
        unwatchProcessChildren());

    self->mReap.mMethod = VoidMethod(0, 0);

    rc = 0;

Finally:

    FINALLY
    ({
        unlockMutex(&self->mReap.mMutex);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
watchJobControlStop(struct JobControl *self,
                    struct VoidMethod  aPauseMethod,
                    struct VoidMethod  aResumeMethod)
{
    int rc = -1;

    pthread_mutex_t *lock = 0;

    ERROR_IF(
        ownVoidMethodNil(aPauseMethod) &&
        ownVoidMethodNil(aResumeMethod),
        {
            errno = EINVAL;
        });

    lock = lockMutex(&self->mStop.mMutex);

    ERROR_IF(
        watchProcessSigStop(VoidMethod(raiseJobControlSigStop_, self)));

    self->mStop.mPauseMethod  = aPauseMethod;
    self->mStop.mResumeMethod = aResumeMethod;

    rc = 0;

Finally:

    FINALLY
    ({
        unlockMutex(lock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
unwatchJobControlStop(struct JobControl *self)
{
    int rc = -1;

    lockMutex(&self->mStop.mMutex);

    ERROR_IF(
        ownVoidMethodNil(self->mStop.mPauseMethod) &&
        ownVoidMethodNil(self->mStop.mResumeMethod),
        {
            errno = EINVAL;
        });

    ERROR_IF(
        unwatchProcessSigStop());

    self->mStop.mPauseMethod  = VoidMethod(0, 0);
    self->mStop.mResumeMethod = VoidMethod(0, 0);

    rc = 0;

Finally:

    FINALLY
    ({
        unlockMutex(&self->mStop.mMutex);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
watchJobControlContinue(struct JobControl *self,
                        struct VoidMethod  aContinueMethod)
{
    int rc = -1;

    pthread_mutex_t *lock = 0;

    ERROR_IF(
        ownVoidMethodNil(aContinueMethod),
        {
            errno = EINVAL;
        });

    lock = lockMutex(&self->mContinue.mMutex);

    ERROR_IF(
        watchProcessSigCont(VoidMethod(raiseJobControlSigCont_, self)));

    self->mContinue.mMethod  = aContinueMethod;

    rc = 0;

Finally:

    FINALLY
    ({
        unlockMutex(lock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
unwatchJobControlContinue(struct JobControl *self)
{
    int rc = -1;

    lockMutex(&self->mContinue.mMutex);

    ERROR_IF(
        ownVoidMethodNil(self->mContinue.mMethod),
        {
            errno = EINVAL;
        });

    ERROR_IF(
        unwatchProcessSigCont());

    self->mContinue.mMethod  = VoidMethod(0, 0);

    rc = 0;

Finally:

    FINALLY
    ({
        unlockMutex(&self->mContinue.mMutex);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
