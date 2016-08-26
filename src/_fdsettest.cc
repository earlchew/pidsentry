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

#include "fdset_.h"

#include "gtest/gtest.h"

TEST(FdTest, CreateDestroy)
{
    struct FdSet  fdset_;
    struct FdSet *fdset = 0;

    EXPECT_EQ(0, createFdSet(&fdset_));
    fdset = &fdset_;

    EXPECT_EQ(0, insertFdSetRange(fdset, 0, 2));
    EXPECT_NE(0, insertFdSetRange(fdset, 1, 2));
    EXPECT_EQ(EEXIST, errno);

    fdset = closeFdSet(fdset);
}

TEST(FdTest, InsertRemove)
{
    struct FdSet  fdset_;
    struct FdSet *fdset = 0;

    EXPECT_EQ(0, createFdSet(&fdset_));
    fdset = &fdset_;

    EXPECT_EQ(0, insertFdSetRange(fdset, 0, 2));
    EXPECT_NE(0, insertFdSetRange(fdset, 0, -1));
    EXPECT_EQ(0, removeFdSetRange(fdset, 0, 2));
    EXPECT_NE(0, removeFdSetRange(fdset, 0, -1));
    EXPECT_NE(0, removeFdSetRange(fdset, 0, 2));
    EXPECT_EQ(ENOENT, errno);

    fdset = closeFdSet(fdset);
}

TEST(FdTest, InsertOverlap)
{
    struct FdSet  fdset_;
    struct FdSet *fdset = 0;

    EXPECT_EQ(0, createFdSet(&fdset_));
    fdset = &fdset_;

    EXPECT_EQ(0, insertFdSetRange(fdset, 0, 2));
    EXPECT_EQ(0, insertFdSetRange(fdset, 4, 6));

    EXPECT_NE(0, insertFdSetRange(fdset, -1, 1));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, -1, 2));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, -1, 3));
    EXPECT_EQ(EEXIST, errno);

    EXPECT_NE(0, insertFdSetRange(fdset, 0, 1));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, 0, 2));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, 0, 3));
    EXPECT_EQ(EEXIST, errno);

    EXPECT_NE(0, insertFdSetRange(fdset, 1, 2));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, 1, 3));
    EXPECT_EQ(EEXIST, errno);

    EXPECT_NE(0, insertFdSetRange(fdset, 3, 4));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, 3, 5));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, 3, 6));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, 3, 7));
    EXPECT_EQ(EEXIST, errno);

    EXPECT_NE(0, insertFdSetRange(fdset, 4, 4));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, 4, 5));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, 4, 6));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, 4, 7));
    EXPECT_EQ(EEXIST, errno);

    EXPECT_NE(0, insertFdSetRange(fdset, 5, 6));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, 5, 7));
    EXPECT_EQ(EEXIST, errno);

    EXPECT_NE(0, insertFdSetRange(fdset, 6, 6));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, 6, 7));
    EXPECT_EQ(EEXIST, errno);

    fdset = closeFdSet(fdset);
}

struct TestVisitor
{
    int mNext;
    int mStop;

    static int
    visit(struct TestVisitor *self, int aFirstFd, int aLastFd)
    {
        EXPECT_EQ(aFirstFd, aLastFd);
        EXPECT_EQ(self->mNext, aFirstFd);

        if (aFirstFd == self->mStop)
            return 1;

        ++self->mNext;

        return 0;
    }
};

TEST(FdTest, Visitor)
{
    struct FdSet  fdset_;
    struct FdSet *fdset = 0;

    EXPECT_EQ(0, createFdSet(&fdset_));
    fdset = &fdset_;

    EXPECT_EQ(0, insertFdSetRange(fdset, 0, 0));
    EXPECT_EQ(0, insertFdSetRange(fdset, 1, 1));
    EXPECT_EQ(0, insertFdSetRange(fdset, 2, 2));

    struct TestVisitor testVisitor;

    testVisitor.mNext = 0;
    testVisitor.mStop = -1;

    EXPECT_EQ(
        3,
        visitFdSet(fdset, FdSetVisitor(TestVisitor::visit, &testVisitor)));

    EXPECT_EQ(3, testVisitor.mNext);

    testVisitor.mNext = 0;
    testVisitor.mStop = 1;

    EXPECT_EQ(
        2,
        visitFdSet(fdset, FdSetVisitor(TestVisitor::visit, &testVisitor)));

    EXPECT_EQ(1, testVisitor.mNext);

    fdset = closeFdSet(fdset);
}

#include "../googletest/src/gtest_main.cc"
