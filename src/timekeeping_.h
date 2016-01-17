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
#ifndef TIMEKEEPING_H
#define TIMEKEEPING_H

#include "process_.h"
#include "timescale_.h"

#include <stdbool.h>
#include <signal.h>

#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    int                         mType;
    int                         mSignal;
    struct MonotonicTime        mMark;
    struct PushedProcessSigMask mSigMask;
    struct sigaction            mAction;
    struct itimerval            mTimer;
};

/* -------------------------------------------------------------------------- */
void
monotonicSleep(struct Duration aPeriod);

/* -------------------------------------------------------------------------- */
struct EventClockTime
eventclockTime(void);

struct MonotonicTime
monotonicTime(void);

struct WallClockTime
wallclockTime(void);

/* -------------------------------------------------------------------------- */
struct Duration
lapTimeSince(struct EventClockTime       *self,
             struct Duration              aPeriod,
             const struct EventClockTime *aTime);

void
lapTimeRestart(struct EventClockTime       *self,
               const struct EventClockTime *aTime);

void
lapTimeSkip(struct EventClockTime       *self,
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
int
Timekeeping_init(void);

int
Timekeeping_exit(void);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* TIMEKEEPING_H */
