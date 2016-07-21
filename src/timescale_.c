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

#include "timescale_.h"
#include "error_.h"

#include <sys/time.h>

/* -------------------------------------------------------------------------- */
const struct Duration ZeroDuration =
    { .duration = { { .ns = 0 } } };

/* -------------------------------------------------------------------------- */
struct NanoSeconds
NanoSeconds_(uint64_t ns)
{
    return (struct NanoSeconds) { { .ns = ns } };
}

struct MicroSeconds
MicroSeconds_(uint64_t us)
{
    return (struct MicroSeconds) { { .us = us } };
}

struct MilliSeconds
MilliSeconds_(uint64_t ms)
{
    return (struct MilliSeconds) { { .ms = ms } };
}

struct Seconds
Seconds_(uint64_t s)
{
    return (struct Seconds) { { .s = s } };
}

struct Duration
Duration_(struct NanoSeconds duration)
{
    return (struct Duration) { .duration = duration };
}

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

        ABORT_IF(
            dstTime / scaleUp != aSrcTime,
            {
                terminate(
                    0,
                    "Time scale overflow converting %" PRIu64 " "
                    "from scale %zu to scale %zu",
                    aSrcTime,
                    aSrcScale,
                    aDstScale);
            });

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
