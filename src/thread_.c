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

#include "thread_.h"
#include "error_.h"
#include "timekeeping_.h"
#include "macros_.h"
#include "process_.h"

#include <errno.h>

/* -------------------------------------------------------------------------- */
void
createThread(pthread_t      *self,
             pthread_attr_t *aAttr,
             void           *aThread(void *),
             void           *aContext)
{
    if (errno = pthread_create(self, aAttr, aThread, aContext))
        terminate(
            errno,
            "Unable to create thread");
}

/* -------------------------------------------------------------------------- */
void *
joinThread(pthread_t *self)
{
    void *rc = 0;

    if (errno = pthread_join(*self, &rc))
        terminate(
            errno,
            "Unable to join thread");

    return rc;
}

/* -------------------------------------------------------------------------- */
void
createThreadAttr(pthread_attr_t *self)
{
    if (errno = pthread_attr_init(self))
        terminate(
            errno,
            "Unable to create thread attribute");
}

/* -------------------------------------------------------------------------- */
void
setThreadAttrDetachState(pthread_attr_t *self, int aState)
{
    if (errno = pthread_attr_setdetachstate(self, aState))
        terminate(
            errno,
            "Unable to configure thread attribute detached state %d", aState);
}

/* -------------------------------------------------------------------------- */
void
destroyThreadAttr(pthread_attr_t *self)
{
    if (errno = pthread_attr_destroy(self))
        terminate(
            errno,
            "Unable to destroy thread attribute");
}

/* -------------------------------------------------------------------------- */
void
createSharedMutex(pthread_mutex_t *self)
{
    pthread_mutexattr_t mutexattr;

    if (errno = pthread_mutexattr_init(&mutexattr))
        terminate(
            errno,
            "Unable to allocate mutex attribute");

    if (errno = pthread_mutexattr_setpshared(&mutexattr,
                                             PTHREAD_PROCESS_SHARED))
        terminate(
            errno,
            "Unable to set mutex attribute PTHREAD_PROCESS_SHARED");

    if (errno = pthread_mutex_init(self, &mutexattr))
        terminate(
            errno,
            "Unable to create shared mutex");

    if (errno = pthread_mutexattr_destroy(&mutexattr))
        terminate(
            errno,
            "Unable to destroy mutex attribute");
}

/* -------------------------------------------------------------------------- */
void
destroyMutex(pthread_mutex_t *self)
{
    if (errno = pthread_mutex_destroy(self))
        terminate(
            errno,
            "Unable to destroy mutex");
}

/* -------------------------------------------------------------------------- */
pthread_mutex_t *
lockMutex(pthread_mutex_t *self)
{
    while (errno = pthread_mutex_trylock(self))
    {
        if (EBUSY != errno)
            terminate(errno, "Unable to lock mutex");

        unsigned sigContCount = ownProcessSigContCount();

        /* There is no way to configure the mutex to use a monotonic
         * clock to compute the deadline. Since the timeout is only
         * important on the error path, this is not a critical problem
         * in this use case. */

        const unsigned timeout_s = 600;

        struct WallClockTime tm = wallclockTime();

        struct timespec deadline =
            timeSpecFromNanoSeconds(
                NanoSeconds(
                    tm.wallclock.ns + NSECS(Seconds(timeout_s * 60)).ns));

        if (errno = pthread_mutex_timedlock(self, &deadline))
        {
            int err = errno;

            if (ETIMEDOUT == err)
            {
                /* Try again if the attempt to lock the mutex timed out
                 * but the process was stopped for some part of that time. */

                if (ownProcessSigContCount() != sigContCount)
                    continue;
            }

            terminate(err, "Unable to lock mutex after %us", timeout_s);
        }

        break;
    }

    return self;
}

/* -------------------------------------------------------------------------- */
void
unlockMutex(pthread_mutex_t *self)
{
    if (self)
    {
        if (errno = pthread_mutex_unlock(self))
            terminate(errno, "Unable to unlock mutex");
    }
}

/* -------------------------------------------------------------------------- */
void
unlockMutexSignal(pthread_mutex_t *self, pthread_cond_t *aCond)
{
    if (errno = pthread_cond_signal(aCond))
        terminate(
            errno,
            "Unable to signal to condition variable");

    if (errno = pthread_mutex_unlock(self))
        terminate(
            errno,
            "Unable to lock mutex");
}

/* -------------------------------------------------------------------------- */
void
unlockMutexBroadcast(pthread_mutex_t *self, pthread_cond_t *aCond)
{
    if (errno = pthread_cond_broadcast(aCond))
        terminate(
            errno,
            "Unable to broadcast to condition variable");

    if (errno = pthread_mutex_unlock(self))
        terminate(
            errno,
            "Unable to lock mutex");
}

/* -------------------------------------------------------------------------- */
void
createSharedCond(pthread_cond_t *self)
{
    pthread_condattr_t condattr;

    if (errno = pthread_condattr_init(&condattr))
        terminate(
            errno,
            "Unable to allocate condition variable attribute");

    if (errno = pthread_condattr_setpshared(&condattr,
                                            PTHREAD_PROCESS_SHARED))
        terminate(
            errno,
            "Unable to set cond attribute PTHREAD_PROCESS_SHARED");

    if (errno = pthread_cond_init(self, &condattr))
        terminate(
            errno,
            "Unable to create shared condition variable");

    if (errno = pthread_condattr_destroy(&condattr))
        terminate(
            errno,
            "Unable to destroy condition variable attribute");
}

/* -------------------------------------------------------------------------- */
void
destroyCond(pthread_cond_t *self)
{
    if (errno = pthread_cond_destroy(self))
        terminate(
            errno,
            "Unable to destroy condition variable");
}

/* -------------------------------------------------------------------------- */
void
waitCond(pthread_cond_t *self, pthread_mutex_t *aMutex)
{
    if (errno = pthread_cond_wait(self, aMutex))
        terminate(
            errno,
            "Unable to wait for condition variable");
}

/* -------------------------------------------------------------------------- */
int
pushThreadSigMask(
    struct ThreadSigMask     *self,
    enum ThreadSigMaskAction  aAction,
    const int                *aSigList)
{
    int rc = -1;

    int maskAction;
    switch (aAction)
    {
    default: errno = EINVAL; goto Finally;
    case ThreadSigMaskUnblock: maskAction = SIG_UNBLOCK; break;
    case ThreadSigMaskSet:     maskAction = SIG_SETMASK; break;
    case ThreadSigMaskBlock:   maskAction = SIG_BLOCK;   break;
    }

    sigset_t sigset;

    if ( ! aSigList)
    {
        if (sigfillset(&sigset))
            goto Finally;
    }
    else
    {
        if (sigemptyset(&sigset))
            goto Finally;
        for (size_t ix = 0; aSigList[ix]; ++ix)
        {
            if (sigaddset(&sigset, aSigList[ix]))
                goto Finally;
        }
    }

    if (pthread_sigmask(maskAction, &sigset, &self->mSigSet))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
popThreadSigMask(struct ThreadSigMask *self)
{
    int rc = -1;

    if (self)
    {
        if (pthread_sigmask(SIG_SETMASK, &self->mSigSet, 0))
            goto Finally;
    }

    rc = 0;

Finally:

    return rc;
}

/* -------------------------------------------------------------------------- */
void
createThreadSigMutex(struct ThreadSigMutex *self)
{
    *self = (struct ThreadSigMutex) THREAD_SIG_MUTEX_INITIALIZER;
}

/* -------------------------------------------------------------------------- */
void
destroyThreadSigMutex(struct ThreadSigMutex *self)
{
    ensure( ! self->mLocked);
}

/* -------------------------------------------------------------------------- */
struct ThreadSigMutex *
lockThreadSigMutex(struct ThreadSigMutex *self)
{
    /* When acquiring the lock, first ensure that no signal is delivered
     * within the context of this thread, and only then lock the mutex
     * to prevent other threads accessing the resource. */

    struct ThreadSigMask threadSigMask;

    if (pushThreadSigMask(&threadSigMask, ThreadSigMaskBlock, 0))
        terminate(
            errno,
            "Unable to push thread signal mask");

    lockMutex(&self->mMutex);

    self->mLocked = true;
    self->mMask   = threadSigMask;
    self->mOwner  = pthread_self();

    return self;
}

/* -------------------------------------------------------------------------- */
void
unlockThreadSigMutex(struct ThreadSigMutex *self)
{
    if (self)
    {
        ensure(self->mLocked);
        ensure(pthread_equal(self->mOwner, pthread_self()));

        struct ThreadSigMask threadSigMask = self->mMask;

        self->mLocked = false;
        unlockMutex(&self->mMutex);

        if (popThreadSigMask(&threadSigMask))
            terminate(
                errno,
                "Unable to pop thread signal mask");
    }
}

/* -------------------------------------------------------------------------- */
