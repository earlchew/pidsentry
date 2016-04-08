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
#ifndef JOBCONTROL_H
#define JOBCONTROL_H

#include "method_.h"
#include "thread_.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Type;

/* -------------------------------------------------------------------------- */
struct JobControl
{
    const struct Type *mType;

    struct
    {
        struct ThreadSigMutex mMutex;
        struct VoidIntMethod  mMethod;
    } mRaise;

    struct
    {
        struct ThreadSigMutex mMutex;
        struct VoidMethod     mMethod;
    } mReap;

    struct
    {
        struct ThreadSigMutex mMutex;
        struct VoidMethod     mPauseMethod;
        struct VoidMethod     mResumeMethod;
    } mStop;

    struct
    {
        struct ThreadSigMutex mMutex;
        struct VoidMethod     mMethod;
    } mContinue;
};

/* -------------------------------------------------------------------------- */
int
createJobControl(struct JobControl *self);

void
closeJobControl(struct JobControl *self);

int
watchJobControlSignals(struct JobControl   *self,
                       struct VoidIntMethod aRaiseMethod);

int
unwatchJobControlSignals(struct JobControl *self);

int
watchJobControlDone(struct JobControl *self,
                    struct VoidMethod  aReapMethod);

int
unwatchJobControlDone(struct JobControl *self);

int
watchJobControlStop(struct JobControl *self,
                    struct VoidMethod  aPauseMethod,
                    struct VoidMethod  aResumeMethod);

int
unwatchJobControlStop(struct JobControl *self);

int
watchJobControlContinue(struct JobControl *self,
                        struct VoidMethod  aContinueMethod);

int
unwatchJobControlContinue(struct JobControl *self);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* JOBCONTROL_H */
