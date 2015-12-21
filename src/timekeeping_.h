/* -*- c-basic-offset:4; indent-tabs-mode:nil -*- vi: set sw=4 et: */
/*
// Copyright (c) 2013, Earl Chew
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
#ifndef TIMEKEEPING_H
#define TIMEKEEPING_H

#include <inttypes.h>
#include <stdbool.h>
#include <signal.h>

#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum TimeScale
{
    TimeScale_ns = 1000 * 1000 * 1000,
    TimeScale_us = 1000 * 1000,
    TimeScale_ms = 1000,
    TimeScale_s  = 1,
};

struct NanoSeconds
{
    union
    {
        uint64_t ns;
        uint64_t Value_;
        char   (*Scale_)[TimeScale_ns];
    };
};

struct MilliSeconds
{
    union
    {
        uint64_t ms;
        uint64_t Value_;
        char   (*Scale_)[TimeScale_ms];
    };
};

struct Seconds
{
    union
    {
        uint64_t s;
        uint64_t Value_;
        char   (*Scale_)[TimeScale_s];
    };
};

struct Duration
{
    struct NanoSeconds duration;
};

struct MonotonicTime
{
    struct NanoSeconds monotonic;
};

struct WallClockTime
{
    struct NanoSeconds wallclock;
};

struct EventClockTime
{
    struct NanoSeconds eventclock;
};

struct PushedIntervalTimer
{
    int                  mType;
    int                  mSignal;
    struct MonotonicTime mMark;
    struct sigaction     mAction;
    struct itimerval     mTimer;
};

/* -------------------------------------------------------------------------- */
#define EVENTCLOCKTIME_INIT ((struct EventClockTime) { { { ns : 0 } } })

static inline struct NanoSeconds
NanoSeconds(uint64_t ns)
{
    return (struct NanoSeconds) { { ns : ns } };
}

static inline struct MilliSeconds
MilliSeconds(uint64_t ms)
{
    return (struct MilliSeconds) { { ms : ms } };
}

static inline struct Seconds
Seconds(uint64_t s)
{
    return (struct Seconds) { { s : s } };
}

#define NSECS(Time)                                             \
    ( (struct NanoSeconds)                                      \
      { {                                                       \
          Value_ : changeTimeScale_((Time).Value_,              \
                                    sizeof(*(Time).Scale_),     \
                                    TimeScale_ns)               \
      } } )

#define MSECS(Time)                                             \
    ( (struct MilliSeconds)                                     \
      { {                                                       \
          Value_ : changeTimeScale_((Time).Value_,              \
                                    sizeof(*(Time).Scale_),     \
                                    TimeScale_ms)               \
      } } )

#define SECS(Time)                                              \
    ( (struct Seconds)                                          \
      { {                                                       \
          Value_ : changeTimeScale_((Time).Value_,              \
                                    sizeof(*(Time).Scale_),     \
                                    TimeScale_s)                \
      } } )

uint64_t
changeTimeScale_(uint64_t aSrcTime, size_t aSrcScale, size_t aDstScale);

/* -------------------------------------------------------------------------- */
struct NanoSeconds
timeValToNanoSeconds(const struct timeval *aTimeVal);

struct NanoSeconds
timeSpecToNanoSeconds(const struct timespec *aTimeSpec);

struct timeval
timeValFromNanoSeconds(struct NanoSeconds aNanoSeconds);

struct timespec
timeSpecFromNanoSeconds(struct NanoSeconds aNanoSeconds);

/* -------------------------------------------------------------------------- */
void
monotonicSleep(struct Duration aPeriod);

/* -------------------------------------------------------------------------- */
struct Duration
duration(struct NanoSeconds aDuration);

struct EventClockTime
eventclockTime(void);

struct MonotonicTime
monotonicTime(void);

struct WallClockTime
wallclockTime(void);

/* -------------------------------------------------------------------------- */
struct NanoSeconds
lapTimeSince(struct EventClockTime       *self,
             struct Duration              aPeriod,
             const struct EventClockTime *aTime);

/* -------------------------------------------------------------------------- */
bool
deadlineTimeExpired(
    struct EventClockTime       *self,
    struct Duration              aPeriod,
    struct Duration             *aRemaining,
    const struct EventClockTime *aTime);

/* -------------------------------------------------------------------------- */
struct timespec
earliestTime(const struct timespec *aLhs, const struct timespec *aRhs);

/* -------------------------------------------------------------------------- */
int
pushIntervalTimer(struct PushedIntervalTimer *aPause,
                   int                        aType,
                   const struct itimerval    *aTimer);

int
popIntervalTimer(struct PushedIntervalTimer *aPause);

/* -------------------------------------------------------------------------- */
struct itimerval
shortenIntervalTime(const struct itimerval *aTimer,
                    struct Duration         aElapsed);

/* -------------------------------------------------------------------------- */
int
Timekeeping_init(void);

int
Timekeeping_exit(void);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* TIMEKEEPING_H */
