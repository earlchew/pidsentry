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

#include "pidsignature_.h"
#include "process_.h"

#include "gtest/gtest.h"

TEST(PidSignatureTest, CreateSignature)
{
    struct PidSignature *pidSignature = 0;

    EXPECT_FALSE(createPidSignature(Pid(0), 0));
    EXPECT_EQ(ENOENT, errno);

    EXPECT_TRUE((pidSignature = createPidSignature(ownProcessId(), 0)));

    EXPECT_TRUE(pidSignature->mSignature);
    EXPECT_TRUE(strlen(pidSignature->mSignature));

    {
        struct PidSignature *altSignature = 0;

        EXPECT_TRUE((altSignature = createPidSignature(ownProcessId(), 0)));

        EXPECT_EQ(0, strcmp(pidSignature->mSignature,
                            altSignature->mSignature));

        altSignature = destroyPidSignature(altSignature);
    }

    sigset_t sigMask;
    EXPECT_EQ(0, pthread_sigmask(SIG_BLOCK, 0, &sigMask));

    struct Pid firstChild = forkProcessChild(ForkProcessInheritProcessGroup,
                                             Pgid(0),
                                             ForkProcessMethodNil());
    EXPECT_NE(-1, firstChild.mPid);

    if ( ! firstChild.mPid)
    {
        do
        {
            sigset_t childSigMask;
            if (pthread_sigmask(SIG_BLOCK, 0, &childSigMask))
            {
                fprintf(stderr, "%s %u\n", __FILE__, __LINE__);
                break;
            }

            if (sigismember(&sigMask, SIGSEGV) !=
                sigismember(&childSigMask, SIGSEGV))
            {
                fprintf(stderr, "%s %u\n", __FILE__, __LINE__);
                break;
            }

            if (ownProcessAppLockCount())
            {
                fprintf(stderr, "%s %u\n", __FILE__, __LINE__);
                break;
            }

            execl("/bin/true", "true", (char *) 0);

        } while (0);

        execl("/bin/false", "false", (char *) 0);
        _exit(EXIT_FAILURE);
    }

    EXPECT_EQ(0, acquireProcessAppLock());
    EXPECT_EQ(1u, ownProcessAppLockCount());

    struct Pid secondChild = forkProcessChild(ForkProcessInheritProcessGroup,
                                              Pgid(0),
                                              ForkProcessMethodNil());
    EXPECT_NE(-1, secondChild.mPid);

    if ( ! secondChild.mPid)
    {
        if (1 != ownProcessAppLockCount())
            execl("/bin/false", "false", (char *) 0);
        else
            execl("/bin/true", "true", (char *) 0);
        _exit(EXIT_FAILURE);
    }

    EXPECT_EQ(0, releaseProcessAppLock());
    EXPECT_EQ(0u, ownProcessAppLockCount());

    struct PidSignature *firstChildSignature = 0;

    EXPECT_TRUE((firstChildSignature = createPidSignature(firstChild, 0)));

    struct PidSignature *secondChildSignature = 0;

    EXPECT_TRUE((secondChildSignature = createPidSignature(secondChild, 0)));

    EXPECT_NE(std::string(firstChildSignature->mSignature),
              std::string(secondChildSignature->mSignature));

    struct ChildProcessState childState;

    childState = waitProcessChild(firstChild);
    EXPECT_EQ(ChildProcessState::ChildProcessStateExited,
              childState.mChildState);
    EXPECT_EQ(0, childState.mChildStatus);

    childState = waitProcessChild(secondChild);
    EXPECT_EQ(ChildProcessState::ChildProcessStateExited,
              childState.mChildState);
    EXPECT_EQ(0, childState.mChildStatus);

    int status;
    EXPECT_EQ(0, reapProcessChild(firstChild, &status));
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(0, WEXITSTATUS(status));

    EXPECT_EQ(0, reapProcessChild(secondChild, &status));
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(0, WEXITSTATUS(status));

    secondChildSignature = destroyPidSignature(secondChildSignature);
    firstChildSignature  = destroyPidSignature(firstChildSignature);
    pidSignature         = destroyPidSignature(pidSignature);
}

#include "../googletest/src/gtest_main.cc"
