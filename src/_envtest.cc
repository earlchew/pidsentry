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

#include "env_.h"

#include <stdlib.h>
#include <string.h>

#include "gtest/gtest.h"

TEST(EnvTest, String)
{
    unsetenv("NIL");

    {
        const char *value;

        EXPECT_EQ(-1, getEnvString("NIL", &value));
    }

    {
        EXPECT_EQ(0, strcmp("abc", setEnvString("VALUE", "abc")));

        const char *value;

        EXPECT_EQ(0,  getEnvString("VALUE", &value));
        EXPECT_EQ(0,  strcmp("abc", value));
    }
}

TEST(EnvTest, Int)
{
    unsetenv("NIL");

    {
        int value;

        EXPECT_EQ(-1, getEnvInt("NIL", &value));
    }

    {
        setenv("EMPTY0", "",  1);
        setenv("EMPTY1", " ", 1);

        int value;

        EXPECT_EQ(-1, getEnvInt("EMPTY0", &value));
        EXPECT_EQ(-1, getEnvInt("EMPTY1", &value));
    }

    {
        EXPECT_EQ(0, strcmp("0", setEnvInt("VALUE", 0)));

        int value;

        EXPECT_EQ(0,  getEnvInt("VALUE", &value));
        EXPECT_EQ(0,  value);
    }

    {
        EXPECT_EQ(0, strcmp("-1", setEnvInt("VALUE", -1)));

        int value;

        EXPECT_EQ(0,  getEnvInt("VALUE", &value));
        EXPECT_EQ(-1, value);
    }

    {
        EXPECT_EQ(0, strcmp("1", setEnvInt("VALUE", 1)));

        int value;

        EXPECT_EQ(0, getEnvInt("VALUE", &value));
        EXPECT_EQ(1, value);
    }
}

#include "../googletest/src/gtest_main.cc"
