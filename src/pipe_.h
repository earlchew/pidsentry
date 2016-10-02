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
#ifndef PIPE_H
#define PIPE_H

#include "compiler_.h"
#include "file_.h"

BEGIN_C_SCOPE;

struct Pipe
{
    struct File  mRdFile_;
    struct File *mRdFile;
    struct File  mWrFile_;
    struct File *mWrFile;
};

/* -------------------------------------------------------------------------- */
CHECKED int
createPipe(struct Pipe *self, unsigned aFlags);

CHECKED struct Pipe *
closePipe(struct Pipe *self);

CHECKED int
detachPipeReader(struct Pipe *self);

CHECKED int
detachPipeWriter(struct Pipe *self);

void
closePipeReader(struct Pipe *self);

void
closePipeWriter(struct Pipe *self);

CHECKED int
closePipeOnExec(struct Pipe *self, unsigned aCloseOnExec);

CHECKED int
nonBlockingPipe(struct Pipe *self, unsigned aNonBlocking);

/* -------------------------------------------------------------------------- */

END_C_SCOPE;

#endif /* PIPE_H */
