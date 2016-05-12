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

#include "file_.h"
#include "process_.h"

#include "gtest/gtest.h"

TEST(FileTest, TemporaryFile)
{
    struct File file;

    ASSERT_EQ(0, temporaryFile(&file));

    EXPECT_EQ(1, writeFile(&file, "A", 1));

    EXPECT_EQ(0, lseekFile(&file, 0, WhenceTypeStart));

    char buf[1];
    EXPECT_EQ(1, readFile(&file, buf, 1));

    EXPECT_EQ('A', buf[0]);

    closeFile(&file);
}

struct LockType
checkLock(struct File *aFile)
{
    struct Pid checkPid =
        forkProcessChild(ForkProcessShareProcessGroup,
                         Pgid(0),
                         ForkProcessMethodNil());

    if (-1 == checkPid.mPid)
        return LockTypeError;

    if ( ! checkPid.mPid)
    {
        struct LockType lockType = ownFileRegionLocked(aFile, 0, 1);

        const char *cmd = "exit 0";

        if (LockTypeUnlocked.mType == lockType.mType)
            cmd = "exit 1";
        else if (LockTypeRead.mType == lockType.mType)
            cmd = "exit 2";
        else if (LockTypeWrite.mType == lockType.mType)
            cmd = "exit 3";

        execl("/bin/sh", "sh", "-c", cmd, (char *) 0);
        _exit(EXIT_SUCCESS);
    }

    int status;
    if (reapProcessChild(checkPid, &status))
        return LockTypeError;

    switch (extractProcessExitStatus(status, checkPid).mStatus)
    {
    default:
        return LockTypeError;
    case 1:
        return LockTypeUnlocked;
    case 2:
        return LockTypeRead;
    case 3:
        return LockTypeWrite;
    }
}

TEST(FileTest, LockFileRegion)
{
    struct File file;

    ASSERT_EQ(0, temporaryFile(&file));

    // If a process holds a region lock, querying the lock state from
    // that process will always show the region as unlocked, but another
    // process will see the region as locked.

    EXPECT_EQ(
        LockTypeUnlocked.mType, ownFileRegionLocked(&file, 0, 0).mType);
    EXPECT_EQ(
        LockTypeUnlocked.mType, checkLock(&file).mType);

    {
        EXPECT_EQ(0, lockFileRegion(&file, LockTypeWrite, 0, 0));

        EXPECT_EQ(
            LockTypeUnlocked.mType, ownFileRegionLocked(&file, 0, 0).mType);
        EXPECT_EQ(
            LockTypeWrite.mType, checkLock(&file).mType);

        EXPECT_EQ(0, unlockFileRegion(&file, 0, 0));

        EXPECT_EQ(
            LockTypeUnlocked.mType, ownFileRegionLocked(&file, 0, 0).mType);
        EXPECT_EQ(
            LockTypeUnlocked.mType, checkLock(&file).mType);
    }

    {
        EXPECT_EQ(0, lockFileRegion(&file, LockTypeRead, 0, 0));

        EXPECT_EQ(
            LockTypeUnlocked.mType, ownFileRegionLocked(&file, 0, 0).mType);
        EXPECT_EQ(
            LockTypeRead.mType, checkLock(&file).mType);

        EXPECT_EQ(0, unlockFileRegion(&file, 0, 0));

        EXPECT_EQ(
            LockTypeUnlocked.mType, ownFileRegionLocked(&file, 0, 0).mType);
        EXPECT_EQ(
            LockTypeUnlocked.mType, checkLock(&file).mType);
    }

    {
        EXPECT_EQ(0, lockFileRegion(&file, LockTypeWrite, 0, 0));

        EXPECT_EQ(
            LockTypeUnlocked.mType, ownFileRegionLocked(&file, 0, 0).mType);
        EXPECT_EQ(
            LockTypeWrite.mType, checkLock(&file).mType);

        EXPECT_EQ(0, lockFileRegion(&file, LockTypeRead, 0, 0));

        EXPECT_EQ(
            LockTypeUnlocked.mType, ownFileRegionLocked(&file, 0, 0).mType);
        EXPECT_EQ(
            LockTypeRead.mType, checkLock(&file).mType);

        EXPECT_EQ(0, lockFileRegion(&file, LockTypeWrite, 0, 0));

        EXPECT_EQ(
            LockTypeUnlocked.mType, ownFileRegionLocked(&file, 0, 0).mType);
        EXPECT_EQ(
            LockTypeWrite.mType, checkLock(&file).mType);

        EXPECT_EQ(0, unlockFileRegion(&file, 0, 0));

        EXPECT_EQ(
            LockTypeUnlocked.mType, ownFileRegionLocked(&file, 0, 0).mType);
        EXPECT_EQ(
            LockTypeUnlocked.mType, checkLock(&file).mType);
    }

    closeFile(&file);
}

#include "../googletest/src/gtest_main.cc"
