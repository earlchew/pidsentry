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

#include "deadline_.h"

/* -------------------------------------------------------------------------- */
int
createDeadline(struct Deadline *self, const struct Duration *aDuration)
{
    int rc = -1;

    self->mSince          = (struct EventClockTime) EVENTCLOCKTIME_INIT;
    self->mTime           = self->mSince;
    self->mRemaining      = ZeroDuration;
    self->mSigContTracker = ProcessSigContTracker();
    self->mDuration_      = aDuration ? *aDuration : ZeroDuration;
    self->mDuration       = aDuration ? &self->mDuration_ : 0;
    self->mExpired        = false;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            self = closeDeadline(self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct Deadline *
closeDeadline(struct Deadline *self)
{
    return 0;
}

/* -------------------------------------------------------------------------- */
int
checkDeadlineExpired(struct Deadline *self,
                     struct DeadlinePollMethod aPollMethod,
                     struct DeadlineWaitMethod aWaitMethod)
{
    int rc = -1;

    self->mTime = eventclockTime();

    int ready;

    TEST_RACE
    ({
        do
        {
            /* In case the process is stopped after the time is
             * latched, check once more if the fds are ready
             * before checking the deadline. */

            ready = -1;

            ERROR_IF(
                (ready = callDeadlinePollMethod(aPollMethod),
                 -1 == ready));

            if (self->mDuration)
            {
                /* Rely on deadlineTimeExpired() to always indicate
                 * that the deadline has not yet expired on the first
                 * iteration. */

                if (deadlineTimeExpired(
                        &self->mSince,
                        *self->mDuration, &self->mRemaining, &self->mTime))
                {
                    if (checkProcessSigContTracker(&self->mSigContTracker))
                    {
                        self->mSince =
                            (struct EventClockTime) EVENTCLOCKTIME_INIT;

                        ready = 0;
                        break;
                    }

                    self->mExpired = true;

                    ready = -ETIMEDOUT;
                    break;
                }
            }

            if ( ! ready)
                ERROR_IF(
                    (ready = callDeadlineWaitMethod(
                        aWaitMethod,
                        self->mDuration ? &self->mRemaining : 0),
                     -1 == ready));

            ensure(1 == ready || 0 == ready);

        } while (0);
    });

    /* The return value covers the following states:
     *
     *  1  The deadline has not expired, no error occurred, and the
     *     underlying event is ready.
     *
     *  0  The deadline has not expired, no error occurred, and the
     *     underlying event is not ready.
     *
     * -1  Either the deadline timed out or an error occurred. If the
     *     deadline expired, ownDeadlineExpired() will return true and
     *     errno will be set to ETIMEDOUT. If another error occurred,
     *     ownDeadlineExpired() will return false, and errno will take
     *     on an arbitrary value. */

    if (0 > ready)
    {
        errno = -ready;
        ready = -1;
    }

    rc = ready;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
bool
ownDeadlineExpired(const struct Deadline *self)
{
    return self->mExpired;
}

/* -------------------------------------------------------------------------- */
