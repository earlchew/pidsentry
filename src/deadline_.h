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
#ifndef DEADLINE_H
#define DEADLINE_H

#include "compiler_.h"
#include "timekeeping_.h"
#include "process_.h"
#include "method_.h"

BEGIN_C_SCOPE;
struct Duration;
END_C_SCOPE;

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_DeadlinePollMethod    int
#define METHOD_CONST_DeadlinePollMethod
#define METHOD_ARG_LIST_DeadlinePollMethod  ()
#define METHOD_CALL_LIST_DeadlinePollMethod ()

#define METHOD_NAME      DeadlinePollMethod
#define METHOD_RETURN    METHOD_RETURN_DeadlinePollMethod
#define METHOD_CONST     METHOD_CONST_DeadlinePollMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_DeadlinePollMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_DeadlinePollMethod
#include "method_.h"

#define DeadlinePollMethod(Method_, Object_) \
    METHOD_TRAMPOLINE(                       \
        Method_, Object_,                    \
        DeadlinePollMethod_,                 \
        METHOD_RETURN_DeadlinePollMethod,    \
        METHOD_CONST_DeadlinePollMethod,     \
        METHOD_ARG_LIST_DeadlinePollMethod,  \
        METHOD_CALL_LIST_DeadlinePollMethod)

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_DeadlineWaitMethod    int
#define METHOD_CONST_DeadlineWaitMethod
#define METHOD_ARG_LIST_DeadlineWaitMethod  (const struct Duration *aTimeout_)
#define METHOD_CALL_LIST_DeadlineWaitMethod (aTimeout_)

#define METHOD_NAME      DeadlineWaitMethod
#define METHOD_RETURN    METHOD_RETURN_DeadlineWaitMethod
#define METHOD_CONST     METHOD_CONST_DeadlineWaitMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_DeadlineWaitMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_DeadlineWaitMethod
#include "method_.h"

#define DeadlineWaitMethod(Method_, Object_) \
    METHOD_TRAMPOLINE(                       \
        Method_, Object_,                    \
        DeadlineWaitMethod_,                 \
        METHOD_RETURN_DeadlineWaitMethod,    \
        METHOD_CONST_DeadlineWaitMethod,     \
        METHOD_ARG_LIST_DeadlineWaitMethod,  \
        METHOD_CALL_LIST_DeadlineWaitMethod)

/* -------------------------------------------------------------------------- */
BEGIN_C_SCOPE;

struct Deadline
{
    struct EventClockTime        mSince;
    struct EventClockTime        mTime;
    struct Duration              mRemaining;
    struct ProcessSigContTracker mSigContTracker;
    struct Duration              mDuration_;
    struct Duration             *mDuration;
};

/* -------------------------------------------------------------------------- */
CHECKED int
createDeadline(struct Deadline *self, const struct Duration *aDuration);

CHECKED int
checkDeadlineExpired(struct Deadline *self,
                     struct DeadlinePollMethod aPollMethod,
                     struct DeadlineWaitMethod aWaitMethod);

CHECKED struct Deadline *
closeDeadline(struct Deadline *self);

/* -------------------------------------------------------------------------- */

END_C_SCOPE;

#endif /* DEADLINE_H */
