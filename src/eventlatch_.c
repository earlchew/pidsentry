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
#include "macros_.h"

#include <errno.h>

/* -------------------------------------------------------------------------- */
#define EVENTLATCH_DISABLE_BIT_ 0
#define EVENTLATCH_DATA_BIT_    1

#define EVENTLATCH_DISABLE_MASK_ (1u << EVENTLATCH_DISABLE_BIT_)
#define EVENTLATCH_DATA_MASK_    (1u << EVENTLATCH_DATA_BIT_)

/* -------------------------------------------------------------------------- */
void
bindEventLatchPipe(struct EventLatch *self, struct EventPipe *aPipe)
{
    lockMutex(&self->mMutex);
    self->mPipe = aPipe;
    unlockMutex(&self->mMutex);
}

/* -------------------------------------------------------------------------- */
int
disableEventLatch(struct EventLatch *self)
{
    int rc = -1;

    lockMutex(&self->mMutex);

    unsigned event = self->mEvent;
    if (event & EVENTLATCH_DISABLE_MASK_)
        rc = 0;
    else
    {
        self->mEvent |= EVENTLATCH_DISABLE_MASK_;

        if (self->mPipe)
        {
            if (-1 == setEventPipe(self->mPipe))
                goto Finally;
        }

        rc = 1;
    }

Finally:

    FINALLY
    ({
        unlockMutex(&self->mMutex);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
setEventLatch(struct EventLatch *self)
{
    int rc = -1;

    lockMutex(&self->mMutex);

    unsigned event = self->mEvent;
    if (event & EVENTLATCH_DISABLE_MASK_)
    {
        errno = ERANGE;
        goto Finally;
    }

    if (event & EVENTLATCH_DATA_MASK_)
        rc = 0;
    else
    {
        self->mEvent ^= EVENTLATCH_DATA_MASK_;

        if (self->mPipe)
        {
            if (-1 == setEventPipe(self->mPipe))
                goto Finally;
        }

        rc = 1;
    }

Finally:

    FINALLY
    ({
        unlockMutex(&self->mMutex);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
resetEventLatch(struct EventLatch *self)
{
    int rc = -1;

    lockMutex(&self->mMutex);

    unsigned event = self->mEvent;
    if (event & EVENTLATCH_DISABLE_MASK_)
    {
        errno = ERANGE;
        goto Finally;
    }

    if ( ! (event & EVENTLATCH_DATA_MASK_))
        rc = 0;
    else
    {
        self->mEvent ^= EVENTLATCH_DATA_MASK_;
        rc = 1;
    }

Finally:

    FINALLY
    ({
        unlockMutex(&self->mMutex);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
ownEventLatchSetting(const struct EventLatch *self_)
{
    int rc = -1;

    struct EventLatch *self = (struct EventLatch *) self_;

    lockMutex(&self->mMutex);

    unsigned event = self->mEvent;
    if (event & EVENTLATCH_DISABLE_MASK_)
    {
        errno = ERANGE;
        goto Finally;
    }

    rc = (event & EVENTLATCH_DATA_MASK_) ? 1 : 0;

Finally:

    FINALLY
    ({
        unlockMutex(&self->mMutex);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
