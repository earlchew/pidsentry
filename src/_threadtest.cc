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

#include <unistd.h>

#include <valgrind/valgrind.h>

#include "gtest/gtest.h"

TEST(ThreadTest, MutexDestroy)
{
    pthread_mutex_t  mutex_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t *mutex  = &mutex_;

    mutex = destroyMutex(mutex);
}

TEST(ThreadTest, CondDestroy)
{
    pthread_cond_t  cond_ = PTHREAD_COND_INITIALIZER;
    pthread_cond_t *cond  = &cond_;

    cond = destroyCond(cond);
}

TEST(ThreadTest, RWMutexDestroy)
{
    pthread_rwlock_t  rwlock_ = PTHREAD_RWLOCK_INITIALIZER;
    pthread_rwlock_t *rwlock  = &rwlock_;

    rwlock = destroyRWMutex(rwlock);
}

static int sigTermCount_;

static void
sigTermAction_(int)
{
    ++sigTermCount_;
}

TEST(ThreadTest, ThreadSigMutex)
{
    struct ThreadSigMutex  sigMutex_;
    struct ThreadSigMutex *sigMutex = 0;

    sigMutex = createThreadSigMutex(&sigMutex_);

    struct sigaction prevAction;
    struct sigaction nextAction;

    nextAction.sa_handler = sigTermAction_;
    nextAction.sa_flags   = 0;
    EXPECT_FALSE(sigfillset(&nextAction.sa_mask));

    EXPECT_FALSE(sigaction(SIGTERM, &nextAction, &prevAction));

    EXPECT_FALSE(raise(SIGTERM));
    EXPECT_EQ(1, sigTermCount_);

    EXPECT_FALSE(raise(SIGTERM));
    EXPECT_EQ(2, sigTermCount_);

    struct ThreadSigMutex *lock = lockThreadSigMutex(sigMutex);
    {
        // Verify that the lock also excludes the delivery of signals
        // while the lock is taken.

        EXPECT_FALSE(raise(SIGTERM));
        EXPECT_EQ(2, sigTermCount_);

        EXPECT_FALSE(raise(SIGTERM));
        EXPECT_EQ(2, sigTermCount_);
    }
    lock = unlockThreadSigMutex(lock);

    EXPECT_EQ(3, sigTermCount_);

    EXPECT_FALSE(raise(SIGTERM));
    EXPECT_EQ(4, sigTermCount_);

    EXPECT_FALSE(raise(SIGTERM));
    EXPECT_EQ(5, sigTermCount_);

    EXPECT_FALSE(sigaction(SIGTERM, &prevAction, 0));

    sigMutex = destroyThreadSigMutex(sigMutex);
}

#include "../googletest/src/gtest_main.cc"
