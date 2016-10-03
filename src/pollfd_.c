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

#include "pollfd_.h"
#include "error_.h"

#include <string.h>

#include <sys/poll.h>

/* -------------------------------------------------------------------------- */
static char *
pollEventTextBit_(char *aBuf, unsigned *aMask, unsigned aBit, const char *aText)
{
    char *buf = aBuf;

    if (*aMask & aBit)
    {
        *aMask ^= aBit;
        *buf++ = ' ';
        buf = stpcpy(buf, aText + sizeof("POLL") - 1);
    }

    return buf;
}

#define pollEventTextBit_(aBuf, aMask, aBit) \
    pollEventTextBit_((aBuf), (aMask), (aBit), # aBit)

const char *
createPollEventText(
    struct PollEventText *aPollEventText, unsigned aPollEventMask)
{
    unsigned mask = aPollEventMask;

    char *buf = aPollEventText->mText;

    buf[0] = 0;
    buf[1] = 0;

    buf = pollEventTextBit_(buf, &mask, POLLIN);
    buf = pollEventTextBit_(buf, &mask, POLLPRI);
    buf = pollEventTextBit_(buf, &mask, POLLOUT);
    buf = pollEventTextBit_(buf, &mask, POLLERR);
    buf = pollEventTextBit_(buf, &mask, POLLHUP);
    buf = pollEventTextBit_(buf, &mask, POLLNVAL);

    if (mask)
    {
        if (0 > sprintf(buf, " 0x%x", mask))
        {
            buf[0] = ' ';
            buf[1] = '?';
            buf[2] = 0;
        }
    }

    return aPollEventText->mText + 1;
}

/* -------------------------------------------------------------------------- */
int
createPollFd(struct PollFd                 *self,
             struct pollfd                 *aPoll,
             struct PollFdAction           *aFdActions,
             const char * const            *aFdNames,
             size_t                         aNumFdActions,
             struct PollFdTimerAction      *aTimerActions,
             const char * const            *aTimerNames,
             size_t                         aNumTimerActions,
             struct PollFdCompletionMethod  aCompletionQuery)
{
    int rc = -1;

    self->mPoll = aPoll;

    self->mCompletionQuery = aCompletionQuery;

    self->mFdActions.mActions = aFdActions;
    self->mFdActions.mNames   = aFdNames;
    self->mFdActions.mSize    = aNumFdActions;

    self->mTimerActions.mActions = aTimerActions;
    self->mTimerActions.mNames   = aTimerNames;
    self->mTimerActions.mSize    = aNumTimerActions;

    rc = 0;

    return rc;
}

/* -------------------------------------------------------------------------- */
struct PollFd *
closePollFd(struct PollFd *self)
{
    return 0;
}

/* -------------------------------------------------------------------------- */
int
runPollFdLoop(struct PollFd *self)
{
    int rc = -1;

    struct EventClockTime polltm;

    while ( ! callPollFdCompletionMethod(self->mCompletionQuery))
    {
        /* Poll the file descriptors and process the file descriptor
         * events before attempting to check for timeouts. This
         * order of operations is important to deal robustly with
         * slow clocks and stoppages. */

        polltm = eventclockTime();

        struct Duration timeout   = ZeroDuration;
        size_t          chosen    = self->mTimerActions.mSize;
        size_t          numActive = 0;

        for (size_t ix = 0; self->mTimerActions.mSize > ix; ++ix)
        {
            if (self->mTimerActions.mActions[ix].mPeriod.duration.ns)
            {
                ++numActive;

                struct Duration remaining;

                if (deadlineTimeExpired(
                        &self->mTimerActions.mActions[ix].mSince,
                        self->mTimerActions.mActions[ix].mPeriod,
                        &remaining,
                        &polltm))
                {
                    chosen  = ix;
                    timeout = ZeroDuration;
                    break;
                }

                if (timeout.duration.ns >
                    remaining.duration.ns || ! timeout.duration.ns)
                {
                    chosen  = ix;
                    timeout = remaining;
                }
            }
        }

        if (self->mTimerActions.mSize != chosen)
            debug(1, "choose %s deadline", self->mTimerActions.mNames[chosen]);

        int timeout_ms;

        if ( ! numActive)
            timeout_ms = -1;
        else
        {
            struct MilliSeconds timeoutDuration = MSECS(timeout.duration);

            timeout_ms = timeoutDuration.ms;

            if (0 > timeout_ms || timeoutDuration.ms != timeout_ms)
                timeout_ms = INT_MAX;
        }

        debug(1, "poll wait %dms", timeout_ms);

        {
            int events;
            ERROR_IF(
                (events = poll(
                     self->mPoll, self->mFdActions.mSize, timeout_ms),
                 -1 == events && EINTR != errno));
        }

        /* Latch the event clock time here before quickly polling the
         * file descriptors again. Deadlines will be compared against
         * this latched time */

        polltm = eventclockTime();

        int events;
        TEST_RACE
        ({
            while (1)
            {
                ERROR_IF(
                    (events = poll(
                        self->mPoll, self->mFdActions.mSize, 0),
                     -1 == events && EINTR != errno));
                if (-1 != events)
                    break;
            }
        });

        {
            /* When processing file descriptor events, do not loop in EINTR
             * but instead allow the polling cycle to be re-run so that
             * the event loop will not remain stuck processing a single
             * file descriptor. */

            unsigned eventCount = ! events;

            /* The poll(2) call will mark POLLNVAL, POLLERR or POLLHUP
             * no matter what the caller has subscribed for. Only pay
             * attention to what was subscribed. */

            debug(1, "polled event count %d", events);

            for (size_t ix = 0; self->mFdActions.mSize > ix; ++ix)
            {
                struct PollEventText pollEventText;
                struct PollEventText pollRcvdEventText;

                debug(
                    1,
                    "poll %s %d (%s) (%s)",
                    self->mFdActions.mNames[ix],
                    self->mPoll[ix].fd,
                    createPollEventText(
                        &pollEventText, self->mPoll[ix].events),
                    createPollEventText(
                        &pollRcvdEventText, self->mPoll[ix].revents));

                self->mPoll[ix].revents &= self->mPoll[ix].events;

                if (self->mPoll[ix].revents)
                {
                    ensure(rc);

                    ++eventCount;

                    if ( ! ownPollFdCallbackMethodNil(
                             self->mFdActions.mActions[ix].mAction))
                        ERROR_IF(
                            callPollFdCallbackMethod(
                                self->mFdActions.mActions[ix].mAction, &polltm),
                            {
                                warn(
                                    errno,
                                    "Error dispatching %s",
                                    self->mFdActions.mNames[ix]);
                            });
                }
            }

            /* Ensure that the interpretation of the poll events is being
             * correctly handled, to avoid a busy-wait poll loop. */

            ensure(eventCount);
        }

        /* With the file descriptors processed, any timeouts have had
         * a chance to be recalibrated, and now the timers can be
         * processed. */

        for (size_t ix = 0; self->mTimerActions.mSize > ix; ++ix)
        {
            struct PollFdTimerAction *timerAction =
                &self->mTimerActions.mActions[ix];

            if (timerAction->mPeriod.duration.ns)
            {
                if (deadlineTimeExpired(
                        &timerAction->mSince, timerAction->mPeriod, 0, &polltm))
                {
                    /* Compute the lap time, and as a side-effect set
                     * the deadline for the next timer cycle. This means
                     * that the timer action need not do anything to
                     * prepare for the next timer cycle, unless it needs
                     * to cancel or otherwise reschedule the timer. */

                    (void) lapTimeSince(
                        &timerAction->mSince, timerAction->mPeriod, &polltm);

                    debug(
                        1, "expire %s timer with period %" PRIs_MilliSeconds,
                        self->mTimerActions.mNames[ix],
                        FMTs_MilliSeconds(
                            MSECS(timerAction->mPeriod.duration)));

                    ERROR_IF(
                        callPollFdCallbackMethod(timerAction->mAction, &polltm),
                        {
                            warn(errno,
                                 "Error dispatching timer %s",
                                 self->mTimerActions.mNames[ix]);
                        });
                }
            }
        }
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
