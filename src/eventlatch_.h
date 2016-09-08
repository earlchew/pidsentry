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
#ifndef EVENTLATCH_H
#define EVENTLATCH_H

#include "compiler_.h"
#include "thread_.h"
#include "method_.h"
#include "queue_.h"

#include <stdio.h>

/* -------------------------------------------------------------------------- */
BEGIN_C_SCOPE;
struct EventClockTime;
END_C_SCOPE;

#define METHOD_DEFINITION
#define METHOD_RETURN_EventLatchMethod    int
#define METHOD_CONST_EventLatchMethod
#define METHOD_ARG_LIST_EventLatchMethod  (     \
    bool                         aEnabled_,     \
    const struct EventClockTime *aPollTime_)
#define METHOD_CALL_LIST_EventLatchMethod (aEnabled_, aPollTime_)

#define METHOD_NAME      EventLatchMethod
#define METHOD_RETURN    METHOD_RETURN_EventLatchMethod
#define METHOD_CONST     METHOD_CONST_EventLatchMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_EventLatchMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_EventLatchMethod
#include "method_.h"

#define EventLatchMethod(Object_, Method_)     \
    METHOD_TRAMPOLINE(                         \
        Object_, Method_,                      \
        EventLatchMethod_,                     \
        METHOD_RETURN_EventLatchMethod,        \
        METHOD_CONST_EventLatchMethod,         \
        METHOD_ARG_LIST_EventLatchMethod,      \
        METHOD_CALL_LIST_EventLatchMethod)

/* -------------------------------------------------------------------------- */
BEGIN_C_SCOPE;

struct EventPipe;
struct EventLatch;

struct EventLatchListEntry
{
    struct EventLatch              *mLatch;
    struct EventLatchMethod         mMethod;
    LIST_ENTRY(EventLatchListEntry) mEntry;
};

struct EventLatchList
{
    LIST_HEAD(, EventLatchListEntry) mList;
};

struct EventLatch
{
    struct ThreadSigMutex       mMutex_;
    struct ThreadSigMutex      *mMutex;
    unsigned                    mEvent;
    struct EventPipe           *mPipe;
    char                       *mName;
    struct EventLatchListEntry  mList;
};

enum EventLatchSetting
{
    EventLatchSettingError    = -1,
    EventLatchSettingDisabled =  0,
    EventLatchSettingOff      =  1,
    EventLatchSettingOn       =  2,
};

/* -------------------------------------------------------------------------- */
CHECKED int
pollEventLatchListEntry(struct EventLatchListEntry  *self,
                        const struct EventClockTime *aPollTime);

/* -------------------------------------------------------------------------- */
CHECKED int
createEventLatch(struct EventLatch *self, const char *aName);

CHECKED struct EventLatch *
closeEventLatch(struct EventLatch *self);

int
printEventLatch(const struct EventLatch *self, FILE *aFile);

CHECKED enum EventLatchSetting
bindEventLatchPipe(struct EventLatch       *self,
                   struct EventPipe        *aPipe,
                   struct EventLatchMethod  aMethod);

CHECKED enum EventLatchSetting
unbindEventLatchPipe(struct EventLatch *self);

CHECKED enum EventLatchSetting
disableEventLatch(struct EventLatch *self);

CHECKED enum EventLatchSetting
setEventLatch(struct EventLatch *self);

CHECKED enum EventLatchSetting
resetEventLatch(struct EventLatch *self);

enum EventLatchSetting
ownEventLatchSetting(const struct EventLatch *self);

/* -------------------------------------------------------------------------- */

END_C_SCOPE;

#endif /* EVENTLATCH_H */
