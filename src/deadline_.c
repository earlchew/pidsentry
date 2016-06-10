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

    int expired = -1;

    self->mTime = eventclockTime();

    TEST_RACE
    ({
        int ready;

        ready = -1;
        ERROR_IF(
            (ready = callDeadlinePollMethod(aPollMethod),
             -1 == ready));

        if ( ! ready)
        {
            if (self->mDuration)
            {
                if (deadlineTimeExpired(
                        &self->mSince,
                        *self->mDuration, &self->mRemaining, &self->mTime))
                {
                    if (checkProcessSigContTracker(&self->mSigContTracker))
                    {
                        self->mSince =
                            (struct EventClockTime) EVENTCLOCKTIME_INIT;

                        expired = 0;
                        break;
                    }

                    expired = 1;
                    break;
                }
            }

            ERROR_IF(
                (ready = callDeadlineWaitMethod(
                    aWaitMethod,
                    self->mDuration ? &self->mRemaining : 0),
                 -1 == ready));
        }

        expired = ready;
    });

    ensure(1 == expired || 0 == expired);

    rc = expired;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
bool
ownDeadlineExpired(const struct Deadline *self)
{
    return self->mSince.eventclock.ns && ! self->mRemaining.duration.ns;
}

/* -------------------------------------------------------------------------- */
