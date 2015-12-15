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

#include "timekeeping_.h"

#include "gtest/gtest.h"

bool
operator==(const struct timespec &aLhs, const struct timespec &aRhs)
{
    return aLhs.tv_sec == aRhs.tv_sec && aLhs.tv_nsec == aRhs.tv_nsec;
}

bool
operator==(const struct timeval &aLhs, const struct timeval &aRhs)
{
    return aLhs.tv_sec == aRhs.tv_sec && aLhs.tv_usec == aRhs.tv_usec;
}

bool
operator==(const struct itimerval &aLhs, const struct itimerval &aRhs)
{
    return
        aLhs.it_value    == aRhs.it_value &&
        aLhs.it_interval == aRhs.it_interval;
}

TEST(TimeKeepingTest, DeadlineRunsOnce)
{
    uint64_t since = 0;

    EXPECT_FALSE(deadlineTimeExpired(&since, 0, 0));
}

TEST(TimeKeepingTest, DeadlineExpires)
{
    auto duration_ns = milliSeconds(1000);

    uint64_t since = 0;
    uint64_t remaining;

    auto startTimeOuter = monotonicTime();
    EXPECT_FALSE(deadlineTimeExpired(&since, duration_ns, &remaining));
    EXPECT_EQ(duration_ns, remaining);
    auto startTimeInner = monotonicTime();

    bool firstiteration = true;
    while ( ! deadlineTimeExpired(&since, duration_ns, &remaining))
    {
        EXPECT_TRUE(firstiteration || remaining);
        firstiteration = false;
    }
    EXPECT_TRUE( ! remaining);

    auto stopTime = monotonicTime();

    auto elapsedInner = toMilliSeconds(stopTime - startTimeInner) / 100;
    auto elapsedOuter = toMilliSeconds(stopTime - startTimeOuter) / 100;

    auto duration = toMilliSeconds(duration_ns) / 100;

    EXPECT_LE(elapsedInner, duration);
    EXPECT_GE(elapsedOuter, duration);
}

TEST(TimeKeepingTest, MonotonicSleep)
{
    auto duration_ns = milliSeconds(1000);

    auto startTime = monotonicTime();
    monotonicSleep(duration_ns);
    auto stopTime = monotonicTime();

    auto elapsedTime = toMilliSeconds(stopTime - startTime) / 100;
    auto duration    = toMilliSeconds(duration_ns) / 100;

    EXPECT_EQ(duration, elapsedTime);
}

TEST(TimeKeepingTest, LapTimeSinceNull)
{
    {
        auto startTime = monotonicTime();
        auto lapTime  = lapTimeSince(0, 0);
        auto stopTime = monotonicTime();

        EXPECT_LE(startTime, lapTime);
        EXPECT_GE(stopTime,  lapTime);
    }

    {
        auto startTime = monotonicTime();
        auto lapTime  = lapTimeSince(0, 1);
        auto stopTime = monotonicTime();

        EXPECT_LE(startTime, lapTime);
        EXPECT_GE(stopTime,  lapTime);
    }
}

TEST(TimeKeepingTest, LapTimeSinceNoPeriod)
{
    auto duration_ns = milliSeconds(1000);

    uint64_t since = 0;

    EXPECT_FALSE(lapTimeSince(&since, 0));

    {
        monotonicSleep(duration_ns);

        auto lapTime =
            toMilliSeconds(lapTimeSince(&since, 0)) / 100;

        auto duration =
            toMilliSeconds(duration_ns) / 100;

        EXPECT_EQ(1 * duration, lapTime);

        lapTime =
            toMilliSeconds(lapTimeSince(&since, 0)) / 100;

        EXPECT_EQ(1 * duration, lapTime);
    }

    {
        monotonicSleep(duration_ns);

        auto lapTime =
            toMilliSeconds(lapTimeSince(&since, 0)) / 100;

        auto duration =
            toMilliSeconds(duration_ns) / 100;

        EXPECT_EQ(2 * duration, lapTime);

        lapTime =
            toMilliSeconds(lapTimeSince(&since, 0)) / 100;

        EXPECT_EQ(2 * duration, lapTime);
    }
}

TEST(TimeKeepingTest, LapTimeSinceWithPeriod)
{
    auto duration_ns = milliSeconds(1000);
    auto period_ns   = milliSeconds(5000);

    uint64_t since = 0;

    EXPECT_FALSE(lapTimeSince(&since, 0));

    {
        monotonicSleep(duration_ns);

        uint64_t lapTime =
            toMilliSeconds(lapTimeSince(&since, period_ns)) / 100;

        auto duration =
            toMilliSeconds(duration_ns) / 100;

        EXPECT_EQ(1 * duration, lapTime);

        lapTime =
            toMilliSeconds(lapTimeSince(&since, period_ns)) / 100;

        EXPECT_EQ(1 * duration, lapTime);
    }

    {
        monotonicSleep(duration_ns);

        uint64_t lapTime =
            toMilliSeconds(lapTimeSince(&since, period_ns)) / 100;

        auto duration =
            toMilliSeconds(duration_ns) / 100;

        EXPECT_EQ(2 * duration, lapTime);

        lapTime =
            toMilliSeconds(lapTimeSince(&since, period_ns)) / 100;

        EXPECT_EQ(2 * duration, lapTime);
    }
}

TEST(TimeKeepingTest, EarliestTime)
{
    struct timespec small;
    small.tv_sec  = 1;
    small.tv_nsec = 1000;

    struct timespec medium;
    medium.tv_sec  = 1;
    medium.tv_nsec = 1001;

    struct timespec large;
    large.tv_sec  = 2;
    large.tv_nsec = 1000;

    EXPECT_EQ(small,  earliestTime(&small,  &small));
    EXPECT_EQ(large,  earliestTime(&large,  &large));
    EXPECT_EQ(medium, earliestTime(&medium, &medium));

    EXPECT_EQ(small, earliestTime(&small,  &medium));
    EXPECT_EQ(small, earliestTime(&medium, &small));

    EXPECT_EQ(small, earliestTime(&small, &large));
    EXPECT_EQ(small, earliestTime(&large, &small));

    EXPECT_EQ(medium, earliestTime(&large,  &medium));
    EXPECT_EQ(medium, earliestTime(&medium, &large));
}

TEST(TimeKeepingTest, TimeVal)
{
    struct timeval timeVal;
    timeVal.tv_sec  = 1;
    timeVal.tv_usec = 2;

    uint64_t nsTime = (1000 * 1000 + 2) * 1000;

    EXPECT_EQ(nsTime,  timeValToTime(&timeVal));
    EXPECT_EQ(timeVal, timeValFromTime(nsTime +    1));
    EXPECT_EQ(timeVal, timeValFromTime(nsTime + 1000 - 1));
}

TEST(TimeKeepingTest, ShortenTimeInterval)
{
    struct timeval zero;
    zero.tv_sec  = 0;
    zero.tv_usec = 0;

    struct timeval one;
    one.tv_sec  = 1;
    one.tv_usec = 0;

    struct timeval two;
    two.tv_sec  = 2;
    two.tv_usec = 0;

    struct timeval three;
    three.tv_sec  = 3;
    three.tv_usec = 0;

    uint64_t nsTime_1_0 = 1000 * 1000 * 1000;

    struct itimerval alarmVal, shortenedVal;

    // Timer is disabled.
    alarmVal.it_value    = zero;
    alarmVal.it_interval = one;

    shortenedVal.it_value    = zero;
    shortenedVal.it_interval = one;

    EXPECT_EQ(shortenedVal, shortenIntervalTime(&alarmVal, nsTime_1_0 * 1));

    // Elapsed time is less that the outstanding alarm time.
    alarmVal.it_value    = two;
    alarmVal.it_interval = three;

    shortenedVal.it_value    = one;
    shortenedVal.it_interval = three;

    EXPECT_EQ(shortenedVal, shortenIntervalTime(&alarmVal, nsTime_1_0 * 1));

    // Elapsed time equals the outstanding alarm time.
    alarmVal.it_value    = two;
    alarmVal.it_interval = three;

    shortenedVal.it_value    = three;
    shortenedVal.it_interval = three;

    EXPECT_EQ(shortenedVal, shortenIntervalTime(&alarmVal, nsTime_1_0 * 2));

    // Elapsed time exceeds the outstanding alarm time.
    alarmVal.it_value    = two;
    alarmVal.it_interval = three;

    shortenedVal.it_value    = two;
    shortenedVal.it_interval = three;

    EXPECT_EQ(shortenedVal, shortenIntervalTime(&alarmVal, nsTime_1_0 * 3));

    // Elapsed time exceeds the outstanding alarm time and the next interval.
    alarmVal.it_value    = two;
    alarmVal.it_interval = three;

    shortenedVal.it_value    = three;
    shortenedVal.it_interval = three;

    EXPECT_EQ(shortenedVal, shortenIntervalTime(&alarmVal, nsTime_1_0 * 8));

    alarmVal.it_value    = two;
    alarmVal.it_interval = three;

    shortenedVal.it_value    = two;
    shortenedVal.it_interval = three;

    EXPECT_EQ(shortenedVal, shortenIntervalTime(&alarmVal, nsTime_1_0 * 9));

    alarmVal.it_value    = two;
    alarmVal.it_interval = three;

    shortenedVal.it_value    = one;
    shortenedVal.it_interval = three;

    EXPECT_EQ(shortenedVal, shortenIntervalTime(&alarmVal, nsTime_1_0 * 10));

    alarmVal.it_value    = two;
    alarmVal.it_interval = three;

    shortenedVal.it_value    = three;
    shortenedVal.it_interval = three;

    EXPECT_EQ(shortenedVal, shortenIntervalTime(&alarmVal, nsTime_1_0 * 11));
}

TEST(TimeKeepingTest, PushIntervalTimer)
{
    struct PushedIntervalTimer pushedTimer;

    struct sigaction timerAction;
    struct itimerval timerVal;

    // Verify that the interval timer can be pushed when there is
    // no previously configured timer.

    EXPECT_FALSE(getitimer(ITIMER_REAL, &timerVal));
    EXPECT_EQ(0, timerVal.it_value.tv_sec);
    EXPECT_EQ(0, timerVal.it_value.tv_usec);
    EXPECT_EQ(0, timerVal.it_interval.tv_sec);
    EXPECT_EQ(0, timerVal.it_interval.tv_usec);

    timerVal.it_value.tv_sec  = 60 * 60;
    timerVal.it_value.tv_usec = 0;
    timerVal.it_interval.tv_sec  = 60 * 60;
    timerVal.it_interval.tv_usec = 0;
    EXPECT_FALSE(pushIntervalTimer(&pushedTimer, ITIMER_REAL, &timerVal));

    EXPECT_FALSE(getitimer(ITIMER_REAL, &timerVal));
    EXPECT_TRUE(timerVal.it_value.tv_sec || timerVal.it_value.tv_usec);
    EXPECT_EQ(60 * 60, timerVal.it_interval.tv_sec);
    EXPECT_EQ(0,       timerVal.it_interval.tv_usec);

    EXPECT_FALSE(popIntervalTimer(&pushedTimer));

    EXPECT_FALSE(getitimer(ITIMER_REAL, &timerVal));
    EXPECT_EQ(0, timerVal.it_value.tv_sec);
    EXPECT_EQ(0, timerVal.it_value.tv_usec);
    EXPECT_EQ(0, timerVal.it_interval.tv_sec);
    EXPECT_EQ(0, timerVal.it_interval.tv_usec);

    EXPECT_EQ(0, sigaction(SIGALRM, 0, &timerAction));
    EXPECT_EQ(0, timerAction.sa_flags & SA_SIGINFO);
    EXPECT_EQ(SIG_DFL, timerAction.sa_handler);

    // Verify that the interval timer can be pushed when there is
    // a previously configured timer, and verify that the timer
    // is restored.

    timerVal.it_value.tv_sec  = 1;
    timerVal.it_value.tv_usec = 0;
    timerVal.it_interval.tv_sec  = 1 * 60 * 60;
    timerVal.it_interval.tv_usec = 0;
    EXPECT_EQ(0, setitimer(ITIMER_REAL, &timerVal, 0));

    timerAction.sa_handler = SIG_IGN;
    timerAction.sa_flags   = 0;
    EXPECT_EQ(0, sigaction(SIGALRM, &timerAction, 0));

    EXPECT_EQ(0, sigaction(SIGALRM, 0, &timerAction));
    EXPECT_EQ(0,       timerAction.sa_flags & SA_SIGINFO);
    EXPECT_EQ(SIG_IGN, timerAction.sa_handler);

    timerVal.it_value.tv_sec  = 1 * 60 * 60;
    timerVal.it_value.tv_usec = 0;
    timerVal.it_interval.tv_sec  = 2 * 60 * 60;
    timerVal.it_interval.tv_usec = 0;
    EXPECT_FALSE(pushIntervalTimer(&pushedTimer, ITIMER_REAL, &timerVal));

    EXPECT_FALSE(getitimer(ITIMER_REAL, &timerVal));
    EXPECT_TRUE(timerVal.it_value.tv_sec || timerVal.it_value.tv_usec);
    EXPECT_EQ(2 * 60 * 60, timerVal.it_interval.tv_sec);
    EXPECT_EQ(0,           timerVal.it_interval.tv_usec);

    EXPECT_EQ(0, sigaction(SIGALRM, 0, &timerAction));
    if (timerAction.sa_flags & SA_SIGINFO)
        EXPECT_TRUE(timerAction.sa_sigaction);
    else
        EXPECT_TRUE(
            SIG_ERR != timerAction.sa_handler &&
            SIG_IGN != timerAction.sa_handler &&
            SIG_DFL != timerAction.sa_handler);

    EXPECT_FALSE(popIntervalTimer(&pushedTimer));

    EXPECT_FALSE(getitimer(ITIMER_REAL, &timerVal));
    EXPECT_TRUE(timerVal.it_value.tv_sec || timerVal.it_value.tv_usec);
    EXPECT_EQ(1 * 60 * 60, timerVal.it_interval.tv_sec);
    EXPECT_EQ(0,           timerVal.it_interval.tv_usec);

    EXPECT_EQ(0, sigaction(SIGALRM, 0, &timerAction));
    EXPECT_EQ(0,       timerAction.sa_flags & SA_SIGINFO);
    EXPECT_EQ(SIG_IGN, timerAction.sa_handler);

    timerVal.it_value.tv_sec  = 0;
    timerVal.it_value.tv_usec = 0;
    timerVal.it_interval.tv_sec  = 0;
    timerVal.it_interval.tv_usec = 0;
    EXPECT_EQ(0, setitimer(ITIMER_REAL, &timerVal, 0));

    timerAction.sa_handler = SIG_DFL;
    timerAction.sa_flags   = 0;
    EXPECT_EQ(0, sigaction(SIGALRM, &timerAction, 0));
}

#include "../googletest/src/gtest_main.cc"
