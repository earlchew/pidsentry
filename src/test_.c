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

#include "test_.h"
#include "options_.h"

#include <stdlib.h>
#include <unistd.h>

#include <valgrind/valgrind.h>

/* -------------------------------------------------------------------------- */
bool
testMode(void)
{
    return gOptions.mTest;
}

/* -------------------------------------------------------------------------- */
bool
testAction(void)
{
    /* If test mode has been enabled, choose to activate a test action
     * a small percentage of the time. */

    return gOptions.mTest && 3 > random() % 10;
}

/* -------------------------------------------------------------------------- */
bool
testSleep(void)
{
    bool slept = false;

    /* Unless running valgrind, if test mode has been enabled, choose to
     * sleep a short time a small percentage of the time. Runs under
     * valgrind are already slow enough to provide opportunities to
     * exploit fault timing windows. */

    if ( ! RUNNING_ON_VALGRIND)
    {
        if (testAction())
        {
            slept = true;
            usleep(random() % (500 * 1000));
        }
    }

    return slept;
}

/* -------------------------------------------------------------------------- */
