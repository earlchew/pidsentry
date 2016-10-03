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

#include "random_.h"
#include "process_.h"

#include <unistd.h>
#include <pthread.h>

/* -------------------------------------------------------------------------- */
static uint64_t seed_;

static const uint64_t multiplier_ = 6364136223846793005ull;
static const uint64_t increment_  = 1;

static void
srandom_(uint64_t aValue)
{
    uint64_t seed;

    do
    {
        seed  = seed_;
    }
    while ( ! __sync_bool_compare_and_swap(&seed_, seed, aValue));

}

static uint64_t
random_(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;

    if (pthread_once(
            &once,
            LAMBDA(
                void, (void),
                {
                    scrambleRandomSeed(getpid());
                })))
        abortProcess();

    uint64_t seed;
    uint64_t value;

    do
    {
        seed  = seed_;
        value = seed * multiplier_ + increment_;
    }
    while ( ! __sync_bool_compare_and_swap(&seed_, seed, value));

    return value >> 32;
}

EARLY_INITIALISER(
    random_,
    ({
        scrambleRandomSeed(getpid());
    }),
    ({ }));

/* -------------------------------------------------------------------------- */
void
scrambleRandomSeed(unsigned aSeed)
{
    srandom_(aSeed * multiplier_);
}

/* -------------------------------------------------------------------------- */
unsigned
fetchRandomRange(unsigned aRange)
{
    return random_() % aRange;
}

/* -------------------------------------------------------------------------- */
unsigned
fetchRandomUniform(unsigned aLhs, unsigned aRhs)
{
    unsigned range = aRhs - aLhs + 1;

    return (range ? random_() % (aRhs - aLhs + 1) : random_()) + aLhs;
}

/* -------------------------------------------------------------------------- */
