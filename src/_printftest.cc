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

#include "printf_.h"

#include "gtest/gtest.h"

class PrintfTest : public ::testing::Test
{
    void SetUp()
    {
        ASSERT_EQ(0, Printf_init(&mModule));
    }

    void TearDown()
    {
        Printf_exit(&mModule);
    }

private:

    struct PrintfModule mModule;
};

class TestClass
{
private:

    int print_(FILE *aFile) const
    {
        return fprintf(aFile, "Test");
    }

public:

    static int print(const void *self_, FILE *aFile)
    {
        const TestClass *self = reinterpret_cast<const TestClass *>(self_);

        return self->print_(aFile);
    }
};

TEST_F(PrintfTest, PrintfMethod)
{
    TestClass test;

    char  *bufPtr = 0;
    size_t bufLen = 0;

    FILE *fp = open_memstream(&bufPtr, &bufLen);

    EXPECT_TRUE(fp);

    rewind(fp);
    xfprintf(
        fp,
        "%" PRIs_Method,
        FMTs_Method(&test, TestClass::print));

    EXPECT_EQ(0, fflush(fp));
    EXPECT_NE(0u, bufLen);
    EXPECT_EQ(std::string("Test"), bufPtr);

    rewind(fp);
    xfprintf(
        fp,
        "-%" PRIs_Method "-",
        FMTs_Method(&test, TestClass::print));

    EXPECT_EQ(0, fflush(fp));
    EXPECT_NE(0u, bufLen);
    EXPECT_EQ(std::string("-Test-"), bufPtr);

    rewind(fp);
    xfprintf(
        fp,
        "%" PRIs_Method "%" PRIs_Method,
        FMTs_Method(&test, TestClass::print),
        FMTs_Method(&test, TestClass::print));

    EXPECT_EQ(0, fflush(fp));
    EXPECT_NE(0u, bufLen);
    EXPECT_EQ(std::string("TestTest"), bufPtr);

    rewind(fp);
    xfprintf(
        fp,
        "%" PRIs_Method "-%" PRIs_Method,
        FMTs_Method(&test, TestClass::print),
        FMTs_Method(&test, TestClass::print));

    EXPECT_EQ(0, fflush(fp));
    EXPECT_NE(0u, bufLen);
    EXPECT_EQ(std::string("Test-Test"), bufPtr);

    EXPECT_EQ(0, fclose(fp));

    free(bufPtr);
}

#include "../googletest/src/gtest_main.cc"
