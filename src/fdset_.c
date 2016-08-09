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

#include "fdset_.h"
#include "error_.h"

#include <stdlib.h>

/* -------------------------------------------------------------------------- */
static int
rankFdSetElement_(struct FdSetElement_ *aLhs, struct FdSetElement_ *aRhs)
{
    int rank =
        ( (aLhs->mFirstFd > aRhs->mFirstFd) -
          (aLhs->mFirstFd < aRhs->mFirstFd) );

    if ( ! rank)
        rank =
            ( (aLhs->mLastFd > aRhs->mLastFd) -
              (aLhs->mLastFd < aRhs->mLastFd) );

    return rank;
}

RB_GENERATE_STATIC(FdSetTree_, FdSetElement_, mTree, rankFdSetElement_)

/* -------------------------------------------------------------------------- */
struct FdSet *
closeFdSet(struct FdSet *self)
{
    if (self)
    {
        while ( ! RB_EMPTY(&self->mRoot))
        {
            struct FdSetElement_ *elem = RB_ROOT(&self->mRoot);

            RB_REMOVE(FdSetTree_, &self->mRoot, elem);

            free(elem);
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
int
createFdSet(struct FdSet *self)
{
    int rc = -1;

    self->mSize = 0;
    RB_INIT(&self->mRoot);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
insertFdSetRange(struct FdSet *self, int aFirstFd, int aLastFd)
{
    int rc = -1;

    bool inserted = false;

    struct FdSetElement_ *elem = 0;

    ERROR_UNLESS(
        aFirstFd <= aLastFd,
        {
            errno = EINVAL;
        });

    ERROR_UNLESS(
        elem = malloc(sizeof(*elem)));

    elem->mFirstFd = aFirstFd;
    elem->mLastFd  = aLastFd;

    ERROR_IF(
        RB_INSERT(FdSetTree_, &self->mRoot, elem),
        {
            errno = EEXIST;
        });
    inserted = true;

    struct FdSetElement_ *prev = RB_PREV(FdSetTree_, &self->mRoot, elem);
    struct FdSetElement_ *next = RB_NEXT(FdSetTree_, &self->mRoot, elem);

    ERROR_UNLESS(
        ! prev || prev->mLastFd < elem->mFirstFd,
        {
            errno = EEXIST;
        });

    ERROR_UNLESS(
        ! next || elem->mLastFd < next->mFirstFd,
        {
            errno = EEXIST;
        });

    elem = 0;

    rc = 0;

Finally:

    FINALLY
    ({
        if (inserted && elem)
            RB_REMOVE(FdSetTree_, &self->mRoot, elem);

        free(elem);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
removeFdSetRange(struct FdSet *self, int aFirstFd, int aLastFd)
{
    int rc = -1;

    struct FdSetElement_ find =
    {
        .mFirstFd = aFirstFd,
        .mLastFd  = aLastFd,
    };

    struct FdSetElement_ *elem;
    ERROR_UNLESS(
        elem = RB_FIND(FdSetTree_, &self->mRoot, &find),
        {
            errno = ENOENT;
        });

    RB_REMOVE(FdSetTree_, &self->mRoot, elem);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
ssize_t
visitFdSet(const struct FdSet *self, struct FdSetVisitor aVisitor)
{
    ssize_t rc = -1;

    ssize_t visited = 0;

    struct FdSetElement_ *elem;
    RB_FOREACH(elem, FdSetTree_, &((struct FdSet *) self)->mRoot)
    {
        int err;
        ERROR_IF(
            (err = callFdSetVisitor(aVisitor, elem->mFirstFd, elem->mLastFd),
             err && EINTR != errno));

        ++visited;

        if (err)
            break;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc ? rc : visited;
}

/* -------------------------------------------------------------------------- */
