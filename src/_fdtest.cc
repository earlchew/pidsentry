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

#include "fd_.h"
#include "pipe_.h"

#include "gtest/gtest.h"

TEST(FdTest, ReadFully)
{
    {
        char *buf = 0;

        EXPECT_EQ(-1, readFdFully(-1, &buf, 0));
        EXPECT_EQ(0,  buf);
    }

    {
        char  buf_;
        char *buf = &buf_;

        struct Pipe  pipe_;
        struct Pipe *pipe = 0;

        EXPECT_EQ(0, createPipe(&pipe_, 0));
        pipe = &pipe_;

        closePipeWriter(pipe);

        EXPECT_EQ(0, readFdFully(pipe->mRdFile->mFd, &buf, 0));
        EXPECT_EQ(0, buf);

        pipe = closePipe(pipe);
    }

    {
        char  buf_;
        char *buf = &buf_;

        struct Pipe  pipe_;
        struct Pipe *pipe = 0;

        EXPECT_EQ(0, createPipe(&pipe_, 0));
        pipe = &pipe_;

        EXPECT_EQ(1, writeFd(pipe->mWrFile->mFd, "1", 1, 0));
        closePipeWriter(pipe);

        EXPECT_EQ(1, readFdFully(pipe->mRdFile->mFd, &buf, 0));
        EXPECT_EQ(0, strncmp("1", buf, 1));

        free(buf);

        pipe = closePipe(pipe);
    }

    {
        char  buf_;
        char *buf = &buf_;

        struct Pipe  pipe_;
        struct Pipe *pipe = 0;

        EXPECT_EQ(0, createPipe(&pipe_, 0));
        pipe = &pipe_;

        EXPECT_EQ(4, writeFd(pipe->mWrFile->mFd, "1234", 4, 0));
        closePipeWriter(pipe);

        EXPECT_EQ(4, readFdFully(pipe->mRdFile->mFd, &buf, 0));
        EXPECT_EQ(0, strncmp("1234", buf, 4));

        free(buf);

        pipe = closePipe(pipe);
    }

    {
        char  buf_;
        char *buf = &buf_;

        struct Pipe  pipe_;
        struct Pipe *pipe = 0;

        EXPECT_EQ(0, createPipe(&pipe_, 0));
        pipe = &pipe_;

        EXPECT_EQ(5, writeFd(pipe->mWrFile->mFd, "12345", 5, 0));
        closePipeWriter(pipe);

        EXPECT_EQ(5, readFdFully(pipe->mRdFile->mFd, &buf, 0));
        EXPECT_EQ(0, strncmp("12345", buf, 5));

        free(buf);

        pipe = closePipe(pipe);
    }
}

#include "../googletest/src/gtest_main.cc"
