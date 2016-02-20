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

#include <stdbool.h>
#include <pthread.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    pthread_mutex_t      mMutex;
    pthread_cond_t       mCond;
    struct ThreadSigMask mMask;
    unsigned             mLocked;
    pthread_t            mOwner;
};

#define THREAD_SIG_MUTEX_INITIALIZER {   \
    .mMutex = PTHREAD_MUTEX_INITIALIZER, \
    .mCond  = PTHREAD_COND_INITIALIZER, }

/* -------------------------------------------------------------------------- */
void
createThread(pthread_t      *self,
             pthread_attr_t *aAttr,
             void           *aThread(void *),
             void           *aContext);

void *
joinThread(pthread_t *self);

/* -------------------------------------------------------------------------- */
void
createThreadAttr(pthread_attr_t *self);

void
destroyThreadAttr(pthread_attr_t *self);

void
setThreadAttrDetachState(pthread_attr_t *self, int aState);

/* -------------------------------------------------------------------------- */
void
createThreadSigMutex(struct ThreadSigMutex *self);

void
destroyThreadSigMutex(struct ThreadSigMutex *self);

struct ThreadSigMutex *
lockThreadSigMutex(struct ThreadSigMutex *self);

unsigned
ownThreadSigMutexLocked(struct ThreadSigMutex *self);

struct ThreadSigMutex *
unlockThreadSigMutex(struct ThreadSigMutex *self);

/* -------------------------------------------------------------------------- */
void
createSharedMutex(pthread_mutex_t *self);

void
destroyMutex(pthread_mutex_t *self);

pthread_mutex_t *
lockMutex(pthread_mutex_t *self);

pthread_mutex_t *
unlockMutex(pthread_mutex_t *self);

pthread_mutex_t *
unlockMutexSignal(pthread_mutex_t *self, pthread_cond_t *aCond);

pthread_mutex_t *
unlockMutexBroadcast(pthread_mutex_t *self, pthread_cond_t *aCond);

/* -------------------------------------------------------------------------- */
void
createSharedCond(pthread_cond_t *self);

void
destroyCond(pthread_cond_t *self);

void
waitCond(pthread_cond_t *self, pthread_mutex_t *aMutex);

/* -------------------------------------------------------------------------- */
int
pushThreadSigMask(
    struct ThreadSigMask    *self,
    enum ThreadSigMaskAction aAction,
    const int               *aSigList);

int
popThreadSigMask(struct ThreadSigMask *self);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* THREAD_H */
