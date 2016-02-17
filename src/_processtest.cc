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

#include "process_.h"
#include "timekeeping_.h"

#include <unistd.h>

#include "gtest/gtest.h"

TEST(ProcessTest, ProcessState)
{
    EXPECT_EQ(ProcessStateError, fetchProcessState(-1));

    EXPECT_EQ(ProcessStateRunning, fetchProcessState(getpid()));
}

TEST(ProcessTest, ProcessStatus)
{
    EXPECT_EQ(ProcessStatusError, monitorProcess(getpid()));

    pid_t childpid = fork();

    EXPECT_NE(-1, childpid);

    if ( ! childpid)
        _exit(EXIT_SUCCESS);

    while (ProcessStatusRunning == monitorProcess(childpid))
        continue;

    EXPECT_EQ(ProcessStatusExited, monitorProcess(childpid));
}

TEST(ProcessTest, ProcessSignature)
{
    char *parentSignature = 0;

    EXPECT_EQ(-1, fetchProcessSignature(0, &parentSignature));
    EXPECT_EQ(ENOENT, errno);

    EXPECT_EQ(0, fetchProcessSignature(getpid(), &parentSignature));
    EXPECT_TRUE(parentSignature);
    EXPECT_TRUE(strlen(parentSignature));

    {
        char *altSignature = 0;
        EXPECT_EQ(0, fetchProcessSignature(getpid(), &altSignature));
        EXPECT_EQ(0, strcmp(parentSignature, altSignature));

        free(altSignature);
    }

    pid_t firstChild = forkProcess(ForkProcessShareProcessGroup, 0);
    EXPECT_NE(-1, firstChild);

    if ( ! firstChild)
        _exit(0);

    pid_t secondChild = forkProcess(ForkProcessShareProcessGroup, 0);
    EXPECT_NE(-1, secondChild);

    if ( ! secondChild)
        _exit(0);

    char *firstChildSignature = 0;
    EXPECT_EQ(0, fetchProcessSignature(firstChild, &firstChildSignature));

    char *secondChildSignature = 0;
    EXPECT_EQ(0, fetchProcessSignature(secondChild, &secondChildSignature));

    EXPECT_NE(std::string(firstChildSignature),
              std::string(secondChildSignature));

    int status;
    EXPECT_EQ(0, reapProcess(firstChild, &status));
    EXPECT_EQ(0, reapProcess(secondChild, &status));

    free(parentSignature);
    free(firstChildSignature);
    free(secondChildSignature);
}

static int sigFpeCount_;

static void
sigFpeAction_(int)
{
    ++sigFpeCount_;
}

TEST(ProcessTest, ProcessAppLock)
{
    struct sigaction nextAction;
    struct sigaction prevAction;

    nextAction.sa_handler = sigFpeAction_;
    EXPECT_FALSE(sigfillset(&nextAction.sa_mask));

    EXPECT_FALSE(sigaction(SIGFPE, &nextAction, &prevAction));

    EXPECT_FALSE(raise(SIGFPE));
    EXPECT_EQ(1, sigFpeCount_);

    EXPECT_FALSE(raise(SIGFPE));
    EXPECT_EQ(2, sigFpeCount_);

    struct ProcessAppLock *lock = createProcessAppLock();
    {
        // Verify that the application lock also excludes the delivery
        // of signals while the lock is taken.

        EXPECT_FALSE(raise(SIGFPE));
        EXPECT_EQ(2, sigFpeCount_);

        EXPECT_FALSE(raise(SIGFPE));
        EXPECT_EQ(2, sigFpeCount_);
    }
    destroyProcessAppLock(lock);

    EXPECT_EQ(3, sigFpeCount_);

    EXPECT_FALSE(raise(SIGFPE));
    EXPECT_EQ(4, sigFpeCount_);

    EXPECT_FALSE(raise(SIGFPE));
    EXPECT_EQ(5, sigFpeCount_);

    EXPECT_FALSE(sigaction(SIGFPE, &prevAction, 0));
}

#include "../googletest/src/gtest_main.cc"
