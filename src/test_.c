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

#include "test_.h"
#include "options_.h"
#include "error_.h"
#include "macros_.h"
#include "timekeeping_.h"
#include "env_.h"

#include <stdlib.h>
#include <unistd.h>

#include <sys/mman.h>

#include <valgrind/valgrind.h>

/* -------------------------------------------------------------------------- */
struct TestState
{
    uint64_t mError;
    uint64_t mTrigger;
};

static struct TestState *testState_;
static unsigned          moduleInit_;

/* -------------------------------------------------------------------------- */
bool
testMode(enum TestLevel aLevel)
{
    return aLevel <= gOptions.mTest;
}

/* -------------------------------------------------------------------------- */
bool
testAction(enum TestLevel aLevel)
{
    /* If test mode has been enabled, choose to activate a test action
     * a small percentage of the time. */

    return aLevel <= gOptions.mTest && 3 > random() % 10;
}

/* -------------------------------------------------------------------------- */
bool
testSleep(enum TestLevel aLevel)
{
    bool slept = false;

    /* Unless running valgrind, if test mode has been enabled, choose to
     * sleep a short time a small percentage of the time. Runs under
     * valgrind are already slow enough to provide opportunities to
     * exploit fault timing windows. */

    if ( ! RUNNING_ON_VALGRIND)
    {
        if (testAction(aLevel))
        {
            slept = true;
            monotonicSleep(
                Duration(
                    NSECS(MicroSeconds(random() % (500 * 1000)))));
        }
    }

    return slept;
}

/* -------------------------------------------------------------------------- */
uint64_t
testErrorLevel(void)
{
    return testState_ ? testState_->mError : 0;
}

/* -------------------------------------------------------------------------- */
bool
testFinally(const struct ErrorFrame *aFrame)
{
    bool inject = false;

    if (testState_)
    {
        uint64_t errorLevel = __sync_add_and_fetch(&testState_->mError, 1);

        if (testState_->mTrigger && errorLevel == testState_->mTrigger)
        {
            static const struct
            {
                int         mCode;
                const char *mText;
            }  errTable[] =
                   {
                       { EINTR, "EINTR" },
                       { EIO,   "EIO" },
                   };

            unsigned choice = random() % NUMBEROF(errTable);

            debug(0,
                  "inject %s into %s %s %u",
                  errTable[choice].mText,
                  aFrame->mName,
                  aFrame->mFile,
                  aFrame->mLine);

            errno  = errTable[choice].mCode;
            inject = true;
        }
    }

    return inject;
}

/* -------------------------------------------------------------------------- */
int
Test_init(struct TestModule *self, const char *aErrorEnv)
{
    int rc = -1;

    struct TestState *state = MAP_FAILED;

    self->mModule = self;

    if ( ! moduleInit_)
    {
        uint64_t errorTrigger = 0;

        if (aErrorEnv)
        {
            ERROR_IF(
                getEnvUInt64(aErrorEnv, &errorTrigger) && ENOENT != errno);
        }

        ERROR_IF(
            (state = mmap(0,
                          sizeof(*testState_),
                          PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_SHARED, -1, 0),
             MAP_FAILED == state));

        testState_ = state;

        testState_->mError   = 1;
        testState_->mTrigger = errorTrigger;
    }

    ++moduleInit_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            if (MAP_FAILED != state)
                ABORT_IF(
                    munmap(state, sizeof(*state)));
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct TestModule *
Test_exit(struct TestModule *self)
{
    if (self)
    {
        struct TestState *state = testState_;

        if (state)
        {
            testState_ = 0;
            ABORT_IF(
                munmap(state, sizeof(*state)));
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
