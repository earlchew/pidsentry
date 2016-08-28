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
#include "pid_.h"
#include "eintr_.h"

#include <errno.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/syscall.h>

#ifdef __GLIBC__
#if __GLIBC__ < 2 || __GLIBC__ == 2 && __GLIBC_MINOR < 12
#define PTHREAD_MUTEX_ROBUST        PTHREAD_MUTEX_ROBUST_NP
#define pthread_mutexattr_setrobust pthread_mutexattr_setrobust_np
#define pthread_mutex_consistent    pthread_mutex_consistent_np
#endif
#endif

/* -------------------------------------------------------------------------- */
static void *
timedLock_(void *aLock,
           int   aTryLock(void *),
           int   aTimedLock(void *, const struct timespec *))
{
    struct ProcessSigContTracker sigContTracker = ProcessSigContTracker();

    while (1)
    {
        ABORT_IF(
            (errno = aTryLock(aLock),
             errno && EBUSY != errno && EOWNERDEAD != errno),
            {
                terminate(errno, "Unable to acquire lock");
            });

        if (EBUSY != errno)
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
            (errno = aTimedLock(aLock, &deadline),
             errno && ETIMEDOUT != errno && EOWNERDEAD != errno),
            {
                terminate(errno,
                          "Unable to acquire lock after %us", timeout_s);
            });

        if (ETIMEDOUT == errno)
        {
            /* Try again if the attempt to lock the mutex timed out
             * but the process was stopped for some part of that time. */

            if (checkProcessSigContTracker(&sigContTracker))
                continue;
        }

        break;
    }

    return errno ? 0 : aLock;
}

/* -------------------------------------------------------------------------- */
struct Tid
ownThreadId(void)
{
    return Tid(syscall(SYS_gettid));
}

/* -------------------------------------------------------------------------- */
struct ThreadFuture_
{
    int mStatus;
};

static struct ThreadFuture_ *
createThreadFuture_(void)
{
    struct ThreadFuture_ *self = malloc(sizeof(*self));

    if (self)
        self->mStatus = -1;

    return self;
}

static void
destroyThreadFuture_(void *self_)
{
    free(self_);
}

/* -------------------------------------------------------------------------- */
struct Thread *
closeThread(struct Thread *self)
{
    if (self)
    {
        /* If the thread has not yet been joined, join it now and include
         * cancellation as a failure condition. The caller will join
         * explicitly if cancellation is to be benign. */

        if ( ! self->mJoined)
        {
            ABORT_IF(
                joinThread(self),
                {
                    terminate("Unable to join thread");
                });
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
struct Thread_
{
    int mDetached;

    pthread_mutex_t mMutex;
    pthread_cond_t  mCond;

    struct ThreadMethod mMethod;

    struct ThreadFuture_ *mFuture;
};

static void *
createThread_(void *self_)
{
    struct Thread_ *self = self_;

    struct ThreadMethod method = self->mMethod;

    struct ThreadFuture_ *future = self->mFuture;

    int detached = self->mDetached;

    pthread_mutex_t *lock = lockMutex(&self->mMutex);
    lock = unlockMutexSignal(lock, &self->mCond);

    /* Do not reference self beyond this point because the parent
     * will have deallocated the struct Thread_ instance. Also do
     * not rely on a struct Thread instance because a detached thread
     * will not have one. */

    pthread_cleanup_push(destroyThreadFuture_, future);
    {
        int rc = -1;

        ABORT_IF(
            (rc = callThreadMethod(method),
             -1 == rc));

        if ( ! detached)
            future->mStatus = rc;
        else
        {
            /* If a thread is detached, its parent will not join to recover
             * the thread future, so deallocate the thread future here. */

            destroyThreadFuture_(future);
            future = 0;
        }
    }
    pthread_cleanup_pop(0);

    /* The registered cleanup handler is popped without being executed.
     * If cancelled, the cleanup handler will be called, and the caller
     * will receive PTHREAD_CANCELED. */

    return future;
}

struct Thread *
createThread(
    struct Thread           *self,
    const struct ThreadAttr *aAttr,
    struct ThreadMethod      aMethod)
{
    /* The caller can specify a null self to create a detached thread.
     * Detached threads are not owned by the parent, and the parent will
     * not join or close the detached thread. */

    struct Thread_ thread =
    {
        .mDetached = ! self,

        .mMutex = PTHREAD_MUTEX_INITIALIZER,
        .mCond  = PTHREAD_COND_INITIALIZER,

        .mMethod = aMethod,
    };

    struct Thread self_;
    if ( ! self)
        self = &self_;
    else if (aAttr)
    {
        /* If the caller has specified a non-null self, ensure that there
         * the thread is detached, since the parent is expected to join
         * and close the thread. */

        int detached;
        ABORT_IF(
            (errno = pthread_attr_getdetachstate(&aAttr->mAttr, &detached)),
            {
                terminate(
                    errno,
                    "Unable to query thread detached state attribute");
            });

        ABORT_IF(
            detached,
            {
                errno = EINVAL;
            });
    }

    ABORT_UNLESS(
        (thread.mFuture = createThreadFuture_()),
        {
            terminate(
                errno,
                "Unable to create thread future");
        });

    pthread_mutex_t *lock = lockMutex(&thread.mMutex);

    self->mJoined = false;

    ABORT_IF(
        (errno = pthread_create(
            &self->mThread, aAttr ? &aAttr->mAttr : 0, createThread_, &thread)),
        {
            terminate(
                errno,
                "Unable to create thread");
        });

    waitCond(&thread.mCond, lock);
    lock = unlockMutex(lock);

    return self != &self_ ? self : 0;
}

/* -------------------------------------------------------------------------- */
int
joinThread(struct Thread *self)
{
    int rc = -1;

    struct ThreadFuture_ *future = 0;

    ERROR_IF(
        self->mJoined,
        {
            errno = EINVAL;
        });

    void *future_;
    ERROR_IF(
        (errno = pthread_join(self->mThread, &future_)));

    self->mJoined = true;

    ERROR_IF(
        PTHREAD_CANCELED == future_,
        {
            errno = ECANCELED;
        });

    future = future_;

    rc = future->mStatus;

Finally:

    FINALLY
    ({
        free(future);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
void
cancelThread(struct Thread *self)
{
    ABORT_IF(
        errno = pthread_cancel(self->mThread),
        {
            terminate("Unable to cancel thread");
        });
}

/* -------------------------------------------------------------------------- */
int
killThread(struct Thread *self, int aSignal)
{
    int rc = -1;

    ERROR_IF(
        errno = pthread_kill(self->mThread, aSignal));

    rc = 0;

Finally:
    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
struct ThreadAttr *
createThreadAttr(struct ThreadAttr *self)
{
    ABORT_IF(
        (errno = pthread_attr_init(&self->mAttr)),
        {
            terminate(
                errno,
                "Unable to create thread attribute");
        });

    return self;
}

/* -------------------------------------------------------------------------- */
struct ThreadAttr *
destroyThreadAttr(struct ThreadAttr *self)
{
    ABORT_IF(
        (errno = pthread_attr_destroy(&self->mAttr)),
        {
            terminate(
                errno,
                "Unable to destroy thread attribute");
        });

    return 0;
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
static CHECKED int
tryMutexLock_(void *self)
{
    return pthread_mutex_trylock(self);
}

static CHECKED int
tryMutexTimedLock_(void *self, const struct timespec *aDeadline)
{
    return pthread_mutex_timedlock(self, aDeadline);
}

static pthread_mutex_t *
lockMutex_(pthread_mutex_t *self)
{
    return timedLock_(self, tryMutexLock_, tryMutexTimedLock_);
}

pthread_mutex_t *
lockMutex(pthread_mutex_t *self)
{
    ensure( ! ownProcessSignalContext());

    ABORT_UNLESS(
        lockMutex_(self));

    return self;
}

/* -------------------------------------------------------------------------- */
static CHECKED pthread_mutex_t *
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
struct SharedMutex *
createSharedMutex(struct SharedMutex *self)
{
    pthread_mutexattr_t  mutexattr_;
    pthread_mutexattr_t *mutexattr = 0;

    ABORT_IF(
        (errno = pthread_mutexattr_init(&mutexattr_)),
        {
            terminate(
                errno,
                "Unable to allocate mutex attribute");
        });
    mutexattr = &mutexattr_;

    ABORT_IF(
        (errno = pthread_mutexattr_setpshared(mutexattr,
                                              PTHREAD_PROCESS_SHARED)),
        {
            terminate(
                errno,
                "Unable to set mutex attribute PTHREAD_PROCESS_SHARED");
        });

    ABORT_IF(
        (errno = pthread_mutexattr_setrobust(mutexattr,
                                             PTHREAD_MUTEX_ROBUST)),
        {
            terminate(
                errno,
                "Unable to set mutex attribute PTHREAD_MUTEX_ROBUST");
        });

    ABORT_IF(
        (errno = pthread_mutex_init(&self->mMutex, mutexattr)),
        {
            terminate(
                errno,
                "Unable to create shared mutex");
        });

    ABORT_IF(
        (errno = pthread_mutexattr_destroy(mutexattr)),
        {
            terminate(
                errno,
                "Unable to destroy mutex attribute");
        });

    return self;
}

/* -------------------------------------------------------------------------- */
struct SharedMutex *
destroySharedMutex(struct SharedMutex *self)
{
    if (self)
        ABORT_IF(
            destroyMutex(&self->mMutex));

    return 0;
}

/* -------------------------------------------------------------------------- */
struct SharedMutex *
lockSharedMutex(struct SharedMutex      *self,
                struct MutexRepairMethod aRepair)
{
    ensure( ! ownProcessSignalContext());

    if ( ! lockMutex_(&self->mMutex))
    {
        ABORT_IF(
            callMutexRepairMethod(aRepair),
            {
                terminate(
                    errno,
                    "Unable to repair mutex consistency");
            });

        ABORT_IF(
            (errno = pthread_mutex_consistent(&self->mMutex)),
            {
                terminate(
                    errno,
                    "Unable to restore mutex consistency");
            });
    }

    return self;
}

/* -------------------------------------------------------------------------- */
struct SharedMutex *
unlockSharedMutex(struct SharedMutex *self)
{
    ensure( ! ownProcessSignalContext());

    if (self)
        ABORT_IF(
            unlockMutex_(&self->mMutex));

    return 0;
}

/* -------------------------------------------------------------------------- */
struct SharedMutex *
unlockSharedMutexSignal(struct SharedMutex *self, struct SharedCond *aCond)
{
    ensure( ! ownProcessSignalContext());

    if (self)
        ABORT_IF(
            unlockMutexSignal_(&self->mMutex, &aCond->mCond));

    return 0;
}

/* -------------------------------------------------------------------------- */
struct SharedMutex *
unlockSharedMutexBroadcast(struct SharedMutex *self, struct SharedCond *aCond)
{
    ensure( ! ownProcessSignalContext());

    if (self)
        ABORT_IF(
            unlockMutexBroadcast_(&self->mMutex, &aCond->mCond));

    return 0;
}

/* -------------------------------------------------------------------------- */
pthread_cond_t *
createCond(pthread_cond_t *self)
{
    pthread_condattr_t  condattr_;
    pthread_condattr_t *condattr = 0;

    ABORT_IF(
        (errno = pthread_condattr_init(&condattr_)),
        {
            terminate(
                errno,
                "Unable to allocate condition variable attribute");
        });
    condattr = &condattr_;

    ABORT_IF(
        (errno = pthread_condattr_setclock(condattr, CLOCK_MONOTONIC)),
        {
            terminate(
                errno,
                "Unable to set condition attribute CLOCK_MONOTONIC");
        });

    ABORT_IF(
        (errno = pthread_cond_init(self, condattr)),
        {
            terminate(
                errno,
                "Unable to create condition variable");
        });

    ABORT_IF(
        (errno = pthread_condattr_destroy(condattr)),
        {
            terminate(
                errno,
                "Unable to destroy condition attribute");
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
static int
waitCond_(pthread_cond_t *self, pthread_mutex_t *aMutex)
{
    ABORT_IF(
        (errno = pthread_cond_wait(self, aMutex),
         errno && EOWNERDEAD != errno),
        {
            terminate(
                errno,
                "Unable to wait for condition variable");
        });

    return errno;
}

void
waitCond(pthread_cond_t *self, pthread_mutex_t *aMutex)
{
    ensure( ! ownProcessSignalContext());

    ABORT_IF(
        waitCond_(self, aMutex),
        {
            terminate(
                errno,
                "Condition variable mutex owner has terminated");
        });
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
struct SharedCond *
createSharedCond(struct SharedCond *self)
{
    pthread_condattr_t  condattr_;
    pthread_condattr_t *condattr = 0;

    ABORT_IF(
        (errno = pthread_condattr_init(&condattr_)),
        {
            terminate(
                errno,
                "Unable to allocate condition variable attribute");
        });
    condattr = &condattr_;

    ABORT_IF(
        (errno = pthread_condattr_setclock(condattr, CLOCK_MONOTONIC)),
        {
            terminate(
                errno,
                "Unable to set condition attribute CLOCK_MONOTONIC");
        });

    ABORT_IF(
        (errno = pthread_condattr_setpshared(condattr,
                                             PTHREAD_PROCESS_SHARED)),
        {
            terminate(
                errno,
                "Unable to set condition attribute PTHREAD_PROCESS_SHARED");
        });

    ABORT_IF(
        (errno = pthread_cond_init(&self->mCond, condattr)),
        {
            terminate(
                errno,
                "Unable to create shared condition variable");
        });

    ABORT_IF(
        (errno = pthread_condattr_destroy(condattr)),
        {
            terminate(
                errno,
                "Unable to destroy condition attribute");
        });

    return self;
}

/* -------------------------------------------------------------------------- */
struct SharedCond *
destroySharedCond(struct SharedCond *self)
{
    if (self)
        ABORT_IF(
            destroyCond(&self->mCond));

    return 0;
}

/* -------------------------------------------------------------------------- */
int
waitSharedCond(struct SharedCond *self, struct SharedMutex *aMutex)
{
    ensure( ! ownProcessSignalContext());

    return waitCond_(&self->mCond, &aMutex->mMutex) ? -1 : 0;
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
int
waitThreadSigMask(const int *aSigList)
{
    int rc = -1;

    sigset_t sigSet;

    if ( ! aSigList)
        ERROR_IF(
            sigemptyset(&sigSet));
    else
    {
        ERROR_IF(
            sigfillset(&sigSet));
        for (size_t ix = 0; aSigList[ix]; ++ix)
            ERROR_IF(
                sigdelset(&sigSet, aSigList[ix]));
    }

    int err = 0;
    ERROR_IF(
        (err = sigsuspend(&sigSet),
         -1 != err || EINTR != errno),
        {
            if (-1 != err)
                errno = 0;
        });

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
struct ThreadSigMutex *
createThreadSigMutex(struct ThreadSigMutex *self)
{
    self->mMutex = createMutex(&self->mMutex_);
    self->mCond  = createCond(&self->mCond_);

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

        self->mCond  = destroyCond(self->mCond);
        self->mMutex = destroyMutex(self->mMutex);
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

    struct ThreadSigMask  threadSigMask_;
    struct ThreadSigMask *threadSigMask =
        pushThreadSigMask(&threadSigMask_, ThreadSigMaskBlock, 0);

    pthread_mutex_t *lock;
    ABORT_UNLESS(
        lock = lockMutex_(self->mMutex));

    if (self->mLocked && ! pthread_equal(self->mOwner, pthread_self()))
    {
        do
            waitCond_(self->mCond, lock);
        while (self->mLocked);
    }

    if (1 == ++self->mLocked)
    {
        self->mMask  = *threadSigMask;
        self->mOwner = pthread_self();
    }

    lock = unlockMutex_(lock);

    ensure(self->mLocked);
    ensure(pthread_equal(self->mOwner, pthread_self()));

    if (1 != self->mLocked)
        threadSigMask = popThreadSigMask(threadSigMask);

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

        unsigned locked = self->mLocked - 1;

        if (locked)
            self->mLocked = locked;
        else
        {
            struct ThreadSigMask  threadSigMask_ = self->mMask;
            struct ThreadSigMask *threadSigMask  = &threadSigMask_;

            pthread_mutex_t *lock;
            ABORT_UNLESS(
                lock = lockMutex_(self->mMutex));

            self->mLocked = 0;

            unlockMutexSignal_(lock, self->mCond);

            /* Do not reference the mutex instance once it is released
             * because it might at this point be owned by another
             * thread. */

            threadSigMask = popThreadSigMask(threadSigMask);
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
unsigned
ownThreadSigMutexLocked(struct ThreadSigMutex *self)
{
    unsigned locked;

    pthread_mutex_t *lock;
    ABORT_UNLESS(
        lock = lockMutex_(self->mMutex));

    locked = self->mLocked;

    if (locked && ! pthread_equal(self->mOwner, pthread_self()))
        locked = 0;

    lock = unlockMutex_(lock);

    return locked;
}

/* -------------------------------------------------------------------------- */
pthread_rwlock_t *
createRWMutex(pthread_rwlock_t *self)
{
    ABORT_IF(
        (errno = pthread_rwlock_init(self, 0)),
        {
            terminate(
                errno,
                "Unable to create rwlock");
        });

    return self;
}

/* -------------------------------------------------------------------------- */
pthread_rwlock_t *
destroyRWMutex(pthread_rwlock_t *self)
{
    if (self)
    {
        ABORT_IF(
            (errno = pthread_rwlock_destroy(self)),
            {
                terminate(
                    errno,
                    "Unable to destroy rwlock");
            });
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
static CHECKED int
tryRWMutexRdLock_(void *self)
{
    return pthread_rwlock_tryrdlock(self);
}

static CHECKED int
tryRWMutexTimedRdLock_(void *self, const struct timespec *aDeadline)
{
    return pthread_rwlock_timedrdlock(self, aDeadline);
}

struct RWMutexReader *
createRWMutexReader(struct RWMutexReader *self,
                    pthread_rwlock_t     *aMutex)
{
    ABORT_UNLESS(
        self->mMutex = timedLock_(
            aMutex, tryRWMutexRdLock_, tryRWMutexTimedRdLock_));

    return self;
}

/* -------------------------------------------------------------------------- */
struct RWMutexReader *
destroyRWMutexReader(struct RWMutexReader *self)
{
    if (self)
    {
        ABORT_IF(
            (errno = pthread_rwlock_unlock(self->mMutex)),
            {
                terminate(
                    errno,
                    "Unable to release rwlock reader lock");
            });
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
static CHECKED int
tryRWMutexWrLock_(void *self)
{
    return pthread_rwlock_trywrlock(self);
}

static CHECKED int
tryRWMutexTimedWrLock_(void *self, const struct timespec *aDeadline)
{
    return pthread_rwlock_timedwrlock(self, aDeadline);
}

struct RWMutexWriter *
createRWMutexWriter(struct RWMutexWriter *self,
                    pthread_rwlock_t     *aMutex)
{
    ABORT_UNLESS(
        self->mMutex = timedLock_(
            aMutex, tryRWMutexWrLock_, tryRWMutexTimedWrLock_));

    return self;
}

/* -------------------------------------------------------------------------- */
struct RWMutexWriter *
destroyRWMutexWriter(struct RWMutexWriter *self)
{
    if (self)
    {
        ABORT_IF(
            (errno = pthread_rwlock_unlock(self->mMutex)),
            {
                terminate(
                    errno,
                    "Unable to release rwlock writer lock");
            });
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
