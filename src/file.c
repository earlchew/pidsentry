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

#include "file.h"
#include "macros.h"
#include "error.h"

#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/resource.h>

/* -------------------------------------------------------------------------- */
static struct FileDescriptor sFileDescriptorList =
{
    .mFd   = -1,
    .mNext = &sFileDescriptorList,
    .mPrev = &sFileDescriptorList,
};

/* -------------------------------------------------------------------------- */
int
createFileDescriptor(struct FileDescriptor *self, int aFd)
{
    assert(self != &sFileDescriptorList);

    int rc = -1;

    self->mFd = aFd;

    /* If the file descriptor is invalid, take care to have preserved
     * errno so that the caller can rely on interpreting errno to
     * discover why the file descriptor is invalid. */

    if (-1 == aFd)
        goto Finally;

    self->mNext =  sFileDescriptorList.mNext;
    self->mPrev = &sFileDescriptorList;

    self->mNext->mPrev = self;
    self->mPrev->mNext = self;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc && -1 != self->mFd)
            close(self->mFd);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closeFileDescriptor(struct FileDescriptor *self)
{
    assert(self != &sFileDescriptorList);

    int rc = -1;

    if (self)
    {
        if (-1 == self->mFd)
            goto Finally;

        if (close(self->mFd))
            goto Finally;

        self->mPrev->mNext = self->mNext;
        self->mNext->mPrev = self->mPrev;

        self->mPrev = 0;
        self->mNext = 0;
        self->mFd   = -1;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
/* Cleanse process of file descriptors
 *
 * Remove all the file descriptors that were not created explicitly by the
 * process itself, with the exclusion of stdin, stdout and stderr.
 */

static int
rankFd_(const void *aLhs, const void *aRhs)
{
    int lhs = * (const int *) aLhs;
    int rhs = * (const int *) aRhs;

    if (lhs < rhs) return -1;
    if (lhs > rhs) return +1;
    return 0;
}

int
cleanseFileDescriptors(void)
{
    int rc = -1;

    /* Count the number of file descriptors explicitly created by the
     * process itself in order to correctly size the array to whitelist
     * the explicitly created file dsscriptors. Include stdin, stdout
     * and stderr in the whitelist by default.
     *
     * Note that stdin, stdout and stderr might in fact already be
     * represented in the file descriptor list, so the resulting
     * algorithms must be capable of handing duplicates. Force that
     * scenario to be covered by explicitly repeating each of them here. */

    int stdfds[] =
    {
        STDIN_FILENO,  STDIN_FILENO,
        STDOUT_FILENO, STDOUT_FILENO,
        STDERR_FILENO, STDERR_FILENO,
    };

    unsigned numFds = NUMBEROF(stdfds);

    for (const struct FileDescriptor *fdPtr = &sFileDescriptorList; ; ++numFds)
    {
        fdPtr = fdPtr->mNext;
        if (fdPtr == &sFileDescriptorList)
            break;
    }

    /* Create the whitelist of file descriptors by copying the fds
     * from each of the explicitly created file descriptors. */

    int whiteList[numFds + 1];

    struct rlimit noFile;

    if (getrlimit(RLIMIT_NOFILE, &noFile))
        goto Finally;

    assert(numFds < noFile.rlim_cur);

    whiteList[numFds] = noFile.rlim_cur;

    {
        unsigned ix = 0;

        for (unsigned jx = 0; NUMBEROF(stdfds) > jx; ++jx, ++ix)
            whiteList[ix] = stdfds[jx];

        for (const struct FileDescriptor *fdPtr = &sFileDescriptorList;
             numFds > ix;
             ++ix)
        {
            fdPtr = fdPtr->mNext;
            assert(fdPtr != &sFileDescriptorList);

            whiteList[ix] = fdPtr->mFd;

            assert(whiteList[ix] < whiteList[numFds]);
        }
    }

    /* Walk the file descriptor space and close all the file descriptors,
     * skipping those mentioned in the whitelist. */

    debug(0, "purging %d fds", whiteList[numFds]);
    for (unsigned ix = 0; ix < NUMBEROF(whiteList); ++ix)
        debug(0, "whitelist %u %d", ix, whiteList[ix]);

    qsort(whiteList, NUMBEROF(whiteList), sizeof(whiteList[0]), rankFd_);

    for (unsigned fd = 0, wx = 0; ; ++fd)
    {
        while (0 > whiteList[wx])
            ++wx;

        if (fd != whiteList[wx])
        {
            if (close(fd) && EBADF != errno)
                goto Finally;
        }
        else
        {
            debug(0, "not closing fd %d", fd);

            if (NUMBEROF(whiteList) == ++wx)
                break;

            while (whiteList[wx] == fd)
                ++wx;

        }
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
dupFileDescriptor(
        struct FileDescriptor *self,
        const struct FileDescriptor *aOther)
{
    int rc = -1;

    if (createFileDescriptor(self, dup(aOther->mFd)))
        goto Finally;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            closeFileDescriptor(self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closeFileDescriptorPair(struct FileDescriptor **aFile1,
                        struct FileDescriptor **aFile2)
{
    int rc = -1;

    if (closeFileDescriptor(*aFile1))
        goto Finally;
    *aFile1 = 0;

    if (closeFileDescriptor(*aFile2))
        goto Finally;
    *aFile2 = 0;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
nonblockingFileDescriptor(struct FileDescriptor *self)
{
    long flags = fcntl(self->mFd, F_GETFL, 0);

    return -1 == flags ? -1 : fcntl(self->mFd, F_SETFL, flags | O_NONBLOCK);
}

/* -------------------------------------------------------------------------- */
