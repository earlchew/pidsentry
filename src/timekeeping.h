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

struct PushedIntervalTimer
{
    int              mType;
    int              mSignal;
    uint64_t         mMark;
    struct sigaction mAction;
    struct itimerval mTimer;
};

/* -------------------------------------------------------------------------- */
static inline uint64_t
milliSeconds(uint64_t aMilliSeconds)
{
    return aMilliSeconds * 1000 * 1000;
}

/* -------------------------------------------------------------------------- */
uint64_t
timeValToTime(const struct timeval *aTimeVal);

struct timeval
timeValFromTime(uint64_t aNanoSeconds);

/* -------------------------------------------------------------------------- */
void
monotonicSleep(uint64_t aDuration);

/* -------------------------------------------------------------------------- */
uint64_t
monotonicTime(void);

/* -------------------------------------------------------------------------- */
bool
deadlineTimeExpired(uint64_t *aSince, uint64_t aDuration);

/* -------------------------------------------------------------------------- */
struct timespec
earliestTime(const struct timespec *aLhs, const struct timespec *aRhs);

/* -------------------------------------------------------------------------- */
int
pushIntervalTimer(struct PushedIntervalTimer *aPause,
                   int                         aType,
                   const struct itimerval     *aTimer);

int
popIntervalTimer(struct PushedIntervalTimer *aPause);

/* -------------------------------------------------------------------------- */
struct itimerval
shortenIntervalTime(const struct itimerval *aTimer, uint64_t aElapsed);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* TIMEKEEPING_H */
