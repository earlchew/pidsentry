/* -*- c-basic-offset:4; indent-tabs-mode:nil -*- vi: set sw=4 et: */
/*
// Copyright (c) 2015, Earl Chew
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

#include "timekeeping_.h"
#include "error_.h"
#include "macros_.h"

#include <time.h>
#include <errno.h>

#include <sys/time.h>

/* -------------------------------------------------------------------------- */
static unsigned             sInit;
static struct MonotonicTime sEventClockTimeBase;

/* -------------------------------------------------------------------------- */
uint64_t
changeTimeScale_(uint64_t aSrcTime, size_t aSrcScale, size_t aDstScale)
{
    if (aSrcScale < aDstScale)
    {
        /* When changing to a timescale with more resolution, take
         * care to check for overflow of the representation. This is
         * not likely to occur since the width of the representation
         * allows the timescale to range far into the future, and if
         * it does occur if probably indicative of a programming error. */

        size_t scaleUp = aDstScale / aSrcScale;

        uint64_t dstTime = aSrcTime * scaleUp;

        if (dstTime / scaleUp != aSrcTime)
            terminate(
                0,
                "Time scale overflow converting %" PRIu64
                " from scale %zu to scale %zu",
                aSrcTime,
                aSrcScale,
                aDstScale);

        return dstTime;
    }

    if (aSrcScale > aDstScale)
    {
        /* The most common usage for timekeeping is to manage timeouts,
         * so when changing to a timescale with less resolution, rounding
         * up results in less surprising outcomes because a non-zero
         * timeout rounds to a non-zero result. */

        size_t scaleDown = aSrcScale / aDstScale;

        return aSrcTime / scaleDown + !! (aSrcTime % scaleDown);
    }

    return aSrcTime;
}

/* -------------------------------------------------------------------------- */
struct Duration
duration(struct NanoSeconds aDuration)
{
    return (struct Duration) { .duration = aDuration };
}

/* -------------------------------------------------------------------------- */
struct MonotonicTime
monotonicTime(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        terminate(
            errno,
            "Unable to fetch monotonic time");

    return (struct MonotonicTime) { .monotonic = timeSpecToNanoSeconds(&ts) };
}

/* -------------------------------------------------------------------------- */
static void
eventclockTime_init_(void)
{
    /* Initialise the time base for the event clock, and ensure that
     * the event clock will subsequently always return a non-zero result. */

    sEventClockTimeBase = (struct MonotonicTime) {
        .monotonic = NanoSeconds(monotonicTime().monotonic.ns - 1) };
}

struct EventClockTime
eventclockTime(void)
{
    struct EventClockTime tm = {
        .eventclock = NanoSeconds(
            monotonicTime().monotonic.ns - sEventClockTimeBase.monotonic.ns) };

    ensure(tm.eventclock.ns);

    return tm;
}

/* -------------------------------------------------------------------------- */
struct WallClockTime
wallclockTime(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts))
        terminate(
            errno,
            "Unable to fetch monotonic time");

    return (struct WallClockTime) { .wallclock = timeSpecToNanoSeconds(&ts) };
}

/* -------------------------------------------------------------------------- */
bool
deadlineTimeExpired(
    struct EventClockTime       *self,
    struct Duration              aPeriod,
    struct Duration             *aRemaining,
    const struct EventClockTime *aTime)
{
    bool                  expired;
    uint64_t              remaining_ns;
    struct EventClockTime tm;

    if ( ! aTime)
    {
        tm   = eventclockTime();
        aTime = &tm;
    }

    if (self->eventclock.ns)
    {
        uint64_t elapsed_ns = aTime->eventclock.ns - self->eventclock.ns;

        if (elapsed_ns >= aPeriod.duration.ns)
        {
            remaining_ns = 0;
            expired      = true;
        }
        else
        {
            remaining_ns = aPeriod.duration.ns - elapsed_ns;
            expired      = false;
        }
    }
    else
    {
        /* Initialise the mark time from which the duration will be
         * measured until the deadline, and then ensure that the
         * caller gets to execute at least once before the deadline
         * expires. */

        *self = *aTime;

        ensure(self->eventclock.ns);

        remaining_ns = aPeriod.duration.ns;
        expired      = false;
    }

    if (aRemaining)
        aRemaining->duration.ns = remaining_ns;

    return expired;
}

/* -------------------------------------------------------------------------- */
struct NanoSeconds
lapTimeSince(struct EventClockTime       *self,
             struct Duration              aPeriod,
             const struct EventClockTime *aTime)
{
    struct EventClockTime tm;

    if ( ! aTime)
    {
        tm    = eventclockTime();
        aTime = &tm;
    }

    uint64_t lapTime_ns;

    if (self->eventclock.ns)
    {
        lapTime_ns = aTime->eventclock.ns - self->eventclock.ns;

        if (aPeriod.duration.ns && lapTime_ns >= aPeriod.duration.ns)
            self->eventclock.ns =
                aTime->eventclock.ns - lapTime_ns % aPeriod.duration.ns;
    }
    else
    {
        lapTime_ns = 0;

        *self = *aTime;

        ensure(self->eventclock.ns);
    }

    return NanoSeconds(lapTime_ns);
}

/* -------------------------------------------------------------------------- */
void
monotonicSleep(struct Duration aPeriod)
{
    struct EventClockTime since = EVENTCLOCKTIME_INIT;
    struct Duration       remaining;

    while ( ! deadlineTimeExpired(&since, aPeriod, &remaining, 0))
    {
        /* This approach avoids the problem of drifting sleep duration
         * caused by repeated signal delivery by fixing the wake time
         * then re-calibrating the sleep time on each iteration. */

        if (remaining.duration.ns)
        {
            struct timespec sleepTime =
            {
                .tv_sec  = remaining.duration.ns / (1000 * 1000 * 1000),
                .tv_nsec = remaining.duration.ns % (1000 * 1000 * 1000),
            };

            nanosleep(&sleepTime, 0);
        }
    }
}

/* -------------------------------------------------------------------------- */
struct timespec
earliestTime(const struct timespec *aLhs, const struct timespec *aRhs)
{
    if (aLhs->tv_sec < aRhs->tv_sec)
        return *aLhs;

    if (aLhs->tv_sec  == aRhs->tv_sec &&
        aLhs->tv_nsec <  aRhs->tv_nsec)
    {
        return *aLhs;
    }

    return *aRhs;
}

/* -------------------------------------------------------------------------- */
struct NanoSeconds
timeValToNanoSeconds(const struct timeval *aTimeVal)
{
    uint64_t ns = aTimeVal->tv_sec;

    return NanoSeconds((ns * 1000 * 1000 + aTimeVal->tv_usec) * 1000);
}

/* -------------------------------------------------------------------------- */
struct timeval
timeValFromNanoSeconds(struct NanoSeconds aNanoSeconds)
{
    return (struct timeval) {
        .tv_sec  = aNanoSeconds.ns / (1000 * 1000 * 1000),
        .tv_usec = aNanoSeconds.ns % (1000 * 1000 * 1000) / 1000,
    };
}

/* -------------------------------------------------------------------------- */
struct NanoSeconds
timeSpecToNanoSeconds(const struct timespec *aTimeSpec)
{
    uint64_t ns = aTimeSpec->tv_sec;

    return NanoSeconds((ns * 1000 * 1000 * 1000) + aTimeSpec->tv_nsec);
}

/* -------------------------------------------------------------------------- */
struct timespec
timeSpecFromNanoSeconds(struct NanoSeconds aNanoSeconds)
{
    return (struct timespec) {
        .tv_sec  = aNanoSeconds.ns / (1000 * 1000 * 1000),
        .tv_nsec = aNanoSeconds.ns % (1000 * 1000 * 1000),
    };
}

/* -------------------------------------------------------------------------- */
struct itimerval
shortenIntervalTime(const struct itimerval *aTimer,
                    struct Duration         aElapsed)
{
    struct itimerval shortenedTimer = *aTimer;

    struct NanoSeconds alarmTime =
        timeValToNanoSeconds(&shortenedTimer.it_value);

    struct NanoSeconds alarmPeriod =
        timeValToNanoSeconds(&shortenedTimer.it_interval);

    if (alarmTime.ns > aElapsed.duration.ns)
    {
        shortenedTimer.it_value = timeValFromNanoSeconds(
            NanoSeconds(alarmTime.ns - aElapsed.duration.ns));
    }
    else if (alarmTime.ns)
    {
        if ( ! alarmPeriod.ns)
            shortenedTimer.it_value = shortenedTimer.it_interval;
        else
        {
            shortenedTimer.it_value = timeValFromNanoSeconds(
                NanoSeconds(
                    alarmPeriod.ns - (
                        aElapsed.duration.ns - alarmTime.ns) % alarmPeriod.ns));
        }
    }

    return shortenedTimer;
}

/* -------------------------------------------------------------------------- */
static void
pushIntervalTimer_(int aSigNum)
{ }

static const struct itimerval sPauseDisableTimer;

int
pushIntervalTimer(struct PushedIntervalTimer *aPushedTimer,
                  int                         aType,
                  const struct itimerval     *aTimer)
{
    int rc = -1;

    switch (aType)
    {
    default:
        errno = EINVAL;
        goto Finally;

    case ITIMER_REAL:    aPushedTimer->mSignal = SIGALRM;   break;
    case ITIMER_VIRTUAL: aPushedTimer->mSignal = SIGVTALRM; break;
    case ITIMER_PROF:    aPushedTimer->mSignal = SIGPROF;   break;
    }

    aPushedTimer->mType = aType;
    aPushedTimer->mMark = monotonicTime();

    /* Disable the timer and signal action so that a new
     * timer and action can be installed.
     *
     * Take care to disable the timer, before resetting the
     * signal handler, then re-configuring the timer. */

    if (setitimer(aType, &sPauseDisableTimer, &aPushedTimer->mTimer))
        goto Finally;

    struct sigaction timerAction =
    {
        .sa_handler = pushIntervalTimer_,
    };

    if (sigaction(aPushedTimer->mSignal, &timerAction, &aPushedTimer->mAction))
        goto Finally;

    if (aTimer && (aTimer->it_value.tv_sec || aTimer->it_value.tv_usec))
    {
        if (setitimer(aType, aTimer, 0))
            goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
popIntervalTimer(struct PushedIntervalTimer *aPushedTimer)
{
    int rc = -1;

    /* Restore the previous setting of the timer and signal handler.
     * Take care to disable the timer, before restoring the
     * signal handler, then restoring the setting of the timer. */

    if (setitimer(aPushedTimer->mType, &sPauseDisableTimer, 0))
        goto Finally;

    if (sigaction(aPushedTimer->mSignal, &aPushedTimer->mAction, 0))
        goto Finally;

    struct itimerval shortenedInterval =
        shortenIntervalTime(
            &aPushedTimer->mTimer,
            duration(
                NanoSeconds(
                    monotonicTime().monotonic.ns -
                    aPushedTimer->mMark.monotonic.ns)));

    if (setitimer(aPushedTimer->mType, &shortenedInterval, 0))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
Timekeeping_init(void)
{
    if (++sInit == 1)
    {
        eventclockTime_init_();
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
int
Timekeeping_exit(void)
{
    --sInit;

    return 0;
}

/* -------------------------------------------------------------------------- */
