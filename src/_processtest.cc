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
#include "bellsocketpair_.h"
#include "macros_.h"
#include <string>

#include <unistd.h>

#include <sys/mman.h>

#include <valgrind/valgrind.h>

#include "gtest/gtest.h"

class ProcessTest : public ::testing::Test
{
    void SetUp()
    {
        ASSERT_EQ(0, Process_init(&mModule, __FILE__));
    }

    void TearDown()
    {
        Process_exit(&mModule);
    }

private:

    struct ProcessModule mModule;
};

TEST_F(ProcessTest, ProcessSignalName)
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

TEST_F(ProcessTest, ProcessState)
{
    EXPECT_EQ(ProcessState::ProcessStateError,
              fetchProcessState(Pid(-1)).mState);

    EXPECT_EQ(ProcessState::ProcessStateRunning,
              fetchProcessState(ownProcessId()).mState);
}

TEST_F(ProcessTest, ProcessStatus)
{
    EXPECT_EQ(ChildProcessState::ChildProcessStateError,
              monitorProcessChild(ownProcessId()).mChildState);

    struct Pid childpid = Pid(fork());

    EXPECT_NE(-1, childpid.mPid);

    if ( ! childpid.mPid)
    {
        execl("/bin/true", "true", (char *) 0);
        _exit(EXIT_FAILURE);
    }

    while (ChildProcessState::ChildProcessStateRunning ==
           monitorProcessChild(childpid).mChildState)
        continue;

    EXPECT_EQ(ChildProcessState::ChildProcessStateExited,
              monitorProcessChild(childpid).mChildState);
}

TEST_F(ProcessTest, ProcessSignature)
{
    char *parentSignature = 0;

    EXPECT_EQ(-1, fetchProcessSignature(Pid(0), &parentSignature));
    EXPECT_EQ(ENOENT, errno);

    EXPECT_EQ(0, fetchProcessSignature(ownProcessId(), &parentSignature));
    EXPECT_TRUE(parentSignature);
    EXPECT_TRUE(strlen(parentSignature));

    {
        char *altSignature = 0;
        EXPECT_EQ(0, fetchProcessSignature(ownProcessId(), &altSignature));
        EXPECT_EQ(0, strcmp(parentSignature, altSignature));

        free(altSignature);
    }

    sigset_t sigMask;
    EXPECT_EQ(0, pthread_sigmask(SIG_BLOCK, 0, &sigMask));

    struct Pid firstChild = forkProcessChild(ForkProcessInheritProcessGroup,
                                             Pgid(0),
                                             IntMethodNil());
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
                                              IntMethodNil());
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

    char *firstChildSignature = 0;
    EXPECT_EQ(0, fetchProcessSignature(firstChild, &firstChildSignature));

    char *secondChildSignature = 0;
    EXPECT_EQ(0, fetchProcessSignature(secondChild, &secondChildSignature));

    EXPECT_NE(std::string(firstChildSignature),
              std::string(secondChildSignature));

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

TEST_F(ProcessTest, ProcessAppLock)
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

    struct ProcessAppLock *appLock = createProcessAppLock();
    {
        // Verify that the application lock also excludes the delivery
        // of signals while the lock is taken.

        EXPECT_FALSE(raise(SIGTERM));
        EXPECT_EQ(2, sigTermCount_);

        EXPECT_FALSE(raise(SIGTERM));
        EXPECT_EQ(2, sigTermCount_);
    }
    destroyProcessAppLock(appLock);

    EXPECT_EQ(3, sigTermCount_);

    EXPECT_FALSE(raise(SIGTERM));
    EXPECT_EQ(4, sigTermCount_);

    EXPECT_FALSE(raise(SIGTERM));
    EXPECT_EQ(5, sigTermCount_);

    EXPECT_FALSE(sigaction(SIGTERM, &prevAction, 0));
}

TEST_F(ProcessTest, ProcessDaemon)
{
    struct BellSocketPair bellSocket;

    EXPECT_EQ(0, createBellSocketPair(&bellSocket, 0));

    struct DaemonState
    {
        int mErrno;
        int mSigMask[NSIG];
    };

    struct DaemonState *daemonState =
        reinterpret_cast<struct DaemonState *>(
            mmap(0,
                 sizeof(*daemonState),
                 PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_SHARED, -1, 0));

    EXPECT_NE(MAP_FAILED, (void *) daemonState);

    daemonState->mErrno = ENOSYS;

    struct Pid daemonPid = forkProcessDaemon(IntMethodNil());

    if ( ! daemonPid.mPid)
    {
        sigset_t sigMask;

        daemonState->mErrno = 0;

        if (pthread_sigmask(SIG_BLOCK, 0, &sigMask))
            daemonState->mErrno = errno;
        else
        {
            for (unsigned sx = 0; NUMBEROF(daemonState->mSigMask) > sx; ++sx)
                daemonState->mSigMask[sx] = sigismember(&sigMask, sx);
        }

        closeBellSocketPairParent(&bellSocket);
        if (ringBellSocketPairChild(&bellSocket) ||
            waitBellSocketPairChild(&bellSocket, 0))
        {
            execl("/bin/false", "false", (char *) 0);
        }
        else
        {
            execl("/bin/true", "true", (char *) 0);
        }

        _exit(EXIT_FAILURE);
    }

    closeBellSocketPairChild(&bellSocket);

    EXPECT_NE(-1, daemonPid.mPid);
    EXPECT_EQ(daemonPid.mPid, getpgid(daemonPid.mPid));
    EXPECT_EQ(getsid(0), getsid(daemonPid.mPid));

    EXPECT_EQ(1, waitBellSocketPairParent(&bellSocket, 0));

    EXPECT_EQ(0, daemonState->mErrno);

    {
        sigset_t sigMask;

        EXPECT_EQ(0, pthread_sigmask(SIG_BLOCK, 0, &sigMask));

        for (unsigned sx = 0; NUMBEROF(daemonState->mSigMask) > sx; ++sx)
            EXPECT_EQ(
                daemonState->mSigMask[sx], sigismember(&sigMask, sx))
                << " failed at index " << sx;
    }

    EXPECT_EQ(0, munmap(daemonState, sizeof(*daemonState)));

    closeBellSocketPair(&bellSocket);
}

#include "../googletest/src/gtest_main.cc"
