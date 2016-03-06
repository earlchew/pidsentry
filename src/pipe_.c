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

#include "pipe_.h"
#include "fd_.h"
#include "error_.h"
#include "macros_.h"

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

/* -------------------------------------------------------------------------- */
int
createPipe(struct Pipe *self, unsigned aFlags)
{
    int rc = -1;

    self->mRdFile = 0;
    self->mWrFile = 0;

    ERROR_IF(
        aFlags & ~ (O_CLOEXEC | O_NONBLOCK),
        {
            errno = EINVAL;
        });

    int fd[2];

    ERROR_IF(
        pipe2(fd, aFlags));

    ERROR_IF(
        -1 == fd[0] || -1 == fd[1],
        {
            errno = EINVAL;
        });

    ensure( ! stdFd(fd[0]));
    ensure( ! stdFd(fd[1]));

    ERROR_IF(
        createFile(&self->mRdFile_, fd[0]));
    self->mRdFile = &self->mRdFile_;

    fd[0] = -1;

    ERROR_IF(
        createFile(&self->mWrFile_, fd[1]));
    self->mWrFile = &self->mWrFile_;

    fd[1] = -1;

    rc = 0;

Finally:

    FINALLY
    ({
        closeFd(&fd[0]);
        closeFd(&fd[1]);

        if (rc)
        {
            closeFile(self->mRdFile);
            closeFile(self->mWrFile);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
detachPipeReader(struct Pipe *self)
{
    int rc = -1;

    ERROR_IF(
        detachFile(self->mRdFile));

    self->mRdFile = 0;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
detachPipeWriter(struct Pipe *self)
{
    int rc = -1;

    ERROR_IF(
        detachFile(self->mWrFile));

    self->mWrFile = 0;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closePipeReader(struct Pipe *self)
{
    closeFile(self->mRdFile);
    self->mRdFile = 0;
}

/* -------------------------------------------------------------------------- */
void
closePipeWriter(struct Pipe *self)
{
    closeFile(self->mWrFile);
    self->mWrFile = 0;
}

/* -------------------------------------------------------------------------- */
int
closePipeOnExec(struct Pipe *self, unsigned aCloseOnExec)
{
    int rc = -1;

    ERROR_IF(
        closeFileOnExec(self->mRdFile, aCloseOnExec));
    ERROR_IF(
        closeFileOnExec(self->mWrFile, aCloseOnExec));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
nonblockingPipe(struct Pipe *self)
{
    int rc = -1;

    ERROR_IF(
        nonblockingFile(self->mRdFile));
    ERROR_IF(
        nonblockingFile(self->mWrFile));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closePipe(struct Pipe *self)
{
    if (self)
        closeFilePair(&self->mRdFile, &self->mWrFile);
}

/* -------------------------------------------------------------------------- */
