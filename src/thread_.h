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
#ifndef THREAD_H
#define THREAD_H

#include "compiler_.h"
#include "method_.h"

#include <stdbool.h>
#include <pthread.h>
#include <signal.h>

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_ThreadMethod    int
#define METHOD_CONST_ThreadMethod
#define METHOD_ARG_LIST_ThreadMethod  ()
#define METHOD_CALL_LIST_ThreadMethod ()

#define METHOD_NAME      ThreadMethod
#define METHOD_RETURN    METHOD_RETURN_ThreadMethod
#define METHOD_CONST     METHOD_CONST_ThreadMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_ThreadMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_ThreadMethod
#include "method_.h"

#define ThreadMethod(Method_, Object_)          \
    METHOD_TRAMPOLINE(                          \
        Method_, Object_,                       \
        ThreadMethod_,                          \
        METHOD_RETURN_ThreadMethod,             \
        METHOD_CONST_ThreadMethod,              \
        METHOD_ARG_LIST_ThreadMethod,           \
        METHOD_CALL_LIST_ThreadMethod)

/* -------------------------------------------------------------------------- */
BEGIN_C_SCOPE;

struct Tid;

struct ThreadSigMask
{
    sigset_t mSigSet;
};

enum ThreadSigMaskAction
{
    ThreadSigMaskUnblock = -1,
    ThreadSigMaskSet     =  0,
    ThreadSigMaskBlock   = +1,
};

struct ThreadSigMutex
{
    pthread_mutex_t      mMutex_;
    pthread_mutex_t     *mMutex;
    pthread_cond_t       mCond_;
    pthread_cond_t      *mCond;
    struct ThreadSigMask mMask;
    unsigned             mLocked;
    pthread_t            mOwner;
};

#define THREAD_SIG_MUTEX_INITIALIZER(Mutex_) \
{                                            \
    .mMutex_ = PTHREAD_MUTEX_INITIALIZER,    \
    .mMutex  = &(Mutex_).mMutex_,            \
    .mCond_  = PTHREAD_COND_INITIALIZER,     \
    .mCond   = &(Mutex_).mCond_,             \
}

struct RWMutexReader
{
    pthread_rwlock_t *mMutex;
};

struct RWMutexWriter
{
    pthread_rwlock_t *mMutex;
};

struct SharedMutex
{
    unsigned        mRefs;
    pthread_mutex_t mMutex;
};

struct SharedCond
{
    unsigned       mRefs;
    pthread_cond_t mCond;
};

/* -------------------------------------------------------------------------- */
struct Tid
ownThreadId(void);

/* -------------------------------------------------------------------------- */
void
createThread(
    pthread_t *self, pthread_attr_t *aAttr, struct ThreadMethod aMethod);

CHECKED int
joinThread(pthread_t *self);

void
cancelThread(pthread_t *self);

/* -------------------------------------------------------------------------- */
void
createThreadAttr(pthread_attr_t *self);

void
destroyThreadAttr(pthread_attr_t *self);

void
setThreadAttrDetachState(pthread_attr_t *self, int aState);

/* -------------------------------------------------------------------------- */
CHECKED struct ThreadSigMutex *
createThreadSigMutex(struct ThreadSigMutex *self);

CHECKED struct ThreadSigMutex *
destroyThreadSigMutex(struct ThreadSigMutex *self);

CHECKED struct ThreadSigMutex *
lockThreadSigMutex(struct ThreadSigMutex *self);

unsigned
ownThreadSigMutexLocked(struct ThreadSigMutex *self);

CHECKED struct ThreadSigMutex *
unlockThreadSigMutex(struct ThreadSigMutex *self);

/* -------------------------------------------------------------------------- */
CHECKED pthread_mutex_t *
createMutex(pthread_mutex_t *self);

CHECKED pthread_mutex_t *
destroyMutex(pthread_mutex_t *self);

CHECKED pthread_mutex_t *
lockMutex(pthread_mutex_t *self);

CHECKED pthread_mutex_t *
unlockMutex(pthread_mutex_t *self);

CHECKED pthread_mutex_t *
unlockMutexSignal(pthread_mutex_t *self, pthread_cond_t *aCond);

CHECKED pthread_mutex_t *
unlockMutexBroadcast(pthread_mutex_t *self, pthread_cond_t *aCond);

/* -------------------------------------------------------------------------- */
CHECKED struct SharedMutex *
createSharedMutex(struct SharedMutex *self);

CHECKED struct SharedMutex *
destroySharedMutex(struct SharedMutex *self);

CHECKED struct SharedMutex *
refSharedMutex(struct SharedMutex *self);

CHECKED struct SharedMutex *
unrefSharedMutex(struct SharedMutex *self);

CHECKED struct SharedMutex *
lockSharedMutex(struct SharedMutex *self);

CHECKED struct SharedMutex *
unlockSharedMutex(struct SharedMutex *self);

CHECKED struct SharedMutex *
markSharedMutexConsistent(struct SharedMutex *self);

CHECKED struct SharedMutex *
unlockSharedMutexSignal(struct SharedMutex *self, struct SharedCond *aCond);

CHECKED struct SharedMutex *
unlockSharedMutexBroadcast(struct SharedMutex *self, struct SharedCond *aCond);

/* -------------------------------------------------------------------------- */
CHECKED pthread_rwlock_t *
createRWMutex(pthread_rwlock_t *self);

CHECKED pthread_rwlock_t *
destroyRWMutex(pthread_rwlock_t *self);

/* -------------------------------------------------------------------------- */
CHECKED struct RWMutexReader *
createRWMutexReader(struct RWMutexReader *self,
                    pthread_rwlock_t     *aMutex);

CHECKED struct RWMutexReader *
destroyRWMutexReader(struct RWMutexReader *self);

/* -------------------------------------------------------------------------- */
CHECKED struct RWMutexWriter *
createRWMutexWriter(struct RWMutexWriter *self,
                    pthread_rwlock_t     *aMutex);

CHECKED struct RWMutexWriter *
destroyRWMutexWriter(struct RWMutexWriter *self);

/* -------------------------------------------------------------------------- */
CHECKED pthread_cond_t *
createCond(pthread_cond_t *self);

CHECKED pthread_cond_t *
destroyCond(pthread_cond_t *self);

CHECKED struct SharedCond *
refSharedCond(struct SharedCond *self);

CHECKED struct SharedCond *
unrefSharedCond(struct SharedCond *self);

void
waitCond(pthread_cond_t *self, pthread_mutex_t *aMutex);

/* -------------------------------------------------------------------------- */
CHECKED struct SharedCond *
createSharedCond(struct SharedCond *self);

CHECKED struct SharedCond *
destroySharedCond(struct SharedCond *self);

CHECKED int
waitSharedCond(struct SharedCond *self, struct SharedMutex *aMutex);

/* -------------------------------------------------------------------------- */
CHECKED struct ThreadSigMask *
pushThreadSigMask(
    struct ThreadSigMask    *self,
    enum ThreadSigMaskAction aAction,
    const int               *aSigList);

CHECKED struct ThreadSigMask *
popThreadSigMask(struct ThreadSigMask *self);

CHECKED int
waitThreadSigMask(const int *aSigList);

/* -------------------------------------------------------------------------- */

END_C_SCOPE;

#endif /* THREAD_H */
