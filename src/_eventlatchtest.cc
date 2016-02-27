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

#include "eventlatch_.h"
#include "eventpipe_.h"

#include "gtest/gtest.h"

TEST(EventLatchTest, SetReset)
{
    struct EventLatch eventLatch;

    EXPECT_EQ(0, createEventLatch(&eventLatch));
    EXPECT_EQ(0, ownEventLatchSetting(&eventLatch));

    EXPECT_EQ(1, setEventLatch(&eventLatch));
    EXPECT_EQ(1, ownEventLatchSetting(&eventLatch));

    EXPECT_EQ(0, setEventLatch(&eventLatch));
    EXPECT_EQ(1, ownEventLatchSetting(&eventLatch));

    EXPECT_EQ(0, setEventLatch(&eventLatch));
    EXPECT_EQ(1, ownEventLatchSetting(&eventLatch));

    EXPECT_EQ(1, resetEventLatch(&eventLatch));
    EXPECT_EQ(0, ownEventLatchSetting(&eventLatch));

    EXPECT_EQ(0, resetEventLatch(&eventLatch));
    EXPECT_EQ(0, ownEventLatchSetting(&eventLatch));

    EXPECT_EQ(0, resetEventLatch(&eventLatch));
    EXPECT_EQ(0, ownEventLatchSetting(&eventLatch));
    EXPECT_EQ(0, closeEventLatch(&eventLatch));
}

TEST(EventLatchTest, DisableSetReset)
{
    struct EventLatch eventLatch;

    EXPECT_EQ(0, createEventLatch(&eventLatch));
    EXPECT_EQ(0, ownEventLatchSetting(&eventLatch));

    EXPECT_EQ(0, disableEventLatch(&eventLatch));
    errno = 0;
    EXPECT_EQ(-1, disableEventLatch(&eventLatch));
    EXPECT_EQ(ERANGE, errno);
    errno = 0;
    EXPECT_EQ(-1, ownEventLatchSetting(&eventLatch));
    EXPECT_EQ(ERANGE, errno);

    errno = 0;
    EXPECT_EQ(-1, setEventLatch(&eventLatch));
    EXPECT_EQ(ERANGE, errno);
    errno = 0;
    EXPECT_EQ(-1, resetEventLatch(&eventLatch));
    EXPECT_EQ(ERANGE, errno);
    EXPECT_EQ(0, closeEventLatch(&eventLatch));
}

TEST(EventLatchTest, SetDisableSetReset)
{
    struct EventLatch eventLatch;

    EXPECT_EQ(0, createEventLatch(&eventLatch));
    EXPECT_EQ(0, ownEventLatchSetting(&eventLatch));

    EXPECT_EQ(1, setEventLatch(&eventLatch));
    EXPECT_EQ(1, ownEventLatchSetting(&eventLatch));

    EXPECT_EQ(0, disableEventLatch(&eventLatch));
    errno = 0;
    EXPECT_EQ(-1, disableEventLatch(&eventLatch));
    EXPECT_EQ(ERANGE, errno);
    errno = 0;
    EXPECT_EQ(-1, ownEventLatchSetting(&eventLatch));
    EXPECT_EQ(ERANGE, errno);

    errno = 0;
    EXPECT_EQ(-1, setEventLatch(&eventLatch));
    EXPECT_EQ(ERANGE, errno);
    errno = 0;
    EXPECT_EQ(-1, resetEventLatch(&eventLatch));
    EXPECT_EQ(ERANGE, errno);
    EXPECT_EQ(0, closeEventLatch(&eventLatch));
}

TEST(EventLatchTest, Pipe)
{
    struct EventLatch eventLatch;
    struct EventPipe  eventPipe;

    EXPECT_EQ(0, createEventLatch(&eventLatch));
    EXPECT_EQ(0, createEventPipe(&eventPipe, 0));

    bindEventLatchPipe(&eventLatch, &eventPipe);

    EXPECT_EQ(0, ownEventLatchSetting(&eventLatch));

    EXPECT_EQ(0, resetEventPipe(&eventPipe));
    EXPECT_EQ(1, setEventLatch(&eventLatch));
    EXPECT_EQ(1, ownEventLatchSetting(&eventLatch));
    EXPECT_EQ(1, resetEventPipe(&eventPipe));
    EXPECT_EQ(0, resetEventPipe(&eventPipe));
    EXPECT_EQ(1, resetEventLatch(&eventLatch));
    EXPECT_EQ(0, resetEventLatch(&eventLatch));
    EXPECT_EQ(0, resetEventPipe(&eventPipe));

    EXPECT_EQ(0, resetEventPipe(&eventPipe));
    EXPECT_EQ(1, setEventLatch(&eventLatch));
    EXPECT_EQ(1, ownEventLatchSetting(&eventLatch));
    EXPECT_EQ(1, resetEventLatch(&eventLatch));
    EXPECT_EQ(0, resetEventLatch(&eventLatch));
    EXPECT_EQ(1, resetEventPipe(&eventPipe));
    EXPECT_EQ(0, resetEventPipe(&eventPipe));
    EXPECT_EQ(0, resetEventLatch(&eventLatch));
    EXPECT_EQ(0, resetEventPipe(&eventPipe));

    EXPECT_EQ(0, closeEventPipe(&eventPipe));
    EXPECT_EQ(0, closeEventLatch(&eventLatch));
}

#include "../googletest/src/gtest_main.cc"
