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

#include "timekeeping.h"
#include "error.h"
#include "macros.h"

#include <time.h>
#include <errno.h>

#include <sys/time.h>

/* -------------------------------------------------------------------------- */
uint64_t
monotonicTime(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        terminate(
            errno,
            "Unable to fetch monotonic time");

    uint64_t ns = ts.tv_sec;

    return ns * 1000 * 1000 * 1000 + ts.tv_nsec;
}

/* -------------------------------------------------------------------------- */
bool
deadlineTimeExpired(uint64_t *aSince, uint64_t aDuration)
{
    bool expired;

    if (*aSince)
        expired = monotonicTime() - *aSince >= aDuration;
    else
    {
        /* Initialise the mark time from which the duration will be
         * measured until the deadline, and then ensure that the
         * caller gets to execute at least once before the deadline
         * expires. */

        uint64_t since;

        do
            since = monotonicTime();
        while ( ! since);

        *aSince = since;

        expired = false;
    }

    return expired;
}

/* -------------------------------------------------------------------------- */
void
monotonicSleep(uint64_t aDuration)
{
    uint64_t since = 0;

    while ( ! deadlineTimeExpired(&since, aDuration))
    {
        /* This approach avoids the problem of drifting sleep duration
         * caused by repeated signal delivery by fixing the wake time
         * then re-calibrating the sleep time on each iteration. */

        uint64_t sleepDuration = since + aDuration - monotonicTime();

        if (sleepDuration)
        {
            struct timespec sleepTime =
            {
                .tv_sec  = sleepDuration / (1000 * 1000 * 1000),
                .tv_nsec = sleepDuration % (1000 * 1000 * 1000),
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
uint64_t
timeValToTime(const struct timeval *aTimeVal)
{
    uint64_t ns = aTimeVal->tv_sec;

    return (ns * 1000 * 1000 + aTimeVal->tv_usec) * 1000;
}

/* -------------------------------------------------------------------------- */
struct timeval
timeValFromTime(uint64_t aNanoSeconds)
{
    return (struct timeval) {
        .tv_sec  = aNanoSeconds / (1000 * 1000 * 1000),
        .tv_usec = aNanoSeconds % (1000 * 1000 * 1000) / 1000,
    };
}

/* -------------------------------------------------------------------------- */
struct itimerval
shortenIntervalTime(const struct itimerval *aTimer, uint64_t aElapsedTime)
{
    struct itimerval shortenedTimer = *aTimer;

    uint64_t alarmTime   = timeValToTime(&shortenedTimer.it_value);
    uint64_t alarmPeriod = timeValToTime(&shortenedTimer.it_interval);

    if (alarmTime > aElapsedTime)
    {
        shortenedTimer.it_value = timeValFromTime(alarmTime - aElapsedTime);
    }
    else if (alarmTime)
    {
        if ( ! alarmPeriod)
            shortenedTimer.it_value = shortenedTimer.it_interval;
        else
        {
            shortenedTimer.it_value = timeValFromTime(
                alarmPeriod - (aElapsedTime - alarmTime) % alarmPeriod);
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
        shortenIntervalTime(&aPushedTimer->mTimer,
                            monotonicTime() - aPushedTimer->mMark);

    if (setitimer(aPushedTimer->mType, &shortenedInterval, 0))
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
