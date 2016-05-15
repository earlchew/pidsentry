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
#ifndef EVENTQUEUE_H
#define EVENTQUEUE_H

#include "int_.h"
#include "file_.h"
#include "method_.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Duration;

enum EventQueuePollTrigger
{
    EventQueuePollDisconnect,
    EventQueuePollRead,
    EventQueuePollWrite,
    EventQueuePollTriggers,
};

/* -------------------------------------------------------------------------- */
struct EventQueueHandle
EventQueueHandle_(void *aHandle);

struct EventQueueHandle
{
#ifdef __cplusplus
    EventQueueHandle()
    { }

    explicit EventQueueHandle(void *aHandle)
    { *this = EventQueueHandle_(aHandle); }
#endif

    void *mHandle;
};

#ifndef __cplusplus
static inline struct EventQueueHandle
EventQueueHandle(void *aHandle)
{ return EventQueueHandle_(aHandle); }
#endif

/* -------------------------------------------------------------------------- */
struct EventQueue
{
    struct File  mFile_;
    struct File *mFile;
};

struct EventQueueFile
{
    struct EventQueue       *mQueue;
    struct File             *mFile;
    unsigned                 mEvents;
    struct EventQueueHandle  mSubject;
};

/* -------------------------------------------------------------------------- */
INT
createEventQueue(struct EventQueue *self);

struct EventQueue *
closeEventQueue(struct EventQueue *self);

INT
pushEventQueue(struct EventQueue     *self,
               struct EventQueueFile *aEvent);

INT
popEventQueue(struct EventQueue      *self,
              struct EventQueueFile **aEvent,
              size_t                  aEvents,
              const struct Duration  *aTimeout);

/* -------------------------------------------------------------------------- */
INT
createEventQueueFile(struct EventQueueFile     *self,
                     struct EventQueue         *aQueue,
                     struct File               *aFile,
                     enum EventQueuePollTrigger aTrigger,
                     struct EventQueueHandle    aSubject);

struct EventQueueFile *
closeEventQueueFile(struct EventQueueFile *self);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* EVENTQUEUE_H */
