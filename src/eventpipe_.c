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

#include "eventpipe_.h"
#include "macros_.h"
#include "error_.h"
#include "timekeeping_.h"

#include <errno.h>

/* -------------------------------------------------------------------------- */
#define EVENTPIPE_PENDING_BIT_   0
#define EVENTPIPE_SIGNALLED_BIT_ 1

#define EVENTPIPE_PENDING_MASK_   (1u << EVENTPIPE_PENDING_BIT_)
#define EVENTPIPE_SIGNALLED_MASK_ (1u << EVENTPIPE_SIGNALLED_BIT_)

/* -------------------------------------------------------------------------- */
int
createEventPipe(struct EventPipe *self, unsigned aFlags)
{
    int rc = -1;

    self->mPipe   = 0;
    self->mEvents = 0;

    if (createPipe(&self->mPipe_, aFlags))
        goto Finally;
    self->mPipe = &self->mPipe_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            if (closePipe(self->mPipe))
                terminate(
                    errno,
                    "Unable to close pipe");
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closeEventPipe(struct EventPipe *self)
{
    int rc = -1;

    if (self)
    {
        if (closePipe(self->mPipe))
            goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
setEventPipe(struct EventPipe *self)
{
    int rc = -1;

    /* Be prepared for multiple writers and only allow the first writer
     * to signal the reader on the underlying pipe. */

    if ( ! (EVENTPIPE_PENDING_MASK_ &
            __sync_fetch_and_or(&self->mEvents, EVENTPIPE_PENDING_MASK_)))
    {
        char buf[1] = { 0 };

        switch (writeFile(self->mPipe->mWrFile, buf, 1))
        {
        default:
            errno = EIO;
            /* Fall through */

        case -1:
            goto Finally;

        case 1:
            break;
        }

        ensure(
            __sync_fetch_and_or(
                &self->mEvents,
                EVENTPIPE_SIGNALLED_MASK_) == EVENTPIPE_PENDING_MASK_);
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
resetEventPipe(struct EventPipe *self)
{
    int rc = -1;

    /* Be prepared for multiple readers and only allow the first reader
     * to read the signal from the underlying pipe. */

    int signalled = 0;

    while (1)
    {
        unsigned events = self->mEvents;

        if ( ! (events & EVENTPIPE_SIGNALLED_MASK_))
            break;

        ensure(
            (EVENTPIPE_PENDING_MASK_ | EVENTPIPE_SIGNALLED_MASK_) == events);

        if ( ! __sync_fetch_and_and(&self->mEvents, 0))
            continue;

        char buf[1];

        switch (readFile(self->mPipe->mRdFile, buf, 1))
        {
        default:
            errno = EIO;
            /* Fall through */

        case -1:
            goto Finally;

        case 1:
            break;
        }

        ensure( ! buf[0]);

        signalled = 1;
        break;
    }

    rc = signalled;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
