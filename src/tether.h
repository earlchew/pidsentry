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
#ifndef TETHER_H
#define TETHER_H

#include "ert/compiler.h"
#include "ert/pipe.h"
#include "ert/timekeeping.h"
#include "ert/thread.h"

ERT_BEGIN_C_SCOPE;

/* -------------------------------------------------------------------------- */
enum TetherThreadState
{
    TETHER_THREAD_STOPPED,
    TETHER_THREAD_RUNNING,
    TETHER_THREAD_STOPPING,
};

struct TetherThread
{
    struct Ert_Pipe  mControlPipe_;
    struct Ert_Pipe *mControlPipe;

    struct Ert_Thread  mThread_;
    struct Ert_Thread *mThread;

    struct Ert_Pipe *mNullPipe;
    bool             mFlushed;

    struct {
        pthread_mutex_t            mMutex_;
        pthread_mutex_t           *mMutex;
        struct Ert_EventClockTime  mSince;
    } mActivity;

    struct {
        pthread_mutex_t        mMutex_;
        pthread_mutex_t       *mMutex;
        pthread_cond_t         mCond_;
        pthread_cond_t        *mCond;
        enum TetherThreadState mValue;
    } mState;
};

/* -------------------------------------------------------------------------- */
ERT_CHECKED int
createTetherThread(struct TetherThread *self, struct Ert_Pipe *aNullPipe);

ERT_CHECKED int
pingTetherThread(struct TetherThread *self);

ERT_CHECKED int
flushTetherThread(struct TetherThread *self);

struct TetherThread *
closeTetherThread(struct TetherThread *self);

/* -------------------------------------------------------------------------- */

ERT_END_C_SCOPE;

#endif /* TETHER_H */
