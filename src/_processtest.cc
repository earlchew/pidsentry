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
#include "fdset_.h"
#include "thread_.h"
#include "macros_.h"

#include <string>

#include <unistd.h>

#include <sys/mman.h>
#include <sys/resource.h>

#include <valgrind/valgrind.h>

#include "gtest/gtest.h"

class ProcessTest : public ::testing::Test
{
    void SetUp()
    {
        ASSERT_EQ(0, Process_init(&mModule_, __FILE__));
        mModule= &mModule_;
    }

    void TearDown()
    {
        mModule = Process_exit(mModule);
    }

private:

    struct ProcessModule  mModule_;
    struct ProcessModule *mModule;
};

TEST_F(ProcessTest, ProcessSignalName)
{
    struct ProcessSignalName sigName;

    static const char nsigFormat[] = "signal %zu";

    char nsigName[sizeof(nsigFormat) + CHAR_BIT * sizeof(size_t)];
    EXPECT_FALSE(0 > sprintf(nsigName, nsigFormat, (size_t) NSIG));

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

    int status;
    EXPECT_EQ(0, reapProcessChild(childpid, &status));
    EXPECT_EQ(0, status);
}

#if 0
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
    appLock = destroyProcessAppLock(appLock);

    EXPECT_EQ(3, sigTermCount_);

    EXPECT_FALSE(raise(SIGTERM));
    EXPECT_EQ(4, sigTermCount_);

    EXPECT_FALSE(raise(SIGTERM));
    EXPECT_EQ(5, sigTermCount_);

    EXPECT_FALSE(sigaction(SIGTERM, &prevAction, 0));
}

TEST_F(ProcessTest, ProcessDaemon)
{
    struct BellSocketPair  bellSocket_;
    struct BellSocketPair *bellSocket = 0;

    EXPECT_EQ(0, createBellSocketPair(&bellSocket_, 0));
    bellSocket = &bellSocket_;

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

    struct Pid daemonPid = forkProcessDaemon(ForkProcessMethodNil());

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

        closeBellSocketPairParent(bellSocket);
        if (ringBellSocketPairChild(bellSocket) ||
            waitBellSocketPairChild(bellSocket, 0))
        {
            execl("/bin/false", "false", (char *) 0);
        }
        else
        {
            execl("/bin/true", "true", (char *) 0);
        }

        _exit(EXIT_FAILURE);
    }

    closeBellSocketPairChild(bellSocket);

    EXPECT_NE(-1, daemonPid.mPid);
    EXPECT_EQ(daemonPid.mPid, getpgid(daemonPid.mPid));
    EXPECT_EQ(getsid(0), getsid(daemonPid.mPid));

    EXPECT_EQ(0, waitBellSocketPairParent(bellSocket, 0));

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

    bellSocket = closeBellSocketPair(bellSocket);
}
#endif

struct ProcessForkArg
{
    bool             mStart;
    pthread_cond_t   mCond_;
    pthread_cond_t  *mCond;
    pthread_mutex_t  mMutex_;
    pthread_mutex_t *mMutex;

    unsigned mNumFds;
    unsigned mNumForks;
};

struct ProcessForkTest
{
    int        mPipeFds[2];
    struct Pid mChildPid;
};

static unsigned
countFds()
{
    struct rlimit fdLimit;

    if (getrlimit(RLIMIT_NOFILE, &fdLimit))
        abort();

    unsigned numFds = 0;

    for (unsigned fd = 0; fd < fdLimit.rlim_cur; ++fd)
    {
        if (ownFdValid(fd))
            ++numFds;
    }

    return numFds;
}

static void
processForkTest_Trivial_()
{
    /* Simple with no prefork or postfork methods */

    struct Pid childPid =
        forkProcessChildX(ForkProcessInheritProcessGroup,
                          Pgid(0),
                          PreForkProcessMethodNil(),
                          PostForkChildProcessMethodNil(),
                          PostForkParentProcessMethodNil(),
                          ForkProcessMethodNil());

    EXPECT_NE(-1, childPid.mPid);

    if ( ! childPid.mPid)
    {
        execl("/bin/true", "true", (char *) 0);
        _exit(EXIT_FAILURE);
    }

    int status;
    EXPECT_EQ(0, reapProcessChild(childPid, &status));
    EXPECT_EQ(0, (extractProcessExitStatus(status, childPid).mStatus));
}

static void
processForkTest_Usual_(struct ProcessForkArg *aArg)
{
    /* Standard use case */

    struct ProcessForkTest forkTest;

    struct Pid childPid =
        forkProcessChildX(ForkProcessInheritProcessGroup,
                          Pgid(0),
                          PreForkProcessMethod(
                              LAMBDA(
                                  int, (
                                      struct ProcessForkTest      *self,
                                      const struct PreForkProcess *aFork),
                                  {
                                      /* Provide time for competing threads
                                       * to also run this code. */

                                      sleep((ownThreadId().mTid / 2) % 3);
                                      int err = pipe(self->mPipeFds);

                                      err = err
                                          ? err
                                          : insertFdSetRange(
                                              aFork->mBlacklistFds,
                                              self->mPipeFds[1],
                                              self->mPipeFds[1]);

                                      err = err
                                          ? err
                                          : insertFdSetRange(
                                              aFork->mWhitelistFds,
                                              self->mPipeFds[1],
                                              self->mPipeFds[1]);

                                      return err;
                                  }),
                              &forkTest),
                          PostForkChildProcessMethod(
                              LAMBDA(
                                  int, (struct ProcessForkTest *self),
                                  {
                                      return 0;
                                  }),
                              &forkTest),
                          PostForkParentProcessMethod(
                              LAMBDA(
                                  int, (struct ProcessForkTest *self,
                                        struct Pid              aChildPid),
                                  {
                                      self->mChildPid = aChildPid;

                                      return 0;
                                  }),
                              &forkTest),
                          ForkProcessMethodNil());

    EXPECT_NE(-1, childPid.mPid);

    if ( ! childPid.mPid)
    {
        int rc = -1;

        do
        {
            if ( ! ownFdValid(STDIN_FILENO))
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            if ( ! ownFdValid(STDOUT_FILENO))
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            if ( ! ownFdValid(STDERR_FILENO))
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            /* Two additional fds were opened, but one is closed, so the
             * net difference should be one extra.
             *
             * There will be an additional extra fd for file used to
             * coordinate the process lock. */

            unsigned openFds = countFds();

            if (5 != openFds)
            {
                fprintf(stderr, "%u %u\n", __LINE__, openFds);
                break;
            }

            if (ownFdValid(forkTest.mPipeFds[0]))
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            if (1 != writeFd(forkTest.mPipeFds[1], "X", 1, 0))
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            forkTest.mPipeFds[1] = closeFd(forkTest.mPipeFds[1]);

            rc = 0;

        } while (0);

        if (rc)
            execl("/bin/false", "false", (char *) 0);
        else
            execl("/bin/true", "true", (char *) 0);
        _exit(EXIT_FAILURE);
    }
    else
    {
        EXPECT_EQ(forkTest.mChildPid.mPid, childPid.mPid);

        EXPECT_EQ(0, ownFdValid(forkTest.mPipeFds[1]));

        char buf[1] = { '@' };

        EXPECT_EQ(
            1, readFd(forkTest.mPipeFds[0], buf, sizeof(buf), 0));

        EXPECT_EQ('X', buf[0]);

        forkTest.mPipeFds[0] = closeFd(forkTest.mPipeFds[0]);
    }

    int status;
    EXPECT_EQ(0, reapProcessChild(childPid, &status));
    EXPECT_EQ(0, (extractProcessExitStatus(status, childPid).mStatus));
}

static void
processForkTest_FailedPreFork_()
{
    /* Failure in prefork */

    struct ProcessForkTest forkTest;

    errno = 0;

    struct Pid childPid =
        forkProcessChildX(ForkProcessInheritProcessGroup,
                          Pgid(0),
                          PreForkProcessMethod(
                              LAMBDA(
                                  int, (
                                      struct ProcessForkTest      *self,
                                      const struct PreForkProcess *aFork),
                                  {
                                      errno = EINVAL;
                                      return -1;
                                  }),
                              &forkTest),
                          PostForkChildProcessMethod(
                              LAMBDA(
                                  int, (struct ProcessForkTest *self),
                                  {
                                      abort();

                                      errno = EINVAL;
                                      return -1;
                                  }),
                              &forkTest),
                          PostForkParentProcessMethod(
                              LAMBDA(
                                  int, (struct ProcessForkTest *self,
                                        struct Pid              aChildPid),
                                  {
                                      abort();

                                      errno = EINVAL;
                                      return -1;
                                  }),
                              &forkTest),
                          ForkProcessMethodNil());

    EXPECT_EQ(-1, childPid.mPid);
    EXPECT_EQ(EINVAL, errno);
}

static void
processForkTest_FailedChildPostFork_()
{
    /* Failure in child postfork */

    struct ProcessForkTest forkTest;

    errno = 0;

    struct Pid childPid =
        forkProcessChildX(ForkProcessInheritProcessGroup,
                          Pgid(0),
                          PreForkProcessMethod(
                              LAMBDA(
                                  int, (
                                      struct ProcessForkTest      *self,
                                      const struct PreForkProcess *aFork),
                                  {
                                      return 0;
                                  }),
                              &forkTest),
                          PostForkChildProcessMethod(
                              LAMBDA(
                                  int, (struct ProcessForkTest *self),
                                  {
                                      errno = EINVAL;
                                      return -1;
                                  }),
                              &forkTest),
                          PostForkParentProcessMethod(
                              LAMBDA(
                                  int, (struct ProcessForkTest *self,
                                        struct Pid              aChildPid),
                                  {
                                      abort();

                                      errno = EINVAL;
                                      return -1;
                                  }),
                              &forkTest),
                          ForkProcessMethodNil());

    EXPECT_EQ(-1, childPid.mPid);
    EXPECT_EQ(EINVAL, errno);
}

static void
processForkTest_FailedParentPostFork_()
{
    /* Failure in parent postfork */

    struct ProcessForkTest forkTest;

    errno = 0;

    struct Pid childPid =
        forkProcessChildX(ForkProcessInheritProcessGroup,
                          Pgid(0),
                          PreForkProcessMethod(
                              LAMBDA(
                                  int, (
                                      struct ProcessForkTest      *self,
                                      const struct PreForkProcess *aFork),
                                  {
                                      return 0;
                                  }),
                              &forkTest),
                          PostForkChildProcessMethod(
                              LAMBDA(
                                  int, (struct ProcessForkTest *self),
                                  {
                                      return 0;
                                  }),
                              &forkTest),
                          PostForkParentProcessMethod(
                              LAMBDA(
                                  int, (struct ProcessForkTest *self,
                                        struct Pid              aChildPid),
                                  {
                                      errno = EINVAL;
                                      return -1;
                                  }),
                              &forkTest),
                          ForkProcessMethodNil());

    EXPECT_EQ(-1, childPid.mPid);
    EXPECT_EQ(EINVAL, errno);
}

static void
processForkTest_(struct ProcessForkArg *aArg)
{
    pthread_mutex_t *lock = lockMutex(aArg->mMutex);

    while ( ! aArg->mStart)
        waitCond(aArg->mCond, lock);

    lock = unlockMutex(lock);

    processForkTest_Trivial_();
    processForkTest_Usual_(aArg);
    processForkTest_FailedPreFork_();
    processForkTest_FailedChildPostFork_();
    processForkTest_FailedParentPostFork_();
}

static void
processForkTest_Raw_(struct ProcessForkArg *aArg)
{
    pthread_mutex_t *lock = lockMutex(aArg->mMutex);

    while ( ! aArg->mStart)
        waitCond(aArg->mCond, lock);

    lock = unlockMutex(lock);

    sleep((ownThreadId().mTid / 2) % 3);

    pid_t childpid = fork();

    EXPECT_NE(-1, childpid);

    if ( ! childpid)
    {
        execl("/bin/true", "true", (char *) 0);
        _exit(EXIT_FAILURE);
    }

    unsigned openFds = countFds();

    EXPECT_GE(openFds, aArg->mNumFds);
    EXPECT_LE(openFds, aArg->mNumFds + aArg->mNumForks);

    struct Pid childPid = Pid(childpid);

    int status;
    EXPECT_EQ(0, reapProcessChild(childPid, &status));
    EXPECT_EQ(0, (extractProcessExitStatus(status, childPid).mStatus));
}

TEST_F(ProcessTest, ProcessFork)
{
    static const char *threadName[2] =
    {
        "thread 1",
        "thread 2",
    };

    struct Thread  thread_[2];
    struct Thread *thread[2] = { 0, 0 };

    struct ProcessForkArg forkArg;

    forkArg.mStart    = false;
    forkArg.mMutex    = createMutex(&forkArg.mMutex_);
    forkArg.mCond     = createCond(&forkArg.mCond_);
    forkArg.mNumFds   = countFds();
    forkArg.mNumForks = NUMBEROF(thread);

    pthread_mutex_t *lock = lockMutex(forkArg.mMutex);
    forkArg.mStart = false;

    for (unsigned ix = 0; ix < NUMBEROF(thread); ++ix)
    {
        thread[ix] = createThread(&thread_[ix],
                                 threadName[ix],
                                  0,
                                  ThreadMethod(
                                      LAMBDA(
                                          int, (struct ProcessForkArg *self),
                                          {
                                              processForkTest_(self);
                                              return 0;
                                          }),
                                      &forkArg));
        EXPECT_TRUE(thread[ix]);
    }

    struct Thread  rawForkThread_;
    struct Thread *rawForkThread;

    rawForkThread = createThread(&rawForkThread_,
                                 "rawkfork",
                                 0,
                                 ThreadMethod(
                                     LAMBDA(
                                         int, (struct ProcessForkArg *self),
                                         {
                                             processForkTest_Raw_(self);

                                             return 0;
                                         }),
                                     &forkArg));
    EXPECT_TRUE(rawForkThread);

    forkArg.mStart = true;
    lock = unlockMutexBroadcast(lock, forkArg.mCond);

    for (unsigned ix = 0; ix < NUMBEROF(thread); ++ix)
        thread[ix] = closeThread(thread[ix]);

    rawForkThread = closeThread(rawForkThread);

    int status;
    EXPECT_EQ(-1, wait(&status));
    EXPECT_EQ(ECHILD, errno);

    forkArg.mCond  = destroyCond(forkArg.mCond);
    forkArg.mMutex = destroyMutex(forkArg.mMutex);
}

#include "../googletest/src/gtest_main.cc"
