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

#include "eventlatch_.h"
#include "eventpipe_.h"
#include "thread_.h"
#include "error_.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
#define EVENTLATCH_DISABLE_BIT_ 0
#define EVENTLATCH_DATA_BIT_    1

#define EVENTLATCH_DISABLE_MASK_ (1u << EVENTLATCH_DISABLE_BIT_)
#define EVENTLATCH_DATA_MASK_    (1u << EVENTLATCH_DATA_BIT_)

/* -------------------------------------------------------------------------- */
int
createEventLatch(struct EventLatch *self, const char *aName)
{
    int rc = -1;

    self->mMutex = createThreadSigMutex(&self->mMutex_);
    self->mEvent = 0;
    self->mPipe  = 0;
    self->mName  = 0;
    self->mList  = (struct EventLatchListEntry)
    {
        .mMethod = EventLatchMethodNil(),
        .mLatch = self,
    };

    ERROR_UNLESS(
        self->mName = strdup(aName));

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            self = closeEventLatch(self);
    });

    return 0;
}

/* -------------------------------------------------------------------------- */
struct EventLatch *
closeEventLatch(struct EventLatch *self)
{
    if (self)
    {
        if (self->mPipe)
            ABORT_IF(
                EventLatchSettingError == unbindEventLatchPipe(self));

        self->mMutex = destroyThreadSigMutex(self->mMutex);

        free(self->mName);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
int
printEventLatch(const struct EventLatch *self, FILE *aFile)
{
    return fprintf(aFile, "<%p %s>", self, self->mName);
}

/* -------------------------------------------------------------------------- */
static int
signalEventLatch_(struct EventLatch *self)
{
    int rc = -1;

    if (self->mPipe)
    {
        int signalled;

        do
        {
            signalled = -1;

            ERROR_IF(
                (signalled = setEventPipe(self->mPipe),
                 -1 == signalled && EINTR != errno));

        } while (-1 == signalled);
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static enum EventLatchSetting
bindEventLatchPipe_(struct EventLatch       *self,
                    struct EventPipe        *aPipe,
                    struct EventLatchMethod  aMethod)
{
    enum EventLatchSetting rc = EventLatchSettingError;

    struct ThreadSigMutex *lock = lockThreadSigMutex(self->mMutex);

    enum EventLatchSetting setting;

    unsigned event = self->mEvent;

    if (event & EVENTLATCH_DISABLE_MASK_)
        setting = EventLatchSettingDisabled;
    else
        setting = (event & EVENTLATCH_DATA_MASK_)
            ? EventLatchSettingOn
            : EventLatchSettingOff;

    if (self->mPipe != aPipe)
    {
        if (self->mPipe)
        {
            detachEventPipeLatch_(self->mPipe, &self->mList);
            self->mList.mMethod = EventLatchMethodNil();
        }

        self->mPipe = aPipe;

        if (self->mPipe)
        {
            self->mList.mMethod = aMethod;
            attachEventPipeLatch_(self->mPipe, &self->mList);

            if (EventLatchSettingOff != setting)
                ERROR_IF(
                    signalEventLatch_(self));
        }
    }

    rc = setting;

Finally:

    FINALLY
    ({
        lock = unlockThreadSigMutex(lock);
    });

    return rc;
}

enum EventLatchSetting
bindEventLatchPipe(struct EventLatch       *self,
                   struct EventPipe        *aPipe,
                   struct EventLatchMethod  aMethod)
{
    ensure(aPipe);
    ensure( ! self->mPipe);

    return bindEventLatchPipe_(self, aPipe, aMethod);
}

enum EventLatchSetting
unbindEventLatchPipe(struct EventLatch *self)
{
    return bindEventLatchPipe_(self, 0, EventLatchMethodNil());
}

/* -------------------------------------------------------------------------- */
enum EventLatchSetting
disableEventLatch(struct EventLatch *self)
{
    enum EventLatchSetting rc = EventLatchSettingError;

    struct ThreadSigMutex *lock = lockThreadSigMutex(self->mMutex);

    enum EventLatchSetting setting;

    unsigned event = self->mEvent;

    if (event & EVENTLATCH_DISABLE_MASK_)
        setting = EventLatchSettingDisabled;
    else
    {
        setting = (event & EVENTLATCH_DATA_MASK_)
            ? EventLatchSettingOn
            : EventLatchSettingOff;

        ERROR_IF(
            signalEventLatch_(self));

        self->mEvent = event ^ EVENTLATCH_DISABLE_MASK_;
    }

    rc = setting;

Finally:

    FINALLY
    ({
        lock = unlockThreadSigMutex(lock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
enum EventLatchSetting
setEventLatch(struct EventLatch *self)
{
    enum EventLatchSetting rc = EventLatchSettingError;

    struct ThreadSigMutex *lock = lockThreadSigMutex(self->mMutex);

    enum EventLatchSetting setting;

    unsigned event = self->mEvent;

    if (event & EVENTLATCH_DISABLE_MASK_)
        setting = EventLatchSettingDisabled;
    else if (event & EVENTLATCH_DATA_MASK_)
        setting = EventLatchSettingOn;
    else
    {
        setting = EventLatchSettingOff;

        ERROR_IF(
            signalEventLatch_(self));

        self->mEvent = event ^ EVENTLATCH_DATA_MASK_;
    }

    rc = setting;

Finally:

    FINALLY
    ({
        lock = unlockThreadSigMutex(lock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
enum EventLatchSetting
resetEventLatch(struct EventLatch *self)
{
    enum EventLatchSetting rc = EventLatchSettingError;

    struct ThreadSigMutex *lock = lockThreadSigMutex(self->mMutex);

    enum EventLatchSetting setting;

    unsigned event = self->mEvent;

    if (event & EVENTLATCH_DISABLE_MASK_)
        setting = EventLatchSettingDisabled;
    else if ( ! (event & EVENTLATCH_DATA_MASK_))
        setting = EventLatchSettingOff;
    else
    {
        setting = EventLatchSettingOn;

        self->mEvent = event ^ EVENTLATCH_DATA_MASK_;
    }

    rc = setting;

Finally:

    FINALLY
    ({
        lock = unlockThreadSigMutex(lock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
enum EventLatchSetting
ownEventLatchSetting(const struct EventLatch *self_)
{
    enum EventLatchSetting rc = EventLatchSettingError;

    struct EventLatch *self = (struct EventLatch *) self_;

    struct ThreadSigMutex *lock = lockThreadSigMutex(self->mMutex);

    enum EventLatchSetting setting;

    unsigned event = self->mEvent;

    if (event & EVENTLATCH_DISABLE_MASK_)
        setting = EventLatchSettingDisabled;
    else
        setting = (event & EVENTLATCH_DATA_MASK_)
            ? EventLatchSettingOn
            : EventLatchSettingOff;

    rc = setting;

Finally:

    FINALLY
    ({
        lock = unlockThreadSigMutex(lock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
pollEventLatchListEntry(struct EventLatchListEntry  *self,
                        const struct EventClockTime *aPollTime)
{
    int rc = -1;

    int called = 0;

    if (self->mLatch)
    {
        enum EventLatchSetting setting;
        ERROR_IF(
            (setting = resetEventLatch(self->mLatch),
             EventLatchSettingError == setting),
            {
                warn(errno,
                     "Unable to reset event latch %" PRIs_Method,
                     FMTs_Method(printEventLatch, self->mLatch));
            });

        if (EventLatchSettingOff != setting)
        {
            bool enabled;

            if (EventLatchSettingOn == setting)
                enabled = true;
            else
            {
                ensure(EventLatchSettingDisabled == setting);

                self->mLatch = 0;

                enabled = false;
            }

            called = 1;

            ERROR_IF(
                callEventLatchMethod(self->mMethod, enabled, aPollTime));
        }
    }

    rc = called;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
