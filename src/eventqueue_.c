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

#include "eventqueue_.h"
#include "error_.h"

#include <sys/epoll.h>

/* -------------------------------------------------------------------------- */
static uint32_t pollTriggers_[EventQueuePollTriggers] =
{
    [EventQueuePollDisconnect] = (EPOLLHUP | EPOLLERR),
    [EventQueuePollRead]       = (EPOLLHUP | EPOLLERR | EPOLLPRI | EPOLLIN),
    [EventQueuePollWrite]      = (EPOLLHUP | EPOLLERR | EPOLLOUT),
};

/* -------------------------------------------------------------------------- */
struct EventQueueHandle
EventQueueHandle_(void *aHandle)
{
    return (struct EventQueueHandle) { .mHandle = aHandle };
}

/* -------------------------------------------------------------------------- */
int
createEventQueue(struct EventQueue *self)
{
    int rc = -1;

    self->mFile = 0;

    ERROR_IF(
        createFile(
            &self->mFile_,
            epoll_create1(EPOLL_CLOEXEC)));
    self->mFile = &self->mFile_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            closeFile(self->mFile);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closeEventQueue(struct EventQueue *self)
{
    if (self)
    {
        closeFile(self->mFile);
    }
}

/* -------------------------------------------------------------------------- */
int
pushEventQueue(struct EventQueue     *self,
               struct EventQueueFile *aEvent)
{
    int rc = -1;

    int ctlOp;

    if (aEvent->mEvents & EPOLLONESHOT)
        ctlOp = EPOLL_CTL_MOD;
    else
    {
        ctlOp = EPOLL_CTL_ADD;
        aEvent->mEvents |= EPOLLONESHOT;
    }

    struct epoll_event pollEvent =
    {
        .events = aEvent->mEvents,
        .data   = { .ptr = aEvent },
    };

    ERROR_IF(
        epoll_ctl(self->mFile->mFd, ctlOp, aEvent->mFile->mFd, &pollEvent));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
popEventQueue(struct EventQueue      *self,
              struct EventQueueFile **aEvent,
              size_t                  aEvents,
              const struct Duration  *aTimeout)
{
    int rc = -1;

    int polledEvents = 0;

    if (aEvents)
    {
        size_t maxPollEvents = 1024;

        struct epoll_event pollEvents[
            maxPollEvents > aEvents ? aEvents : maxPollEvents];

        int timeout_ms = -1;

        if (aTimeout)
        {
            struct MilliSeconds timeoutDuration = MSECS(aTimeout->duration);

            timeout_ms = timeoutDuration.ms;

            if (0 > timeout_ms || timeoutDuration.ms != timeout_ms)
                timeout_ms = INT_MAX;
        }

        ERROR_IF(
            (polledEvents = epoll_wait(
               self->mFile->mFd, pollEvents, NUMBEROF(pollEvents), timeout_ms),
             -1 == polledEvents));

        for (int px = 0; px < polledEvents; ++px)
            aEvent[px] = pollEvents[px].data.ptr;
    }

    rc = polledEvents;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
createEventQueueFile(struct EventQueueFile     *self,
                     struct EventQueue         *aQueue,
                     struct File               *aFile,
                     enum EventQueuePollTrigger aTrigger,
                     struct EventQueueHandle    aSubject)
{
    int rc = -1;

    self->mQueue   = aQueue;
    self->mFile    = aFile;
    self->mEvents  = pollTriggers_[aTrigger];
    self->mSubject = aSubject;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closeEventQueueFile(struct EventQueueFile *self)
{
    if (self)
    {
        ABORT_IF(
            epoll_ctl(
                self->mQueue->mFile->mFd, EPOLL_CTL_DEL, self->mFile->mFd, 0) &&
            ENOENT != errno);
    }
}


/* -------------------------------------------------------------------------- */
