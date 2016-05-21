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

#include "eventqueue_.h"
#include "bellsocketpair_.h"
#include "timekeeping_.h"
#include "macros_.h"

#include "gtest/gtest.h"

TEST(EventQueueTest, CreatePushPopClose)
{
    struct EventQueue      eventQueue_;
    struct EventQueue     *eventQueue = 0;

    struct EventQueueFile  eventFile_;
    struct EventQueueFile *eventFile = 0;

    struct BellSocketPair  testSocket_;
    struct BellSocketPair *testSocket = 0;

    EXPECT_EQ(0, createEventQueue(&eventQueue_));
    eventQueue = &eventQueue_;

    EXPECT_EQ(0, createBellSocketPair(&testSocket_, 0));
    testSocket = &testSocket_;

    /* Create the event queue file, push it but and pop the event queue.
     * This is the expected life cycle of the event file. */

    EXPECT_EQ(0, createEventQueueFile(
                  &eventFile_,
                  eventQueue,
                  testSocket->mSocketPair->mParentSocket->mFile,
                  EventQueuePollRead,
                  EventQueueHandle(testSocket)));
    eventFile = &eventFile_;

    struct Duration        zeroDuration = Duration(NanoSeconds(0));
    struct EventQueueFile *polledEvents[2];

    EXPECT_EQ(0, popEventQueue(
        eventQueue, polledEvents, NUMBEROF(polledEvents), &zeroDuration));

    EXPECT_EQ(0, pushEventQueue(eventQueue, eventFile));
    EXPECT_EQ(0, popEventQueue(
        eventQueue, polledEvents, NUMBEROF(polledEvents), &zeroDuration));

    EXPECT_EQ(0, ringBellSocketPairChild(testSocket));
    EXPECT_EQ(0, popEventQueue(eventQueue, 0, 0, &zeroDuration));
    EXPECT_EQ(1, popEventQueue(
        eventQueue, polledEvents, NUMBEROF(polledEvents), 0));
    EXPECT_EQ(polledEvents[0], eventFile);

    EXPECT_EQ(0, popEventQueue(
        eventQueue, polledEvents, NUMBEROF(polledEvents), &zeroDuration));

    EXPECT_EQ(0, pushEventQueue(eventQueue, eventFile));
    EXPECT_EQ(1, popEventQueue(
        eventQueue, polledEvents, NUMBEROF(polledEvents), 0));

    EXPECT_EQ(0, popEventQueue(
        eventQueue, polledEvents, NUMBEROF(polledEvents), &zeroDuration));

    eventFile = closeEventQueueFile(eventFile);

    /* Create the event queue file, push it but do not pop the event queue.
     * Simply close the event queue file, and then verify that it has
     * taken itself off the event queue. */

    EXPECT_EQ(0, popEventQueue(
        eventQueue, polledEvents, NUMBEROF(polledEvents), &zeroDuration));

    EXPECT_EQ(0, createEventQueueFile(
                  &eventFile_,
                  eventQueue,
                  testSocket->mSocketPair->mParentSocket->mFile,
                  EventQueuePollRead,
                  EventQueueHandle(testSocket)));
    eventFile = &eventFile_;

    EXPECT_EQ(0, pushEventQueue(eventQueue, eventFile));
    EXPECT_EQ(1, popEventQueue(
        eventQueue, polledEvents, NUMBEROF(polledEvents), 0));
    EXPECT_EQ(polledEvents[0], eventFile);
    EXPECT_EQ(0, pushEventQueue(eventQueue, eventFile));
    eventFile = closeEventQueueFile(eventFile);

    EXPECT_EQ(0, popEventQueue(
        eventQueue, polledEvents, NUMBEROF(polledEvents), &zeroDuration));

    testSocket = closeBellSocketPair(testSocket);
    eventQueue = closeEventQueue(eventQueue);
}

TEST(EventQueueTest, CreateCloseEventFile)
{
    struct EventQueue      eventQueue_;
    struct EventQueue     *eventQueue = 0;

    struct EventQueueFile  eventFile_;
    struct EventQueueFile *eventFile = 0;

    struct BellSocketPair  testSocket_;
    struct BellSocketPair *testSocket = 0;

    EXPECT_EQ(0, createEventQueue(&eventQueue_));
    eventQueue = &eventQueue_;

    EXPECT_EQ(0, createBellSocketPair(&testSocket_, 0));
    testSocket = &testSocket_;

    /* Create the event queue file, then immediately close it to
     * verify that it can be cleaned up. */

    EXPECT_EQ(0, createEventQueueFile(
                  &eventFile_,
                  eventQueue,
                  testSocket->mSocketPair->mParentSocket->mFile,
                  EventQueuePollRead,
                  EventQueueHandle(testSocket)));
    eventFile = &eventFile_;

    eventFile  = closeEventQueueFile(eventFile);
    testSocket = closeBellSocketPair(testSocket);
    eventQueue = closeEventQueue(eventQueue);
}

#include "../googletest/src/gtest_main.cc"
