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
#include "fd.h"

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/resource.h>

/* -------------------------------------------------------------------------- */
static struct File sFileList =
{
    .mFd   = -1,
    .mNext = &sFileList,
    .mPrev = &sFileList,
};

/* -------------------------------------------------------------------------- */
int
createFile(struct File *self, int aFd)
{
    ensure(self != &sFileList);

    int rc = -1;

    self->mFd = aFd;

    /* If the file descriptor is invalid, take care to have preserved
     * errno so that the caller can rely on interpreting errno to
     * discover why the file descriptor is invalid. */

    if (-1 == aFd)
        goto Finally;

    self->mNext =  sFileList.mNext;
    self->mPrev = &sFileList;

    self->mNext->mPrev = self;
    self->mPrev->mNext = self;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc && -1 != self->mFd)
            closeFd(&self->mFd);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closeFile(struct File *self)
{
    ensure(self != &sFileList);

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
cleanseFiles(void)
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

    for (const struct File *fdPtr = &sFileList; ; ++numFds)
    {
        fdPtr = fdPtr->mNext;
        if (fdPtr == &sFileList)
            break;
    }

    /* Create the whitelist of file descriptors by copying the fds
     * from each of the explicitly created file descriptors. */

    int whiteList[numFds + 1];

    struct rlimit noFile;

    if (getrlimit(RLIMIT_NOFILE, &noFile))
        goto Finally;

    ensure(numFds < noFile.rlim_cur);

    whiteList[numFds] = noFile.rlim_cur;

    {
        unsigned ix = 0;

        for (unsigned jx = 0; NUMBEROF(stdfds) > jx; ++jx, ++ix)
            whiteList[ix] = stdfds[jx];

        for (const struct File *fdPtr = &sFileList;
             numFds > ix;
             ++ix)
        {
            fdPtr = fdPtr->mNext;
            ensure(fdPtr != &sFileList);

            whiteList[ix] = fdPtr->mFd;

            ensure(whiteList[ix] < whiteList[numFds]);
        }
    }

    /* Walk the file descriptor space and close all the file descriptors,
     * skipping those mentioned in the whitelist. */

    debug(0, "purging %d fds", whiteList[numFds]);

    qsort(whiteList, NUMBEROF(whiteList), sizeof(whiteList[0]), rankFd_);

    for (int fd = 0, wx = 0; ; ++fd)
    {
        while (0 > whiteList[wx])
            ++wx;

        if (fd != whiteList[wx])
        {
            int closedFd = fd;

            if (closeFd(&closedFd) && EBADF != errno)
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
dupFile(struct File *self, const struct File *aOther)
{
    int rc = -1;

    if (createFile(self, dup(aOther->mFd)))
        goto Finally;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            closeFile(self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closeFilePair(struct File **aFile1, struct File **aFile2)
{
    int rc = -1;

    if (*aFile1)
    {
        if (closeFile(*aFile1))
            goto Finally;
        *aFile1 = 0;
    }

    if (*aFile2)
    {
        if (closeFile(*aFile2))
            goto Finally;
        *aFile2 = 0;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
nonblockingFile(struct File *self)
{
    return nonblockingFd(self->mFd);
}

/* -------------------------------------------------------------------------- */
int
closeFileOnExec(struct File *self, unsigned aCloseOnExec)
{
    return closeFdOnExec(self->mFd, aCloseOnExec);
}

/* -------------------------------------------------------------------------- */
int
lockFile(struct File *self, int aType, const struct FileLockTimeout *aTimeout)
{
    return lockFd(self->mFd, aType, aTimeout ? aTimeout->mMilliSeconds : 30000);
}

/* -------------------------------------------------------------------------- */
int
unlockFile(struct File *self)
{
    return unlockFd(self->mFd);
}

/* -------------------------------------------------------------------------- */
ssize_t
writeFile(struct File *self, const char *aBuf, size_t aLen)
{
    return writeFd(self->mFd, aBuf, aLen);
}

/* -------------------------------------------------------------------------- */
ssize_t
readFile(struct File *self, char *aBuf, size_t aLen)
{
    return readFd(self->mFd, aBuf, aLen);
}

/* -------------------------------------------------------------------------- */
int
fstatFile(struct File *self, struct stat *aStat)
{
    return fstat(self->mFd, aStat);
}

/* -------------------------------------------------------------------------- */
int
fcntlFileGetFlags(struct File *self)
{
    return fcntl(self->mFd, F_GETFL);
}

/* -------------------------------------------------------------------------- */
int
ftruncateFile(struct File *self, off_t aLength)
{
    return ftruncate(self->mFd, aLength);
}

/* -------------------------------------------------------------------------- */
