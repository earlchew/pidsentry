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
#include "thread_.h"
#include "timekeeping_.h"

#include <errno.h>

/* -------------------------------------------------------------------------- */
int
createEventPipe(struct EventPipe *self, unsigned aFlags)
{
    int rc = -1;

    self->mPipe      = 0;
    self->mSignalled = false;
    self->mMutex     = createMutex(&self->mMutex_);

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

            self->mMutex = destroyMutex(self->mMutex);
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

        self->mMutex = destroyMutex(self->mMutex);
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

    pthread_mutex_t *lock = lockMutex(self->mMutex);

    int signalled = 0;

    if ( ! self->mSignalled)
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

        self->mSignalled = true;
        signalled        = 1;
    }

    rc = signalled;

Finally:

    FINALLY
    ({
        lock = unlockMutex(lock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
resetEventPipe(struct EventPipe *self)
{
    int rc = -1;

    pthread_mutex_t *lock = lockMutex(self->mMutex);

    int signalled = 0;

    if (self->mSignalled)
    {
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

        self->mSignalled = false;
        signalled        = 1;
    }

    rc = signalled;

Finally:

    FINALLY
     ({
         lock = unlockMutex(lock);
     });

    return rc;
}

/* -------------------------------------------------------------------------- */
