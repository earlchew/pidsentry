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

#include "error_.h"

#include <unistd.h>

#undef terminate
#undef warn
#undef debug
#undef breadcrumb

#include "gtest/gtest.h"

TEST(ErrorTest, ErrnoText)
{
    Error_warn_(EPERM, "Test EPERM");
    Error_warn_(0,     "Test errno 0");
    Error_warn_(-1,    "Test errno -1");
}

static int
ok()
{
    return 0;
}

static int
fail()
{
    return -1;
}

static int
testFinallyIfOk()
{
    int rc = -1;

    FINALLY_IF(ok(), errno = 0);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static int
testFinallyIfFail_0()
{
    errno = 0;
    return -1;
}

static int
testFinallyIfFail_1()
{
    int rc = -1;

    FINALLY_IF(fail(), errno = -1);

    rc = 0;

Finally:

    FINALLY
    ({
        testFinallyIfFail_0();
    });

    return rc;
}

static int
testFinallyIfFail_2()
{
    int rc = -1;

    FINALLY_IF(testFinallyIfFail_1(), errno = -2);

    rc = 0;

Finally:

    FINALLY
    ({
        testFinallyIfFail_1();
    });

    return rc;
}

TEST(ErrorTest, FinallyIf)
{
    int errCode;
    int sigErrCode;

    EXPECT_EQ(0, testFinallyIfOk());
    EXPECT_EQ(0u, ownErrorFrameLevel());
    resetErrorFrameLevel();

    EXPECT_EQ(-1, testFinallyIfFail_1());
    errCode = errno;
    EXPECT_EQ(1u, ownErrorFrameLevel());
    EXPECT_EQ(-1, ownErrorFrame(ErrorFrameStackThread, 0)->mErrno);
    EXPECT_EQ(0,  ownErrorFrame(ErrorFrameStackThread, 1));
    logErrorFrameWarning();
    Error_warn_(errCode, "One level error frame test");
    resetErrorFrameLevel();

    EXPECT_EQ(-1, testFinallyIfFail_2());
    errCode = errno;
    EXPECT_EQ(2u, ownErrorFrameLevel());
    EXPECT_EQ(-1, ownErrorFrame(ErrorFrameStackThread, 0)->mErrno);
    EXPECT_EQ(-2, ownErrorFrame(ErrorFrameStackThread, 1)->mErrno);
    EXPECT_EQ(0,  ownErrorFrame(ErrorFrameStackThread, 2));
    logErrorFrameWarning();
    Error_warn_(errCode, "Two level error frame test");
    resetErrorFrameLevel();

    EXPECT_EQ(-1, testFinallyIfFail_2());
    errCode = errno;

    enum ErrorFrameStackKind stackKind =
        switchErrorFrameStack(ErrorFrameStackSignal);
    EXPECT_EQ(ErrorFrameStackThread, stackKind);

    EXPECT_EQ(-1, testFinallyIfFail_1());
    sigErrCode = errno;
    EXPECT_EQ(1u, ownErrorFrameLevel());
    EXPECT_EQ(-1, ownErrorFrame(ErrorFrameStackThread, 0)->mErrno);
    EXPECT_EQ(0,  ownErrorFrame(ErrorFrameStackThread, 1));
    logErrorFrameWarning();
    Error_warn_(sigErrCode, "Signal stack one level error frame test");
    resetErrorFrameLevel();

    stackKind = switchErrorFrameStack(stackKind);
    EXPECT_EQ(ErrorFrameStackSignal, stackKind);

    EXPECT_EQ(2u, ownErrorFrameLevel());
    EXPECT_EQ(-1, ownErrorFrame(ErrorFrameStackThread, 0)->mErrno);
    EXPECT_EQ(-2, ownErrorFrame(ErrorFrameStackThread, 1)->mErrno);
    EXPECT_EQ(0,  ownErrorFrame(ErrorFrameStackThread, 2));
    logErrorFrameWarning();
    Error_warn_(errCode, "Two level error frame test");
    resetErrorFrameLevel();
}

#include "../googletest/src/gtest_main.cc"
