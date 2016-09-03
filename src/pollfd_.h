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
#ifndef POLLFD_H
#define POLLFD_H

#include "compiler_.h"
#include "timekeeping_.h"
#include "method_.h"

#include <stdbool.h>
#include <limits.h>

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_PollFdCompletionMethod    bool
#define METHOD_CONST_PollFdCompletionMethod
#define METHOD_ARG_LIST_PollFdCompletionMethod  ()
#define METHOD_CALL_LIST_PollFdCompletionMethod ()

#define METHOD_NAME      PollFdCompletionMethod
#define METHOD_RETURN    METHOD_RETURN_PollFdCompletionMethod
#define METHOD_CONST     METHOD_CONST_PollFdCompletionMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_PollFdCompletionMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_PollFdCompletionMethod
#include "method_.h"

#define PollFdCompletionMethod(Object_, Method_)  \
    METHOD_TRAMPOLINE(                            \
        Object_, Method_,                         \
        PollFdCompletionMethod_,                  \
        METHOD_RETURN_PollFdCompletionMethod,     \
        METHOD_CONST_PollFdCompletionMethod,      \
        METHOD_ARG_LIST_PollFdCompletionMethod,   \
        METHOD_CALL_LIST_PollFdCompletionMethod)

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_PollFdCallbackMethod \
    int
#define METHOD_CONST_PollFdCallbackMethod
#define METHOD_ARG_LIST_PollFdCallbackMethod \
    (const struct EventClockTime *aPollTime_)
#define METHOD_CALL_LIST_PollFdCallbackMethod \
    (aPollTime_)

#define METHOD_NAME      PollFdCallbackMethod
#define METHOD_RETURN    METHOD_RETURN_PollFdCallbackMethod
#define METHOD_CONST     METHOD_CONST_PollFdCallbackMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_PollFdCallbackMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_PollFdCallbackMethod
#include "method_.h"

#define PollFdCallbackMethod(Object_, Method_)  \
    METHOD_TRAMPOLINE(                          \
        Object_, Method_,                       \
        PollFdCallbackMethod_,                  \
        METHOD_RETURN_PollFdCallbackMethod,     \
        METHOD_CONST_PollFdCallbackMethod,      \
        METHOD_ARG_LIST_PollFdCallbackMethod,   \
        METHOD_CALL_LIST_PollFdCallbackMethod)

/* -------------------------------------------------------------------------- */
BEGIN_C_SCOPE;

struct pollfd;

/* -------------------------------------------------------------------------- */
struct PollFdAction
{
    struct PollFdCallbackMethod mAction;
};

struct PollFdTimerAction
{
    struct PollFdCallbackMethod mAction;
    struct Duration             mPeriod;
    struct EventClockTime       mSince;
};

struct PollFd
{
    struct pollfd                 *mPoll;
    struct PollFdCompletionMethod  mCompletionQuery;

    struct
    {
        struct PollFdAction *mActions;
        const char * const  *mNames;
        size_t               mSize;
    } mFdActions;

    struct
    {
        struct PollFdTimerAction *mActions;
        const char * const       *mNames;
        size_t                    mSize;
    } mTimerActions;
};

struct PollEventText
{
    char mText[
        sizeof(unsigned) * CHAR_BIT +
        sizeof(
            " "
            "0x "
            "IN "
            "PRI "
            "OUT "
            "ERR "
            "HUP "
            "NVAL ")];
};

/* -------------------------------------------------------------------------- */
#define POLL_INPUTEVENTS       ((unsigned) (POLLHUP|POLLERR|POLLPRI|POLLIN))
#define POLL_OUTPUTEVENTS      ((unsigned) (POLLHUP|POLLERR|POLLOUT))
#define POLL_DISCONNECTEVENT   ((unsigned) (POLLHUP|POLLERR))

/* -------------------------------------------------------------------------- */
CHECKED int
createPollFd(struct PollFd                 *self,
             struct pollfd                 *aPoll,
             struct PollFdAction           *aFdActions,
             const char * const            *aFdNames,
             size_t                         aNumFdActions,
             struct PollFdTimerAction      *aTimerActions,
             const char * const            *aTimerNames,
             size_t                         aNumTimerActions,
             struct PollFdCompletionMethod  aCompletionQuery);

CHECKED int
runPollFdLoop(struct PollFd *self);

CHECKED struct PollFd *
closePollFd(struct PollFd *self);

/* -------------------------------------------------------------------------- */
const char *
createPollEventText(
    struct PollEventText *aPollEventText, unsigned aPollEventMask);

/* -------------------------------------------------------------------------- */

END_C_SCOPE;

#endif /* POLLFD_H */
