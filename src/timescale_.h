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
#ifndef TIMESCALE_H
#define TIMESCALE_H

#include <inttypes.h>
#include <stddef.h>

struct timespec;
struct timeval;
struct itimerval;

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

#define PRIu_NanoSeconds PRIu64
#define PRIs_NanoSeconds PRIu64 ".%09" PRIu64 "s"
#define FMTs_NanoSeconds(NanoSeconds) \
    ((NanoSeconds).ns / TimeScale_ns), ((NanoSeconds).ns % TimeScale_ns)
struct NanoSeconds
{
    union
    {
        uint64_t ns;
        uint64_t Value_;
        char   (*Scale_)[TimeScale_ns];
    };
};

#define PRIs_MilliSeconds PRIu64 ".%03" PRIu64 "s"
#define FMTs_MilliSeconds(MilliSeconds) \
    ((MilliSeconds).ms / 1000), ((MilliSeconds).ms % 1000)
struct MilliSeconds
{
    union
    {
        uint64_t ms;
        uint64_t Value_;
        char   (*Scale_)[TimeScale_ms];
    };
};

#define PRIs_Seconds PRIu64 "s"
#define FMTs_Seconds(Seconds) ((Seconds).s)
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

/* -------------------------------------------------------------------------- */
#define EVENTCLOCKTIME_INIT ((struct EventClockTime) { { { ns : 0 } } })

static inline struct Duration
Duration(struct NanoSeconds aDuration)
{
    return (struct Duration) { duration : aDuration };
}

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
struct itimerval
shortenIntervalTime(const struct itimerval *aTimer,
                    struct Duration         aElapsed);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* TIMESCALE_H */