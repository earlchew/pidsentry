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

#include "pathname.h"
#include "macros.h"

#include <string.h>
#include <stdlib.h>
#include <libgen.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
int
createPathName(struct PathName *self, const char *aFileName)
{
    int rc = -1;

    self->mFileName  = 0;
    self->mBaseName_ = 0;
    self->mBaseName  = 0;
    self->mDirName_  = 0;
    self->mDirName   = 0;
    self->mDirFile   = 0;

    self->mFileName = strdup(aFileName);
    if ( ! self->mFileName)
        goto Finally;

    self->mDirName_ = strdup(self->mFileName);
    if ( ! self->mDirName_)
        goto Finally;

    self->mBaseName_ = strdup(self->mFileName);
    if ( ! self->mBaseName_)
        goto Finally;

    self->mDirName  = strdup(dirname(self->mDirName_));
    if ( ! self->mDirName)
        goto Finally;

    self->mBaseName = strdup(basename(self->mBaseName_));
    if ( ! self->mBaseName)
        goto Finally;

    if (createFileDescriptor(
            &self->mDirFile_,
            open(self->mDirName, O_RDONLY | O_CLOEXEC)))
        goto Finally;
    self->mDirFile = &self->mDirFile_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            free(self->mFileName);
            free(self->mBaseName_);
            free(self->mBaseName);
            free(self->mDirName_);
            free(self->mDirName);

            closeFileDescriptor(self->mDirFile);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closePathName(struct PathName *self)
{
    int rc = -1;

    if (self)
    {
        if (closeFileDescriptor(self->mDirFile))
            goto Finally;

        free(self->mFileName);
        free(self->mBaseName_);
        free(self->mBaseName);
        free(self->mDirName_);
        free(self->mDirName);

        self->mFileName  = 0;
        self->mBaseName_ = 0;
        self->mBaseName  = 0;
        self->mDirName_  = 0;
        self->mDirName   = 0;
        self->mDirFile   = 0;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
openPathName(struct PathName *self, int aFlags, mode_t aMode)
{
    return openat(self->mDirFile->mFd, self->mBaseName, aFlags, aMode);
}

/* -------------------------------------------------------------------------- */
int
unlinkPathName(struct PathName *self, int aFlags)
{
    return unlinkat(self->mDirFile->mFd, self->mBaseName, aFlags);
}

/* -------------------------------------------------------------------------- */
int
fstatPathName(const struct PathName *self, struct stat *aStat, int aFlags)
{
    return fstatat(self->mDirFile->mFd, self->mBaseName, aStat, aFlags);
}

/* -------------------------------------------------------------------------- */
