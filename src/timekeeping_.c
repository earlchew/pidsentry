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

#include "timekeeping_.h"
#include "fd_.h"
#include "error_.h"
#include "macros_.h"

#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#ifdef __linux__
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7 /* Since 2.6.39 */
#endif
#endif

/* -------------------------------------------------------------------------- */
static unsigned             sInit;
static struct MonotonicTime sEventClockTimeBase;

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
#if __linux__
int
procUptime(struct Duration *aUptime, const char *aFileName)
{
    int   rc  = -1;
    char *buf = 0;
    int   fd  = -1;

    fd = open(aFileName, O_RDONLY);
    if (-1 == fd)
        goto Finally;

    ssize_t buflen = readFdFully(fd, &buf, 64);

    if (-1 == buflen)
        goto Finally;
    if ( ! buflen)
    {
        errno = ERANGE;
        goto Finally;
    }

    char *end = strchr(buf, ' ');
    if ( ! end)
    {
        errno = ERANGE;
        goto Finally;
    }

    uint64_t uptime_ns  = 0;
    unsigned fracdigits = 0;

    for (char *ptr = buf; ptr != end; ++ptr)
    {
        unsigned digit;

        switch (*ptr)
        {
        default:
            errno = ERANGE;
            goto Finally;

        case '.':
            if (fracdigits)
            {
                errno = ERANGE;
                goto Finally;
            }
            fracdigits = 1;
            continue;

        case '0': digit = 0; break;
        case '1': digit = 1; break;
        case '2': digit = 2; break;
        case '3': digit = 3; break;
        case '4': digit = 4; break;
        case '5': digit = 5; break;
        case '6': digit = 6; break;
        case '7': digit = 7; break;
        case '8': digit = 8; break;
        case '9': digit = 9; break;
        }

        uint64_t value = uptime_ns * 10;
        if (value / 10 != uptime_ns || value + digit < uptime_ns)
        {
            errno = ERANGE;
            goto Finally;
        }

        uptime_ns = value + digit;

        if (fracdigits)
            fracdigits += 2;
    }

    switch (fracdigits / 2)
    {
    default:
        errno = ERANGE;
        goto Finally;

    case 0: uptime_ns *= 1000000000; break;
    case 1: uptime_ns *=  100000000; break;
    case 2: uptime_ns *=   10000000; break;
    case 3: uptime_ns *=    1000000; break;
    case 4: uptime_ns *=     100000; break;
    case 5: uptime_ns *=      10000; break;
    case 6: uptime_ns *=       1000; break;
    case 7: uptime_ns *=        100; break;
    case 8: uptime_ns *=         10; break;
    case 9: uptime_ns *=          1; break;
    }

    *aUptime = Duration(NanoSeconds(uptime_ns));

    rc = 0;

Finally:

    FINALLY
    ({
        if (-1 != fd)
            close(fd);

        free(buf);
    });

    return rc;
}
#endif

/* -------------------------------------------------------------------------- */
struct BootClockTime
bootclockTime(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_BOOTTIME, &ts))
    {
        do
        {
#ifdef __linux__
            if (EINVAL == errno)
            {
                static const char procUptimeFileName[] = "/proc/uptime";

                struct Duration uptime;

                if (procUptime(&uptime, procUptimeFileName))
                    terminate(
                        errno,
                        "Unable to read %s", procUptimeFileName);

                ts.tv_nsec = uptime.duration.ns % TimeScale_ns;
                ts.tv_sec  = uptime.duration.ns / TimeScale_ns;
                break;
            }
#endif

            terminate(
                errno,
                "Unable to fetch boot time");

        } while (0);
    }

    return (struct BootClockTime) { .bootclock = timeSpecToNanoSeconds(&ts) };
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
void
lapTimeSkip(struct EventClockTime       *self,
            struct Duration              aPeriod,
            const struct EventClockTime *aTime)
{
    self->eventclock = NanoSeconds(
        (aTime ? *aTime
               : eventclockTime()).eventclock.ns - aPeriod.duration.ns);
}

/* -------------------------------------------------------------------------- */
void
lapTimeRestart(struct EventClockTime       *self,
               const struct EventClockTime *aTime)
{
    if (self->eventclock.ns)
        *self = aTime ? *aTime : eventclockTime();
}

/* -------------------------------------------------------------------------- */
struct Duration
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

    return Duration(NanoSeconds(lapTime_ns));
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

    const int sigList[] = { SIGALRM, 0 };

    if (pushProcessSigMask(&aPushedTimer->mSigMask,
                           ProcessSigMaskUnblock,
                           sigList))
        goto Finally;

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

    if (popProcessSigMask(&aPushedTimer->mSigMask))
        goto Finally;

    struct itimerval shortenedInterval =
        shortenIntervalTime(
            &aPushedTimer->mTimer,
            Duration(
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
