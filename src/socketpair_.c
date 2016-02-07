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

#include "socketpair_.h"
#include "fd_.h"
#include "macros_.h"
#include "error_.h"

#include <errno.h>

/* -------------------------------------------------------------------------- */
int
createSocketPair(struct SocketPair *self)
{
    int rc = -1;

    self->mParentFile = 0;
    self->mChildFile  = 0;

    int fd[2];

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fd))
        goto Finally;

    if (-1 == fd[0] || -1 == fd[1])
        goto Finally;

    ensure( ! stdFd(fd[0]));
    ensure( ! stdFd(fd[1]));

    if (createFile(&self->mParentFile_, fd[0]))
        goto Finally;
    self->mParentFile = &self->mParentFile_;

    fd[0] = -1;

    if (createFile(&self->mChildFile_, fd[1]))
        goto Finally;
    self->mChildFile = &self->mChildFile_;

    fd[1] = -1;

    rc = 0;

Finally:

    FINALLY
    ({
        closeFd(&fd[0]);
        closeFd(&fd[1]);

        if (rc)
        {
            closeFile(self->mParentFile);
            closeFile(self->mChildFile);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closeSocketPairParent(struct SocketPair *self)
{
    int rc = -1;

    if (closeFile(self->mParentFile))
        goto Finally;
    self->mParentFile = 0;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closeSocketPairChild(struct SocketPair *self)
{
    int rc = -1;

    if (closeFile(self->mChildFile))
        goto Finally;
    self->mChildFile = 0;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
closeSocketPair(struct SocketPair *self)
{
    int rc = -1;

    if (self)
    {
        if (closeFilePair(&self->mParentFile, &self->mChildFile))
            goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
