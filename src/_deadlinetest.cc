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

#include "deadline_.h"

#include <memory>

#include "gtest/gtest.h"

class DeadlineTest : public ::testing::Test
{
    void SetUp()
    {
        mResult = 0;
    }

    void TearDown()
    { }

public:

    int mResult;
};

TEST_F(DeadlineTest, ErrorReturn)
{
    class DeadlineTest *fixture = this;

    struct Deadline  deadline_;
    struct Deadline *deadline = 0;

    EXPECT_EQ(0, createDeadline(&deadline_, 0));
    deadline = &deadline_;

    // Verify that an error return from the poll method returns immediately.

    EXPECT_EQ(-1,
              checkDeadlineExpired(
                  deadline,
                  DeadlinePollMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *),
                          {
                              errno = EPERM;
                              return -1;
                          })),
                  DeadlineWaitMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *,
                                const struct Duration *aTimeout),
                          {
                              EXPECT_TRUE(false);
                              return 0;
                          }))));
    EXPECT_EQ(EPERM, errno);

    // Verify that an error return from the wait method returns.

    EXPECT_EQ(-1,
              checkDeadlineExpired(
                  deadline,
                  DeadlinePollMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *),
                          {
                              return 0;
                          })),
                  DeadlineWaitMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *,
                                const struct Duration *aTimeout),
                          {
                              EXPECT_FALSE(aTimeout);
                              errno = EINVAL;
                              return -1;
                          }))));
    EXPECT_EQ(EINVAL, errno);

    deadline = closeDeadline(deadline);
}

TEST_F(DeadlineTest, SuccessReturn)
{
    class DeadlineTest *fixture = this;

    struct Deadline  deadline_;
    struct Deadline *deadline = 0;

    EXPECT_EQ(0, createDeadline(&deadline_, 0));
    deadline = &deadline_;

    // Verify a successful return from the poll method.

    EXPECT_EQ(1,
              checkDeadlineExpired(
                  deadline,
                  DeadlinePollMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *self),
                          {
                              self->mResult = 1;
                              return 1;
                          })),
                  DeadlineWaitMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *self,
                                const struct Duration *aTimeout),
                          {
                              EXPECT_TRUE(false);
                              self->mResult = 2;
                              return 0;
                          }))));
    EXPECT_EQ(1, mResult);
    EXPECT_FALSE(ownDeadlineExpired(deadline));

    // Verify a successful return from the wait method.

    EXPECT_EQ(1,
              checkDeadlineExpired(
                  deadline,
                  DeadlinePollMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *self),
                          {
                              return 0;
                          })),
                  DeadlineWaitMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *self,
                                const struct Duration *aTimeout),
                          {
                              self->mResult = 2;
                              return 1;
                          }))));
    EXPECT_EQ(2, mResult);
    EXPECT_FALSE(ownDeadlineExpired(deadline));

    deadline = closeDeadline(deadline);
}

TEST_F(DeadlineTest, InfiniteTimeout)
{
    class DeadlineTest *fixture = this;

    struct Deadline  deadline_;
    struct Deadline *deadline = 0;

    EXPECT_EQ(0, createDeadline(&deadline_, 0));
    deadline = &deadline_;

    // Verify that an infinite timeout does not expire.

    for (unsigned ix = 0; 100 > ix; ++ix)
    {
        EXPECT_EQ(0,
                  checkDeadlineExpired(
                      deadline,
                      DeadlinePollMethod(
                          fixture,
                          LAMBDA(
                              int, (class DeadlineTest *),
                              {
                                  return 0;
                              })),
                      DeadlineWaitMethod(
                          fixture,
                          LAMBDA(
                              int, (class DeadlineTest *,
                                    const struct Duration *aTimeout),
                              {
                                  EXPECT_FALSE(aTimeout);
                                  return 0;
                              }))));
        EXPECT_FALSE(ownDeadlineExpired(deadline));
    }

    deadline = closeDeadline(deadline);
}

TEST_F(DeadlineTest, ZeroTimeout)
{
    class DeadlineTest *fixture = this;

    struct Deadline  deadline_;
    struct Deadline *deadline = 0;

    EXPECT_EQ(0, createDeadline(&deadline_, &ZeroDuration));
    deadline = &deadline_;

    // Verify that a zero timeout is not expired on the first iteration.

    EXPECT_EQ(0,
              checkDeadlineExpired(
                  deadline,
                  DeadlinePollMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *),
                          {
                              return 0;
                          })),
                  DeadlineWaitMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *,
                                const struct Duration *aTimeout),
                          {
                              EXPECT_FALSE(aTimeout->duration.ns);
                              return 0;
                          }))));
    EXPECT_FALSE(ownDeadlineExpired(deadline));

    EXPECT_EQ(-1,
              checkDeadlineExpired(
                  deadline,
                  DeadlinePollMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *),
                          {
                              return 0;
                          })),
                  DeadlineWaitMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *,
                                const struct Duration *aTimeout),
                          {
                              EXPECT_TRUE(false);
                              return 0;
                          }))));
    EXPECT_EQ(ETIMEDOUT, errno);
    EXPECT_TRUE(ownDeadlineExpired(deadline));

    // Verify that once expired, the deadline remains expired.

    EXPECT_EQ(-1,
              checkDeadlineExpired(
                  deadline,
                  DeadlinePollMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *),
                          {
                              return 0;
                          })),
                  DeadlineWaitMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *,
                                const struct Duration *aTimeout),
                          {
                              EXPECT_TRUE(false);
                              return 0;
                          }))));
    EXPECT_EQ(ETIMEDOUT, errno);
    EXPECT_TRUE(ownDeadlineExpired(deadline));

    deadline = closeDeadline(deadline);
}

TEST_F(DeadlineTest, NonZeroTimeout)
{
    class DeadlineTest *fixture = this;

    struct Deadline  deadline_;
    struct Deadline *deadline = 0;

    struct Duration oneSecond = Duration(NSECS(Seconds(1)));

    EXPECT_EQ(0, createDeadline(&deadline_, &oneSecond));
    deadline = &deadline_;

    // Verify that the deadline is never expired on the first iteration.

    EXPECT_EQ(0,
              checkDeadlineExpired(
                  deadline,
                  DeadlinePollMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *),
                          {
                              return 0;
                          })),
                  DeadlineWaitMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *,
                                const struct Duration *aTimeout),
                          {
                              EXPECT_TRUE(aTimeout);
                              return 0;
                          }))));
    EXPECT_FALSE(ownDeadlineExpired(deadline));

    // Verify that the deadline is not expired on the second iteration.

    EXPECT_EQ(0,
              checkDeadlineExpired(
                  deadline,
                  DeadlinePollMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *),
                          {
                              return 0;
                          })),
                  DeadlineWaitMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *,
                                const struct Duration *aTimeout),
                          {
                              EXPECT_TRUE(aTimeout);
                              monotonicSleep(*aTimeout);
                              return 0;
                          }))));
    EXPECT_FALSE(ownDeadlineExpired(deadline));

    // Verify that the deadline is expired on the third iteration.

    EXPECT_EQ(-1,
              checkDeadlineExpired(
                  deadline,
                  DeadlinePollMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *),
                          {
                              return 0;
                          })),
                  DeadlineWaitMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *,
                                const struct Duration *aTimeout),
                          {
                              EXPECT_FALSE(true);
                              return 0;
                          }))));
    EXPECT_EQ(ETIMEDOUT, errno);
    EXPECT_TRUE(ownDeadlineExpired(deadline));

    deadline = closeDeadline(deadline);
}

TEST_F(DeadlineTest, NonZeroTimeoutAlwaysReady)
{
    class DeadlineTest *fixture = this;

    struct Deadline  deadline_;
    struct Deadline *deadline = 0;

    struct Duration oneNanosecond = Duration(NanoSeconds(1));

    EXPECT_EQ(0, createDeadline(&deadline_, &oneNanosecond));
    deadline = &deadline_;

    // Verify that the first iteration always succeeds.

    EXPECT_EQ(1,
              checkDeadlineExpired(
                  deadline,
                  DeadlinePollMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *),
                          {
                              return 1;
                          })),
                  DeadlineWaitMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *,
                                const struct Duration *aTimeout),
                          {
                              EXPECT_FALSE(aTimeout);
                              EXPECT_TRUE(false);
                              return 0;
                          }))));
    EXPECT_FALSE(ownDeadlineExpired(deadline));

    // Verify that the second iteration expires.

    monotonicSleep(Duration(NSECS(Seconds(1))));

    EXPECT_EQ(-1,
              checkDeadlineExpired(
                  deadline,
                  DeadlinePollMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *),
                          {
                              return 1;
                          })),
                  DeadlineWaitMethod(
                      fixture,
                      LAMBDA(
                          int, (class DeadlineTest *,
                                const struct Duration *aTimeout),
                          {
                              EXPECT_FALSE(aTimeout);
                              EXPECT_TRUE(false);
                              return 0;
                          }))));
    EXPECT_EQ(ETIMEDOUT, errno);
    EXPECT_TRUE(ownDeadlineExpired(deadline));

    deadline = closeDeadline(deadline);
}

#include "../googletest/src/gtest_main.cc"
