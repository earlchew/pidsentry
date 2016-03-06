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

#include "eventpipe_.h"
#include "timekeeping_.h"

#include "gtest/gtest.h"

TEST(EventPipeTest, ResetOnce)
{
    struct EventPipe eventPipe;

    EXPECT_EQ(0, createEventPipe(&eventPipe, 0));

    EXPECT_EQ(0, resetEventPipe(&eventPipe));

    struct Duration zeroTimeout = Duration(NanoSeconds(0));

    EXPECT_EQ(0, waitFileReadReady(eventPipe.mPipe->mRdFile, &zeroTimeout));

    closeEventPipe(&eventPipe);
}

TEST(EventPipeTest, SetOnce)
{
    struct EventPipe eventPipe;

    EXPECT_EQ(0, createEventPipe(&eventPipe, 0));

    EXPECT_EQ(1, setEventPipe(&eventPipe));

    struct Duration zeroTimeout = Duration(NanoSeconds(0));

    EXPECT_EQ(1, waitFileReadReady(eventPipe.mPipe->mRdFile, &zeroTimeout));

    char buf[1];
    EXPECT_EQ(1, readFile(eventPipe.mPipe->mRdFile, buf, 1));

    EXPECT_EQ(0, waitFileReadReady(eventPipe.mPipe->mRdFile, &zeroTimeout));

    closeEventPipe(&eventPipe);
}

TEST(EventPipeTest, SetTwice)
{
    struct EventPipe eventPipe;

    EXPECT_EQ(0, createEventPipe(&eventPipe, 0));

    EXPECT_EQ(1, setEventPipe(&eventPipe));
    EXPECT_EQ(0, setEventPipe(&eventPipe));

    struct Duration zeroTimeout = Duration(NanoSeconds(0));

    EXPECT_EQ(1, waitFileReadReady(eventPipe.mPipe->mRdFile, &zeroTimeout));

    char buf[1];
    EXPECT_EQ(1, readFile(eventPipe.mPipe->mRdFile, buf, 1));

    EXPECT_EQ(0, waitFileReadReady(eventPipe.mPipe->mRdFile, &zeroTimeout));

    closeEventPipe(&eventPipe);
}

TEST(EventPipeTest, SetOnceResetOnce)
{
    struct EventPipe eventPipe;

    EXPECT_EQ(0, createEventPipe(&eventPipe, 0));

    EXPECT_EQ(1, setEventPipe(&eventPipe));
    EXPECT_EQ(1, resetEventPipe(&eventPipe));

    struct Duration zeroTimeout = Duration(NanoSeconds(0));

    EXPECT_EQ(0, waitFileReadReady(eventPipe.mPipe->mRdFile, &zeroTimeout));

    EXPECT_EQ(0, resetEventPipe(&eventPipe));

    closeEventPipe(&eventPipe);
}

TEST(EventPipeTest, SetOnceResetTwice)
{
    struct EventPipe eventPipe;

    EXPECT_EQ(0, createEventPipe(&eventPipe, 0));

    EXPECT_EQ(1, setEventPipe(&eventPipe));
    EXPECT_EQ(1, resetEventPipe(&eventPipe));
    EXPECT_EQ(0, resetEventPipe(&eventPipe));

    struct Duration zeroTimeout = Duration(NanoSeconds(0));

    EXPECT_EQ(0, waitFileReadReady(eventPipe.mPipe->mRdFile, &zeroTimeout));

    EXPECT_EQ(0, resetEventPipe(&eventPipe));

    closeEventPipe(&eventPipe);
}

TEST(EventPipeTest, SetOnceResetOnceSetOnce)
{
    struct EventPipe eventPipe;

    EXPECT_EQ(0, createEventPipe(&eventPipe, 0));

    EXPECT_EQ(1, setEventPipe(&eventPipe));
    EXPECT_EQ(1, resetEventPipe(&eventPipe));
    EXPECT_EQ(1, setEventPipe(&eventPipe));

    struct Duration zeroTimeout = Duration(NanoSeconds(0));

    EXPECT_EQ(1, waitFileReadReady(eventPipe.mPipe->mRdFile, &zeroTimeout));

    char buf[1];
    EXPECT_EQ(1, readFile(eventPipe.mPipe->mRdFile, buf, 1));

    EXPECT_EQ(0, waitFileReadReady(eventPipe.mPipe->mRdFile, &zeroTimeout));

    closeEventPipe(&eventPipe);
}

#include "../googletest/src/gtest_main.cc"
