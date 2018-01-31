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

#include "ert/process.h"

#include "gtest/gtest.h"

TEST(PidSignatureTest, CreateSignature)
{
    struct PidSignature *pidSignature = 0;

    EXPECT_TRUE(pidSignature = createPidSignature(Ert_Pid(-1), 0));
    EXPECT_EQ(-1, pidSignature->mPid.mPid);
    pidSignature = destroyPidSignature(pidSignature);

    EXPECT_TRUE((pidSignature = createPidSignature(Ert_Pid(0), 0)));
    EXPECT_EQ(0, pidSignature->mPid.mPid);
    pidSignature = destroyPidSignature(pidSignature);

    EXPECT_TRUE((pidSignature = createPidSignature(Ert_Pid(-1), "")));
    pidSignature = destroyPidSignature(pidSignature);

    EXPECT_TRUE((pidSignature = createPidSignature(ert_ownProcessId(), 0)));

    EXPECT_TRUE(pidSignature->mSignature);
    EXPECT_TRUE(strlen(pidSignature->mSignature));

    {
        struct PidSignature *altSignature = 0;

        EXPECT_TRUE((altSignature = createPidSignature(ert_ownProcessId(), 0)));

        EXPECT_EQ(0, strcmp(pidSignature->mSignature,
                            altSignature->mSignature));

        altSignature = destroyPidSignature(altSignature);
    }

    sigset_t sigMask;
    EXPECT_EQ(0, pthread_sigmask(SIG_BLOCK, 0, &sigMask));

    struct Ert_Pid firstChild = ert_forkProcessChild(
        Ert_ForkProcessInheritProcessGroup,
        Ert_Pgid(0),
        Ert_PreForkProcessMethodNil(),
        Ert_PostForkChildProcessMethodNil(),
        Ert_PostForkParentProcessMethodNil(),
        Ert_ForkProcessMethod(
            &sigMask,
            ERT_LAMBDA(
                int, (sigset_t *aSigMask),
                {
                    int rc = 0;

                    do
                    {
                        sigset_t childSigMask;
                        if (pthread_sigmask(SIG_BLOCK, 0, &childSigMask))
                        {
                            rc = 1;
                            break;
                        }

                        if (sigismember(aSigMask, SIGSEGV) !=
                            sigismember(&childSigMask, SIGSEGV))
                        {
                            rc = 2;
                            break;
                        }

                        if (ert_ownProcessAppLockCount())
                        {
                            rc = 3;
                            break;
                        }

                    } while (0);

                    return rc;
                })));

    EXPECT_NE(-1, firstChild.mPid);

    EXPECT_EQ(0, ert_acquireProcessAppLock());
    EXPECT_EQ(1u, ert_ownProcessAppLockCount());

    struct Ert_Pid secondChild = ert_forkProcessChild(
        Ert_ForkProcessInheritProcessGroup,
        Ert_Pgid(0),
        Ert_PreForkProcessMethodNil(),
        Ert_PostForkChildProcessMethodNil(),
        Ert_PostForkParentProcessMethodNil(),
        Ert_ForkProcessMethod(
            (char *) "",
            ERT_LAMBDA(
                int, (char *self),
                {
                    return
                        1 == ert_ownProcessAppLockCount()
                        ? EXIT_SUCCESS
                        : EXIT_FAILURE;
                })));

    EXPECT_NE(-1, secondChild.mPid);

    EXPECT_EQ(0, ert_releaseProcessAppLock());
    EXPECT_EQ(0u, ert_ownProcessAppLockCount());

    struct PidSignature *firstChildSignature = 0;

    EXPECT_TRUE((firstChildSignature = createPidSignature(firstChild, 0)));

    struct PidSignature *secondChildSignature = 0;

    EXPECT_TRUE((secondChildSignature = createPidSignature(secondChild, 0)));

    EXPECT_NE(std::string(firstChildSignature->mSignature),
              std::string(secondChildSignature->mSignature));

    struct Ert_ChildProcessState childState;

    childState = ert_waitProcessChild(firstChild);
    EXPECT_EQ(Ert_ChildProcessState::Ert_ChildProcessStateExited,
              childState.mChildState);
    EXPECT_EQ(0, childState.mChildStatus);

    childState = ert_waitProcessChild(secondChild);
    EXPECT_EQ(Ert_ChildProcessState::Ert_ChildProcessStateExited,
              childState.mChildState);
    EXPECT_EQ(0, childState.mChildStatus);

    int status;
    EXPECT_EQ(0, ert_reapProcessChild(firstChild, &status));
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(0, WEXITSTATUS(status));

    EXPECT_EQ(0, ert_reapProcessChild(secondChild, &status));
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(0, WEXITSTATUS(status));

    secondChildSignature = destroyPidSignature(secondChildSignature);
    firstChildSignature  = destroyPidSignature(firstChildSignature);
    pidSignature         = destroyPidSignature(pidSignature);
}

#include "../googletest/src/gtest_main.cc"
