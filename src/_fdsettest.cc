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

#include <sys/resource.h>

#include <limits.h>

TEST(FdTest, RangeContains)
{
    EXPECT_EQ( 0, containsFdRange(FdRange(20, 29), FdRange(10, 19)));
    EXPECT_EQ( 0, containsFdRange(FdRange(20, 29), FdRange(10, 20)));
    EXPECT_EQ( 0, containsFdRange(FdRange(20, 29), FdRange(10, 25)));
    EXPECT_EQ(+1, containsFdRange(FdRange(20, 29), FdRange(20, 20)));
    EXPECT_EQ(+1, containsFdRange(FdRange(20, 29), FdRange(20, 25)));
    EXPECT_EQ(+3, containsFdRange(FdRange(20, 29), FdRange(20, 29)));
    EXPECT_EQ(-1, containsFdRange(FdRange(20, 29), FdRange(21, 28)));
    EXPECT_EQ(+2, containsFdRange(FdRange(20, 29), FdRange(25, 29)));
    EXPECT_EQ(+2, containsFdRange(FdRange(20, 29), FdRange(29, 29)));
    EXPECT_EQ( 0, containsFdRange(FdRange(20, 29), FdRange(25, 35)));
    EXPECT_EQ( 0, containsFdRange(FdRange(20, 29), FdRange(30, 39)));
}

TEST(FdTest, RightNeighbour)
{
    EXPECT_FALSE(rightFdRangeNeighbour(FdRange(20, 29), FdRange(10, 19)));
    EXPECT_FALSE(rightFdRangeNeighbour(FdRange(20, 29), FdRange(10, 20)));
    EXPECT_FALSE(rightFdRangeNeighbour(FdRange(20, 29), FdRange(10, 25)));
    EXPECT_FALSE(rightFdRangeNeighbour(FdRange(20, 29), FdRange(20, 20)));
    EXPECT_FALSE(rightFdRangeNeighbour(FdRange(20, 29), FdRange(20, 25)));
    EXPECT_FALSE(rightFdRangeNeighbour(FdRange(20, 29), FdRange(25, 29)));
    EXPECT_FALSE(rightFdRangeNeighbour(FdRange(20, 29), FdRange(29, 29)));
    EXPECT_FALSE(rightFdRangeNeighbour(FdRange(20, 29), FdRange(25, 35)));
    EXPECT_TRUE(rightFdRangeNeighbour(FdRange(20, 29),  FdRange(30, 39)));
    EXPECT_FALSE(rightFdRangeNeighbour(FdRange(20, 29), FdRange(35, 39)));
}

TEST(FdTest, LeftNeighbour)
{
    EXPECT_FALSE(leftFdRangeNeighbour(FdRange(20, 29), FdRange(10, 15)));
    EXPECT_TRUE(leftFdRangeNeighbour(FdRange(20, 29),  FdRange(10, 19)));
    EXPECT_FALSE(leftFdRangeNeighbour(FdRange(20, 29), FdRange(10, 20)));
    EXPECT_FALSE(leftFdRangeNeighbour(FdRange(20, 29), FdRange(10, 25)));
    EXPECT_FALSE(leftFdRangeNeighbour(FdRange(20, 29), FdRange(20, 20)));
    EXPECT_FALSE(leftFdRangeNeighbour(FdRange(20, 29), FdRange(20, 25)));
    EXPECT_FALSE(leftFdRangeNeighbour(FdRange(20, 29), FdRange(25, 29)));
    EXPECT_FALSE(leftFdRangeNeighbour(FdRange(20, 29), FdRange(29, 29)));
    EXPECT_FALSE(leftFdRangeNeighbour(FdRange(20, 29), FdRange(25, 35)));
    EXPECT_FALSE(leftFdRangeNeighbour(FdRange(20, 29), FdRange(30, 39)));
}

TEST(FdTest, RightOf)
{
    EXPECT_FALSE(rightFdRangeOf(FdRange(20, 29), FdRange(10, 19)));
    EXPECT_FALSE(rightFdRangeOf(FdRange(20, 29), FdRange(10, 20)));
    EXPECT_FALSE(rightFdRangeOf(FdRange(20, 29), FdRange(10, 25)));
    EXPECT_FALSE(rightFdRangeOf(FdRange(20, 29), FdRange(20, 20)));
    EXPECT_FALSE(rightFdRangeOf(FdRange(20, 29), FdRange(20, 25)));
    EXPECT_FALSE(rightFdRangeOf(FdRange(20, 29), FdRange(25, 29)));
    EXPECT_FALSE(rightFdRangeOf(FdRange(20, 29), FdRange(29, 29)));
    EXPECT_FALSE(rightFdRangeOf(FdRange(20, 29), FdRange(25, 35)));
    EXPECT_TRUE(rightFdRangeOf(FdRange(20, 29),  FdRange(30, 39)));
    EXPECT_TRUE(rightFdRangeOf(FdRange(20, 29),  FdRange(35, 39)));
}

TEST(FdTest, LeftOf)
{
    EXPECT_TRUE(leftFdRangeOf(FdRange(20, 29),  FdRange(10, 15)));
    EXPECT_TRUE(leftFdRangeOf(FdRange(20, 29),  FdRange(10, 19)));
    EXPECT_FALSE(leftFdRangeOf(FdRange(20, 29), FdRange(10, 20)));
    EXPECT_FALSE(leftFdRangeOf(FdRange(20, 29), FdRange(10, 25)));
    EXPECT_FALSE(leftFdRangeOf(FdRange(20, 29), FdRange(20, 20)));
    EXPECT_FALSE(leftFdRangeOf(FdRange(20, 29), FdRange(20, 25)));
    EXPECT_FALSE(leftFdRangeOf(FdRange(20, 29), FdRange(25, 29)));
    EXPECT_FALSE(leftFdRangeOf(FdRange(20, 29), FdRange(29, 29)));
    EXPECT_FALSE(leftFdRangeOf(FdRange(20, 29), FdRange(25, 35)));
    EXPECT_FALSE(leftFdRangeOf(FdRange(20, 29), FdRange(30, 39)));
}

TEST(FdTest, CreateDestroy)
{
    struct FdSet  fdset_;
    struct FdSet *fdset = 0;

    EXPECT_EQ(0, createFdSet(&fdset_));
    fdset = &fdset_;

    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(0, 2)));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(1, 2)));
    EXPECT_EQ(EEXIST, errno);

    fdset = closeFdSet(fdset);
}

TEST(FdTest, Clear)
{
    struct FdSet  fdset_;
    struct FdSet *fdset = 0;

    EXPECT_EQ(0, createFdSet(&fdset_));
    fdset = &fdset_;

    struct rlimit fdLimit;
    EXPECT_EQ(0, getrlimit(RLIMIT_NOFILE, &fdLimit));

    int fdList[fdLimit.rlim_cur / 2 - 1];

    for (unsigned ix = 0; NUMBEROF(fdList) > ix; ++ix)
        fdList[ix] = 2 * ix;

    for (unsigned ix = 0; NUMBEROF(fdList) > ix; ++ix)
    {
        unsigned jx = random() % (NUMBEROF(fdList) - ix) + ix;

        SWAP(fdList[ix], fdList[jx]);
    }

    for (unsigned ix = 0; NUMBEROF(fdList) > ix; ++ix)
        EXPECT_EQ(0, insertFdSetRange(fdset,
                                      FdRange(fdList[ix], fdList[ix])));

    for (unsigned ix = 0; NUMBEROF(fdList) > ix; ++ix)
    {
        errno = 0;
        EXPECT_NE(0, insertFdSetRange(fdset,
                                      FdRange(fdList[ix], fdList[ix])));
        EXPECT_EQ(EEXIST, errno);
    }

    clearFdSet(fdset);

    for (unsigned ix = 0; NUMBEROF(fdList) > ix; ++ix)
        EXPECT_EQ(0, insertFdSetRange(fdset,
                                      FdRange(fdList[ix], fdList[ix])));

    fdset = closeFdSet(fdset);
}

TEST(FdTest, InvertEmpty)
{
    struct FdSet  fdset_;
    struct FdSet *fdset = 0;

    EXPECT_EQ(0, createFdSet(&fdset_));
    fdset = &fdset_;

    EXPECT_EQ(0, invertFdSet(fdset));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(0, 0)));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(INT_MAX, INT_MAX)));
    EXPECT_EQ(0, invertFdSet(fdset));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(0, 0)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(INT_MAX, INT_MAX)));

    fdset = closeFdSet(fdset);
}

TEST(FdTest, InvertSingle)
{
    struct FdSet  fdset_;
    struct FdSet *fdset = 0;

    EXPECT_EQ(0, createFdSet(&fdset_));
    fdset = &fdset_;

    /* Single left side fd */
    clearFdSet(fdset);
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(0, 0)));
    EXPECT_EQ(0, invertFdSet(fdset));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(1, 1)));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(INT_MAX, INT_MAX)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(0, 0)));

    /* Range left side fd */
    clearFdSet(fdset);
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(0, 1)));
    EXPECT_EQ(0, invertFdSet(fdset));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(2, 2)));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(INT_MAX, INT_MAX)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(0, 1)));

    /* Single right side fd */
    clearFdSet(fdset);
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(INT_MAX, INT_MAX)));
    EXPECT_EQ(0, invertFdSet(fdset));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(0, 0)));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(INT_MAX-1, INT_MAX-1)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(INT_MAX, INT_MAX)));

    /* Range right side fd */
    clearFdSet(fdset);
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(INT_MAX-1, INT_MAX)));
    EXPECT_EQ(0, invertFdSet(fdset));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(0, 0)));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(INT_MAX-2, INT_MAX-2)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(INT_MAX-1, INT_MAX)));

    /* Two and three ranges */
    clearFdSet(fdset);
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(0, 1)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(INT_MAX-1, INT_MAX)));
    EXPECT_EQ(0, invertFdSet(fdset));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(2, 2)));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(INT_MAX-2, INT_MAX-2)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(0, 0)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(INT_MAX, INT_MAX)));
    EXPECT_EQ(0, removeFdSetRange(fdset, FdRange(0, 0)));
    EXPECT_EQ(0, removeFdSetRange(fdset, FdRange(INT_MAX, INT_MAX)));

    EXPECT_EQ(0, invertFdSet(fdset));

    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(0, 0)));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(1, 1)));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(INT_MAX-1, INT_MAX-1)));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(INT_MAX, INT_MAX)));

    fdset = closeFdSet(fdset);
}

TEST(FdTest, InsertRemove)
{
    struct FdSet  fdset_;
    struct FdSet *fdset = 0;

    EXPECT_EQ(0, createFdSet(&fdset_));
    fdset = &fdset_;

    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(0, 2)));
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(0, 0)));
    EXPECT_EQ(0, removeFdSetRange(fdset, FdRange(0, 2)));
    EXPECT_NE(0, removeFdSetRange(fdset, FdRange(0, 0)));
    EXPECT_NE(0, removeFdSetRange(fdset, FdRange(0, 2)));
    EXPECT_EQ(ENOENT, errno);

    fdset = closeFdSet(fdset);
}

TEST(FdTest, InsertOverlap)
{
    struct FdSet  fdset_;
    struct FdSet *fdset = 0;

    EXPECT_EQ(0, createFdSet(&fdset_));
    fdset = &fdset_;

    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(0, 2)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(4, 6)));

    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(0, 1)));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(0, 2)));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(0, 3)));
    EXPECT_EQ(EEXIST, errno);

    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(0, 1)));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(0, 2)));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(0, 3)));
    EXPECT_EQ(EEXIST, errno);

    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(1, 2)));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(1, 3)));
    EXPECT_EQ(EEXIST, errno);

    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(3, 4)));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(3, 5)));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(3, 6)));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(3, 7)));
    EXPECT_EQ(EEXIST, errno);

    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(4, 4)));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(4, 5)));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(4, 6)));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(4, 7)));
    EXPECT_EQ(EEXIST, errno);

    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(5, 6)));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(5, 7)));
    EXPECT_EQ(EEXIST, errno);

    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(6, 6)));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(6, 7)));
    EXPECT_EQ(EEXIST, errno);

    fdset = closeFdSet(fdset);
}

TEST(FdTest, RemoveOverlap)
{
    struct FdSet  fdset_;
    struct FdSet *fdset = 0;

    EXPECT_EQ(0, createFdSet(&fdset_));
    fdset = &fdset_;

    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(0, 2)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(4, 6)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(8, 10)));

    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(0, 1)));
    EXPECT_EQ(0, removeFdSetRange(fdset, FdRange(0, 1)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(0, 1)));

    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(0, 2)));
    EXPECT_EQ(0, removeFdSetRange(fdset, FdRange(0, 2)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(0, 2)));

    EXPECT_NE(0, insertFdSetRange(fdset, FdRange(1, 2)));
    EXPECT_EQ(0, removeFdSetRange(fdset, FdRange(1, 2)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(1, 2)));

    EXPECT_NE(0, removeFdSetRange(fdset, FdRange(3, 3)));
    EXPECT_NE(0, removeFdSetRange(fdset, FdRange(3, 4)));
    EXPECT_NE(0, removeFdSetRange(fdset, FdRange(3, 5)));
    EXPECT_NE(0, removeFdSetRange(fdset, FdRange(3, 6)));
    EXPECT_NE(0, removeFdSetRange(fdset, FdRange(3, 7)));

    EXPECT_EQ(0, removeFdSetRange(fdset, FdRange(4, 4)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(4, 4)));

    EXPECT_EQ(0, removeFdSetRange(fdset, FdRange(4, 5)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(4, 5)));

    EXPECT_EQ(0, removeFdSetRange(fdset, FdRange(5, 6)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(5, 6)));

    EXPECT_EQ(0, removeFdSetRange(fdset, FdRange(6, 6)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(6, 6)));

    EXPECT_EQ(0, removeFdSetRange(fdset, FdRange(8, 8)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(8, 8)));

    EXPECT_EQ(0, removeFdSetRange(fdset, FdRange(8, 9)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(8, 9)));

    EXPECT_EQ(0, removeFdSetRange(fdset, FdRange(9, 10)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(9, 10)));

    EXPECT_EQ(0, removeFdSetRange(fdset, FdRange(10, 10)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(10, 10)));

    fdset = closeFdSet(fdset);
}

struct TestVisitor
{
    int mNext;
    int mStop;

    static int
    visit(struct TestVisitor *self, struct FdRange aRange)
    {
        EXPECT_EQ(aRange.mLhs, aRange.mRhs);
        EXPECT_EQ(self->mNext, aRange.mLhs);

        if (aRange.mLhs == self->mStop)
            return 1;

        self->mNext += 2;

        return 0;
    }
};

TEST(FdTest, Visitor)
{
    struct FdSet  fdset_;
    struct FdSet *fdset = 0;

    EXPECT_EQ(0, createFdSet(&fdset_));
    fdset = &fdset_;

    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(0, 0)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(2, 2)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(4, 4)));

    struct TestVisitor testVisitor;

    testVisitor.mNext = 0;
    testVisitor.mStop = -1;

    EXPECT_EQ(
        3,
        visitFdSet(fdset, FdSetVisitor(&testVisitor, TestVisitor::visit)));

    EXPECT_EQ(6, testVisitor.mNext);

    testVisitor.mNext = 0;
    testVisitor.mStop = 2;

    EXPECT_EQ(
        2,
        visitFdSet(fdset, FdSetVisitor(&testVisitor, TestVisitor::visit)));

    EXPECT_EQ(2, testVisitor.mNext);

    fdset = closeFdSet(fdset);
}

#include "../googletest/src/gtest_main.cc"
