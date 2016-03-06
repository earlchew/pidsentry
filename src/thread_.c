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
    ABORT_IF(
        (errno = pthread_create(self, aAttr, aThread, aContext)),
        {
            terminate(
                errno,
                "Unable to create thread");
        });
}

/* -------------------------------------------------------------------------- */
void *
joinThread(pthread_t *self)
{
    void *rc = 0;

    ABORT_IF(
        (errno = pthread_join(*self, &rc)),
        {
            terminate(
                errno,
                "Unable to join thread");
        });

    return rc;
}

/* -------------------------------------------------------------------------- */
void
createThreadAttr(pthread_attr_t *self)
{
    ABORT_IF(
        (errno = pthread_attr_init(self)),
        {
            terminate(
                errno,
                "Unable to create thread attribute");
        });
}

/* -------------------------------------------------------------------------- */
void
setThreadAttrDetachState(pthread_attr_t *self, int aState)
{
    ABORT_IF(
        (errno = pthread_attr_setdetachstate(self, aState)),
        {
            terminate(
                errno,
                "Unable to configure thread attribute detached state %d",
                aState);
        });
}

/* -------------------------------------------------------------------------- */
void
destroyThreadAttr(pthread_attr_t *self)
{
    ABORT_IF(
        (errno = pthread_attr_destroy(self)),
        {
            terminate(
                errno,
                "Unable to destroy thread attribute");
        });
}

/* -------------------------------------------------------------------------- */
pthread_mutex_t *
createMutex(pthread_mutex_t *self)
{
    ABORT_IF(
        (errno = pthread_mutex_init(self, 0)),
        {
            terminate(
                errno,
                "Unable to create mutex");
        });

    return self;
}

/* -------------------------------------------------------------------------- */
pthread_mutex_t *
createSharedMutex(pthread_mutex_t *self)
{
    pthread_mutexattr_t mutexattr;

    ABORT_IF(
        (errno = pthread_mutexattr_init(&mutexattr)),
        {
            terminate(
                errno,
                "Unable to allocate mutex attribute");
        });

    ABORT_IF(
        (errno = pthread_mutexattr_setpshared(&mutexattr,
                                              PTHREAD_PROCESS_SHARED)),
        {
            terminate(
                errno,
                "Unable to set mutex attribute PTHREAD_PROCESS_SHARED");
        });

    ABORT_IF(
        (errno = pthread_mutex_init(self, &mutexattr)),
        {
            terminate(
                errno,
                "Unable to create shared mutex");
        });

    ABORT_IF(
        (errno = pthread_mutexattr_destroy(&mutexattr)),
        {
            terminate(
                errno,
                "Unable to destroy mutex attribute");
        });

    return self;
}

/* -------------------------------------------------------------------------- */
pthread_mutex_t *
destroyMutex(pthread_mutex_t *self)
{
    if (self)
    {
        ABORT_IF(
            (errno = pthread_mutex_destroy(self)),
            {
                terminate(
                    errno,
                    "Unable to destroy mutex");
            });
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
static pthread_mutex_t *
lockMutex_(pthread_mutex_t *self)
{
    struct ProcessSigContTracker sigContTracker = ProcessSigContTracker();

    while (1)
    {
        ABORT_IF(
            (errno = pthread_mutex_trylock(self),
             errno && EBUSY != errno),
            {
                terminate(errno, "Unable to lock mutex");
            });

        if ( ! errno)
            break;

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

        ABORT_IF(
            (errno = pthread_mutex_timedlock(self, &deadline),
             errno && ETIMEDOUT != errno),
            {
                terminate(errno, "Unable to lock mutex after %us", timeout_s);
            });

        if (errno)
        {
            /* Try again if the attempt to lock the mutex timed out
             * but the process was stopped for some part of that time. */

            if (checkProcessSigContTracker(&sigContTracker))
                continue;
        }

        break;
    }

    return self;
}

pthread_mutex_t *
lockMutex(pthread_mutex_t *self)
{
    ensure( ! ownProcessSignalContext());

    return lockMutex_(self);
}

/* -------------------------------------------------------------------------- */
static pthread_mutex_t *
unlockMutex_(pthread_mutex_t *self)
{
    if (self)
    {
        ABORT_IF(
            (errno = pthread_mutex_unlock(self)),
            {
                terminate(errno, "Unable to unlock mutex");
            });
    }

    return 0;
}

pthread_mutex_t *
unlockMutex(pthread_mutex_t *self)
{
    ensure( ! ownProcessSignalContext());

    return unlockMutex_(self);
}

/* -------------------------------------------------------------------------- */
static pthread_mutex_t *
unlockMutexSignal_(pthread_mutex_t *self, pthread_cond_t *aCond)
{
    if (self)
    {
        ABORT_IF(
            (errno = pthread_cond_signal(aCond)),
            {
                terminate(
                    errno,
                    "Unable to signal to condition variable");
            });

        ABORT_IF(
            (errno = pthread_mutex_unlock(self)),
            {
                terminate(
                    errno,
                    "Unable to lock mutex");
            });
    }

    return 0;
}

pthread_mutex_t *
unlockMutexSignal(pthread_mutex_t *self, pthread_cond_t *aCond)
{
    ensure( ! ownProcessSignalContext());

    return unlockMutexSignal_(self, aCond);
}

/* -------------------------------------------------------------------------- */
static pthread_mutex_t *
unlockMutexBroadcast_(pthread_mutex_t *self, pthread_cond_t *aCond)
{
    if (self)
    {
        ABORT_IF(
            (errno = pthread_cond_broadcast(aCond)),
            {
                terminate(
                    errno,
                    "Unable to broadcast to condition variable");
            });

        ABORT_IF(
            (errno = pthread_mutex_unlock(self)),
            {
                terminate(
                    errno,
                    "Unable to lock mutex");
            });
    }

    return 0;
}

pthread_mutex_t *
unlockMutexBroadcast(pthread_mutex_t *self, pthread_cond_t *aCond)
{
    ensure( ! ownProcessSignalContext());

    return unlockMutexBroadcast_(self, aCond);
}

/* -------------------------------------------------------------------------- */
pthread_cond_t *
createCond(pthread_cond_t *self)
{
    ABORT_IF(
        (errno = pthread_cond_init(self, 0)),
        {
            terminate(
                errno,
                "Unable to create condition variable");
        });

    return self;
}

/* -------------------------------------------------------------------------- */
pthread_cond_t *
createSharedCond(pthread_cond_t *self)
{
    pthread_condattr_t condattr;

    ABORT_IF(
        (errno = pthread_condattr_init(&condattr)),
        {
            terminate(
                errno,
                "Unable to allocate condition variable attribute");
        });

    ABORT_IF(
        (errno = pthread_condattr_setpshared(&condattr,
                                             PTHREAD_PROCESS_SHARED)),
        {
            terminate(
                errno,
                "Unable to set cond attribute PTHREAD_PROCESS_SHARED");
        });

    ABORT_IF(
        (errno = pthread_cond_init(self, &condattr)),
        {
            terminate(
                errno,
                "Unable to create shared condition variable");
        });

    ABORT_IF(
        (errno = pthread_condattr_destroy(&condattr)),
        {
            terminate(
                errno,
                "Unable to destroy condition variable attribute");
        });

    return self;
}

/* -------------------------------------------------------------------------- */
pthread_cond_t *
destroyCond(pthread_cond_t *self)
{
    if (self)
    {
        ABORT_IF(
            (errno = pthread_cond_destroy(self)),
            {
                terminate(
                    errno,
                    "Unable to destroy condition variable");
            });
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
static void
waitCond_(pthread_cond_t *self, pthread_mutex_t *aMutex)
{
    ABORT_IF(
        (errno = pthread_cond_wait(self, aMutex)),
        {
            terminate(
                errno,
                "Unable to wait for condition variable");
        });
}

void
waitCond(pthread_cond_t *self, pthread_mutex_t *aMutex)
{
    ensure( ! ownProcessSignalContext());

    waitCond_(self, aMutex);
}

/* -------------------------------------------------------------------------- */
struct ThreadSigMask *
pushThreadSigMask(
    struct ThreadSigMask     *self,
    enum ThreadSigMaskAction  aAction,
    const int                *aSigList)
{
    int maskAction;
    switch (aAction)
    {
    default:
        ABORT_IF(
            true,
            {
                errno = EINVAL;
            });
    case ThreadSigMaskUnblock: maskAction = SIG_UNBLOCK; break;
    case ThreadSigMaskSet:     maskAction = SIG_SETMASK; break;
    case ThreadSigMaskBlock:   maskAction = SIG_BLOCK;   break;
    }

    sigset_t sigSet;

    if ( ! aSigList)
        ABORT_IF(sigfillset(&sigSet));
    else
    {
        ABORT_IF(sigemptyset(&sigSet));
        for (size_t ix = 0; aSigList[ix]; ++ix)
            ABORT_IF(sigaddset(&sigSet, aSigList[ix]));
    }

    ABORT_IF(pthread_sigmask(maskAction, &sigSet, &self->mSigSet));

    return self;
}

/* -------------------------------------------------------------------------- */
struct ThreadSigMask *
popThreadSigMask(struct ThreadSigMask *self)
{
    if (self)
        ABORT_IF(pthread_sigmask(SIG_SETMASK, &self->mSigSet, 0));

    return 0;
}

/* -------------------------------------------------------------------------- */
struct ThreadSigMutex *
createThreadSigMutex(struct ThreadSigMutex *self)
{
    createMutex(&self->mMutex);
    createCond(&self->mCond);

    self->mLocked = 0;

    return self;
}

/* -------------------------------------------------------------------------- */
struct ThreadSigMutex *
destroyThreadSigMutex(struct ThreadSigMutex *self)
{
    if (self)
    {
        ensure( ! self->mLocked);

        destroyCond(&self->mCond);
        destroyMutex(&self->mMutex);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
struct ThreadSigMutex *
lockThreadSigMutex(struct ThreadSigMutex *self)
{
    /* When acquiring the lock, first ensure that no signal is delivered
     * within the context of this thread, and only then lock the mutex
     * to prevent other threads accessing the resource. */

    struct ThreadSigMask threadSigMask;
    pushThreadSigMask(&threadSigMask, ThreadSigMaskBlock, 0);

    lockMutex_(&self->mMutex);

    if (self->mLocked && ! pthread_equal(self->mOwner, pthread_self()))
    {
        do
            waitCond_(&self->mCond, &self->mMutex);
        while (self->mLocked);
    }

    if (1 == ++self->mLocked)
    {
        self->mMask  = threadSigMask;
        self->mOwner = pthread_self();
    }

    unlockMutex_(&self->mMutex);

    ensure(self->mLocked);
    ensure(pthread_equal(self->mOwner, pthread_self()));

    if (1 != self->mLocked)
        popThreadSigMask(&threadSigMask);

    return self;
}

/* -------------------------------------------------------------------------- */
struct ThreadSigMutex *
unlockThreadSigMutex(struct ThreadSigMutex *self)
{
    if (self)
    {
        ensure(self->mLocked);
        ensure(pthread_equal(self->mOwner, pthread_self()));

        lockMutex_(&self->mMutex);

        unsigned             locked        = --self->mLocked;
        struct ThreadSigMask threadSigMask = self->mMask;

        if (locked)
            unlockMutex_(&self->mMutex);
        else
        {
            unlockMutexSignal_(&self->mMutex, &self->mCond);

            popThreadSigMask(&threadSigMask);
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
unsigned
ownThreadSigMutexLocked(struct ThreadSigMutex *self)
{
    unsigned locked;

    lockMutex_(&self->mMutex);

    locked = self->mLocked;

    if (locked && ! pthread_equal(self->mOwner, pthread_self()))
        locked = 0;

    unlockMutex_(&self->mMutex);

    return locked;
}

/* -------------------------------------------------------------------------- */
