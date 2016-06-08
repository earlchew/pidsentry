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
    self->mMutex     = createThreadSigMutex(&self->mMutex_);

    ERROR_IF(
        createPipe(&self->mPipe_, aFlags));
    self->mPipe = &self->mPipe_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            self->mPipe  = closePipe(self->mPipe);
            self->mMutex = destroyThreadSigMutex(self->mMutex);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct EventPipe *
closeEventPipe(struct EventPipe *self)
{
    if (self)
    {
        self->mPipe  = closePipe(self->mPipe);
        self->mMutex = destroyThreadSigMutex(self->mMutex);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
int
setEventPipe(struct EventPipe *self)
{
    int rc = -1;

    struct ThreadSigMutex *lock = lockThreadSigMutex(self->mMutex);

    int signalled = 0;

    if ( ! self->mSignalled)
    {
        char buf[1] = { 0 };

        ssize_t rv = 0;
        ERROR_IF(
            (rv = writeFile(self->mPipe->mWrFile, buf, 1, 0),
             1 != rv),
            {
                errno = -1 == rv ? errno : EIO;
            });

        self->mSignalled = true;
        signalled        = 1;
    }

    rc = signalled;

Finally:

    FINALLY
    ({
        lock = unlockThreadSigMutex(lock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
resetEventPipe(struct EventPipe *self)
{
    int rc = -1;

    struct ThreadSigMutex *lock = lockThreadSigMutex(self->mMutex);

    int signalled = 0;

    if (self->mSignalled)
    {
        char buf[1];

        ssize_t rv = 0;
        ERROR_IF(
            (rv = readFile(self->mPipe->mRdFile, buf, 1, 0),
             1 != rv),
            {
                errno = -1 == rv ? errno : EIO;
            });

        ensure( ! buf[0]);

        self->mSignalled = false;
        signalled        = 1;
    }

    rc = signalled;

Finally:

    FINALLY
     ({
         lock = unlockThreadSigMutex(lock);
     });

    return rc;
}

/* -------------------------------------------------------------------------- */
