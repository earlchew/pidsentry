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

#include <valgrind/valgrind.h>

#include "gtest/gtest.h"

TEST(ProcessTest, ProcessSignalName)
{
    struct ProcessSignalName sigName;

    static const char nsigFormat[] = "signal %zu";

    char nsigName[sizeof(nsigFormat) + CHAR_BIT * sizeof(size_t)];
    sprintf(nsigName, nsigFormat, (size_t) NSIG);

    EXPECT_EQ(std::string("SIGHUP"), formatProcessSignalName(&sigName, SIGHUP));
    EXPECT_EQ(std::string("signal 0"), formatProcessSignalName(&sigName, 0));
    EXPECT_EQ(std::string("signal -1"), formatProcessSignalName(&sigName, -1));
    EXPECT_EQ(std::string(nsigName), formatProcessSignalName(&sigName, NSIG));
}

TEST(ProcessTest, ProcessState)
{
    EXPECT_EQ(ProcessState::ProcessStateError,
              fetchProcessState(-1).mState);

    EXPECT_EQ(ProcessState::ProcessStateRunning,
              fetchProcessState(getpid()).mState);
}

TEST(ProcessTest, ProcessStatus)
{
    EXPECT_EQ(ChildProcessState::ChildProcessStateError,
              monitorProcessChild(getpid()).mChildState);

    pid_t childpid = fork();

    EXPECT_NE(-1, childpid);

    if ( ! childpid)
    {
        execlp("sh", "sh", "-c", "exit 0", 0);
        _exit(EXIT_FAILURE);
    }

    while (ChildProcessState::ChildProcessStateRunning ==
           monitorProcessChild(childpid).mChildState)
        continue;

    EXPECT_EQ(ChildProcessState::ChildProcessStateExited,
              monitorProcessChild(childpid).mChildState);
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

    struct Pid firstChild = forkProcess(ForkProcessShareProcessGroup, 0);
    EXPECT_NE(-1, firstChild.mPid);

    if ( ! firstChild.mPid)
    {
        execlp("sh", "sh", "-c", "exit 0", 0);
        _exit(EXIT_SUCCESS);
    }

    struct Pid secondChild = forkProcess(ForkProcessShareProcessGroup, 0);
    EXPECT_NE(-1, secondChild.mPid);

    if ( ! secondChild.mPid)
    {
        execlp("sh", "sh", "-c", "exit 0", 0);
        _exit(EXIT_SUCCESS);
    }

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

static int sigTermCount_;

static void
sigTermAction_(int)
{
    ++sigTermCount_;
}

TEST(ProcessTest, ProcessAppLock)
{
    struct sigaction nextAction;
    struct sigaction prevAction;

    nextAction.sa_handler = sigTermAction_;
    nextAction.sa_flags   = 0;
    EXPECT_FALSE(sigfillset(&nextAction.sa_mask));

    EXPECT_FALSE(sigaction(SIGTERM, &nextAction, &prevAction));

    EXPECT_FALSE(raise(SIGTERM));
    EXPECT_EQ(1, sigTermCount_);

    EXPECT_FALSE(raise(SIGTERM));
    EXPECT_EQ(2, sigTermCount_);

    struct ProcessAppLock *lock = createProcessAppLock();
    {
        // Verify that the application lock also excludes the delivery
        // of signals while the lock is taken.

        EXPECT_FALSE(raise(SIGTERM));
        EXPECT_EQ(2, sigTermCount_);

        EXPECT_FALSE(raise(SIGTERM));
        EXPECT_EQ(2, sigTermCount_);
    }
    destroyProcessAppLock(lock);

    EXPECT_EQ(3, sigTermCount_);

    EXPECT_FALSE(raise(SIGTERM));
    EXPECT_EQ(4, sigTermCount_);

    EXPECT_FALSE(raise(SIGTERM));
    EXPECT_EQ(5, sigTermCount_);

    EXPECT_FALSE(sigaction(SIGTERM, &prevAction, 0));
}

#include "../googletest/src/gtest_main.cc"
