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
#ifndef TIMESCALE_H
#define TIMESCALE_H

#include "compiler_.h"

#include <inttypes.h>
#include <stddef.h>

BEGIN_C_SCOPE;

struct timespec;
struct timeval;
struct itimerval;

#ifndef __cplusplus
#define TIMESCALE_CTOR_(Struct_, Type_, Field_)
#else
#define TIMESCALE_CTOR_(Struct_, Type_, Field_)         \
    Struct_()                                           \
    { *this = Struct_ ## _(static_cast<Type_>(0)); }    \
                                                        \
    Struct_(Type_ Field_ ## _)                          \
    { *this = Struct_ ## _(Field_ ## _); }
#endif

enum TimeScale
{
    TimeScale_ns = 1000 * 1000 * 1000,
    TimeScale_us = 1000 * 1000,
    TimeScale_ms = 1000,
    TimeScale_s  = 1,
};

/* -------------------------------------------------------------------------- */
#define PRIu_NanoSeconds PRIu64
#define PRIs_NanoSeconds PRIu64 ".%09" PRIu64 "s"
#define FMTs_NanoSeconds(NanoSeconds) \
    ((NanoSeconds).ns / TimeScale_ns), ((NanoSeconds).ns % TimeScale_ns)

struct NanoSeconds
NanoSeconds_(uint64_t ns);

struct NanoSeconds
{
    TIMESCALE_CTOR_(NanoSeconds, uint64_t, ns)

    union
    {
        uint64_t ns;
        uint64_t Value_;
        char   (*Scale_)[TimeScale_ns];
    };
};

/* -------------------------------------------------------------------------- */
#define PRIu_MicroSeconds PRIu64
#define PRIs_MicroSeconds PRIu64 ".%06" PRIu64 "s"
#define FMTs_MicroSeconds(MicroSeconds) \
    ((MicroSeconds).us / 1000000), ((MicroSeconds).us % 1000000)

struct MicroSeconds
MicroSeconds_(uint64_t us);

struct MicroSeconds
{
    TIMESCALE_CTOR_(MicroSeconds, uint64_t, us)

    union
    {
        uint64_t us;
        uint64_t Value_;
        char   (*Scale_)[TimeScale_us];
    };
};

/* -------------------------------------------------------------------------- */
#define PRIu_MilliSeconds PRIu64
#define PRIs_MilliSeconds PRIu64 ".%03" PRIu64 "s"
#define FMTs_MilliSeconds(MilliSeconds) \
    ((MilliSeconds).ms / 1000), ((MilliSeconds).ms % 1000)

struct MilliSeconds
MilliSeconds_(uint64_t ms);

struct MilliSeconds
{
    TIMESCALE_CTOR_(MilliSeconds, uint64_t, ms)

    union
    {
        uint64_t ms;
        uint64_t Value_;
        char   (*Scale_)[TimeScale_ms];
    };
};

/* -------------------------------------------------------------------------- */
#define PRIu_Seconds PRIu64
#define PRIs_Seconds PRIu64 "s"
#define FMTs_Seconds(Seconds) ((Seconds).s)

struct Seconds
Seconds_(uint64_t s);

struct Seconds
{
    TIMESCALE_CTOR_(Seconds, uint64_t, s)

    union
    {
        uint64_t s;
        uint64_t Value_;
        char   (*Scale_)[TimeScale_s];
    };
};

/* -------------------------------------------------------------------------- */
#define PRIs_Duration PRIs_NanoSeconds
#define FMTs_Duration(Duration) FMTs_NanoSeconds((Duration).duration)

struct Duration
Duration_(struct NanoSeconds duration);

struct Duration
{
#ifdef __cplusplus
    explicit Duration()
    : duration(NanoSeconds(0))
    { }

    explicit Duration(struct NanoSeconds duration_)
    : duration(duration_)
    { }
#endif

    struct NanoSeconds duration;
};

extern const struct Duration ZeroDuration;

/* -------------------------------------------------------------------------- */
#ifndef __cplusplus
static inline struct NanoSeconds
NanoSeconds(uint64_t ns)
{
    return NanoSeconds_(ns);
}

static inline struct MicroSeconds
MicroSeconds(uint64_t ns)
{
    return MicroSeconds_(ns);
}

static inline struct MilliSeconds
MilliSeconds(uint64_t ms)
{
    return MilliSeconds_(ms);
}

static inline struct Seconds
Seconds(uint64_t s)
{
    return Seconds_(s);
}

static inline struct Duration
Duration(struct NanoSeconds aDuration)
{
    return Duration_(aDuration);
}
#endif

/* -------------------------------------------------------------------------- */
#define NSECS(Time)                                             \
    ( (struct NanoSeconds)                                      \
      { {                                                       \
          Value_ : changeTimeScale_((Time).Value_,              \
                                    sizeof(*(Time).Scale_),     \
                                    TimeScale_ns)               \
      } } )

#define USECS(Time)                                             \
    ( (struct MicroSeconds)                                     \
      { {                                                       \
          Value_ : changeTimeScale_((Time).Value_,              \
                                    sizeof(*(Time).Scale_),     \
                                    TimeScale_us)               \
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
struct timespec
earliestTime(const struct timespec *aLhs, const struct timespec *aRhs);

/* -------------------------------------------------------------------------- */

END_C_SCOPE;

#endif /* TIMESCALE_H */
