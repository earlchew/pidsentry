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
#ifndef FILEEVENTQUEUE_H
#define FILEEVENTQUEUE_H

#include "compiler_.h"
#include "file_.h"
#include "method_.h"

/* -------------------------------------------------------------------------- */
BEGIN_C_SCOPE;
struct FileEventQueue;
END_C_SCOPE;

#define METHOD_DEFINITION
#define METHOD_RETURN_FileEventQueueActivityMethod    int
#define METHOD_CONST_FileEventQueueActivityMethod
#define METHOD_ARG_LIST_FileEventQueueActivityMethod  ()
#define METHOD_CALL_LIST_FileEventQueueActivityMethod ()

#define METHOD_NAME      FileEventQueueActivityMethod
#define METHOD_RETURN    METHOD_RETURN_FileEventQueueActivityMethod
#define METHOD_CONST     METHOD_CONST_FileEventQueueActivityMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_FileEventQueueActivityMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_FileEventQueueActivityMethod
#include "method_.h"

#define FileEventQueueActivityMethod(Method_, Object_)  \
    METHOD_TRAMPOLINE(                                  \
        Method_, Object_,                               \
        FileEventQueueActivityMethod_,                  \
        METHOD_RETURN_FileEventQueueActivityMethod,     \
        METHOD_CONST_FileEventQueueActivityMethod,      \
        METHOD_ARG_LIST_FileEventQueueActivityMethod,   \
        METHOD_CALL_LIST_FileEventQueueActivityMethod)

/* -------------------------------------------------------------------------- */
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
struct FileEventQueue
{
    struct File         mFile_;
    struct File        *mFile;
    struct epoll_event *mQueue;
    int                 mQueueSize;
    int                 mQueuePending;
    int                 mNumArmed;
    int                 mNumPending;
};

struct FileEventQueueActivity
{
    struct FileEventQueue               *mQueue;
    struct File                         *mFile;
    struct epoll_event                  *mPending;
    unsigned                             mArmed;
    struct FileEventQueueActivityMethod  mMethod;
};

/* -------------------------------------------------------------------------- */
CHECKED int
createFileEventQueue(struct FileEventQueue *self, int aQueueSize);

CHECKED struct FileEventQueue *
closeFileEventQueue(struct FileEventQueue *self);

CHECKED int
pollFileEventQueueActivity(struct FileEventQueue *self,
                           const struct Duration *aTimeout);

/* -------------------------------------------------------------------------- */
CHECKED int
createFileEventQueueActivity(struct FileEventQueueActivity *self,
                             struct FileEventQueue         *aQueue,
                             struct File                   *aFile);

CHECKED int
armFileEventQueueActivity(struct FileEventQueueActivity      *self,
                          enum EventQueuePollTrigger          aTrigger,
                          struct FileEventQueueActivityMethod aMethod);

CHECKED struct FileEventQueueActivity *
closeFileEventQueueActivity(struct FileEventQueueActivity *self);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* FILEEVENTQUEUE_H */
