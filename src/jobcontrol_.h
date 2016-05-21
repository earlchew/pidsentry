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

#include "compiler_.h"
#include "method_.h"
#include "thread_.h"

/* -------------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
struct JobControl
{
    struct
    {
        struct IntIntMethod mMethod;
    } mRaise;

    struct
    {
        struct IntMethod mMethod;
    } mReap;

    struct
    {
        struct IntMethod mPauseMethod;
        struct IntMethod mResumeMethod;
    } mStop;

    struct
    {
        struct IntMethod mMethod;
    } mContinue;
};

/* -------------------------------------------------------------------------- */
CHECKED int
createJobControl(struct JobControl *self);

CHECKED struct JobControl *
closeJobControl(struct JobControl *self);

CHECKED int
watchJobControlSignals(struct JobControl  *self,
                       struct IntIntMethod aRaiseMethod);

CHECKED int
unwatchJobControlSignals(struct JobControl *self);

CHECKED int
watchJobControlDone(struct JobControl *self,
                    struct IntMethod   aReapMethod);

CHECKED int
unwatchJobControlDone(struct JobControl *self);

CHECKED int
watchJobControlStop(struct JobControl *self,
                    struct IntMethod   aPauseMethod,
                    struct IntMethod   aResumeMethod);

CHECKED int
unwatchJobControlStop(struct JobControl *self);

CHECKED int
watchJobControlContinue(struct JobControl *self,
                        struct IntMethod   aContinueMethod);

CHECKED int
unwatchJobControlContinue(struct JobControl *self);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* JOBCONTROL_H */
