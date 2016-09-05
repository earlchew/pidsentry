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
#include "file_.h"
#include "error_.h"

#include <stdlib.h>
#include <limits.h>

/* -------------------------------------------------------------------------- */
static int
rankFdSetElement_(struct FdSetElement_ *aLhs, struct FdSetElement_ *aRhs)
{
    int rank =
        ( (aLhs->mRange.mLhs > aRhs->mRange.mLhs) -
          (aLhs->mRange.mLhs < aRhs->mRange.mLhs) );

    if ( ! rank)
        rank =
            ( (aLhs->mRange.mRhs > aRhs->mRange.mRhs) -
              (aLhs->mRange.mRhs < aRhs->mRange.mRhs) );

    return rank;
}

RB_GENERATE_STATIC(FdSetTree_, FdSetElement_, mTree, rankFdSetElement_)

/* -------------------------------------------------------------------------- */
int
printFdSet(const struct FdSet *self, FILE *aFile)
{
    int rc = -1;

    int printed = 0;

    ERROR_IF(
        PRINTF(printed, fprintf(aFile, "<fdset %p", self)));

    struct FdSetElement_ *elem;
    RB_FOREACH(elem, FdSetTree_, &((struct FdSet *) self)->mRoot)
    {
        ERROR_IF(
            PRINTF(
                printed,
                fprintf(aFile,
                        " (%d,%d)", elem->mRange.mLhs, elem->mRange.mRhs)));
    }

    ERROR_IF(
        PRINTF(printed, fprintf(aFile, ">")));

    rc = 0;

Finally:

    FINALLY({});

    return printed ? printed : rc;
}

/* -------------------------------------------------------------------------- */
struct FdSet *
closeFdSet(struct FdSet *self)
{
    if (self)
    {
        clearFdSet(self);
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
void
clearFdSet(struct FdSet *self)
{
    struct FdSetElement_ *elem = RB_ROOT(&self->mRoot);

    while (elem)
    {
        /* The loop invariant is that the subtree rooted at elem is still
         * valid, and must be cleared. Clear the two subtrees before freeing
         * the root of the subtree. */

        struct FdSetElement_ *lh = RB_LEFT(elem, mTree);
        struct FdSetElement_ *rh = RB_RIGHT(elem, mTree);

        if (lh)
        {
            elem = lh;
            continue;
        }

        if (rh)
        {
            elem = rh;
            continue;
        }

        /* Only free the root of the subtree while walking up the
         * subtree, thus ensuring that the left and right child
         * subtrees have already been released. */

        do
        {
            /* The loop-invariant is that children, if any, of the subtree
             * rooted at elem have been cleared, leaving the root itself
             * to be freed. */

            struct FdSetElement_ *parent = RB_PARENT(elem, mTree);

            free(elem);

            if (parent)
            {
                struct FdSetElement_ *plhs = RB_LEFT(parent, mTree);
                struct FdSetElement_ *prhs = RB_RIGHT(parent, mTree);

                if (elem == plhs && prhs)
                {
                    /* If the subtree rooted at elem is the left subtree
                     * of its parent, and there is a right subtree, then
                     * the next step is to process the right subtree. */

                    elem = prhs;
                    break;
                }
            }

            elem = parent;

        } while (elem);
    }

    RB_INIT(&self->mRoot);
}

/* -------------------------------------------------------------------------- */
int
invertFdSet(struct FdSet *self)
{
    int rc = -1;

    struct FdSetElement_ *elem = 0;

    /* Inverting the set can be modelled as creating a new set of the gaps
     * between the ranges of the old set. If there are N ranges in the old
     * set, there will be up to N+1 ranges in the new inverted set. */

    if (RB_EMPTY(&self->mRoot))
    {
        ERROR_IF(
            insertFdSetRange(self, FdRange(0, INT_MAX)));
    }
    else
    {
        struct FdSetElement_ *next = RB_MAX(FdSetTree_, &self->mRoot);

        if (INT_MAX == next->mRange.mRhs)
        {
            do
            {
                if ( ! next->mRange.mLhs)
                {
                    RB_REMOVE(FdSetTree_, &self->mRoot, next);
                    free(next);
                    next = 0;
                }
                else
                {
                    struct FdSetElement_ *prev =
                        RB_PREV(FdSetTree_, &self->mRoot, next);

                    next->mRange.mRhs = next->mRange.mLhs - 1;
                    next->mRange.mLhs = prev ? prev->mRange.mRhs + 1 : 0;

                    next = prev;
                }
            } while (next);
        }
        else
        {
            struct FdSetElement_ *prev = RB_MIN(FdSetTree_, &self->mRoot);

            if (prev->mRange.mLhs)
            {
                ERROR_UNLESS(
                    elem = malloc(sizeof(*elem)));

                elem->mRange = FdRange(0, prev->mRange.mLhs - 1);

                ERROR_IF(
                    RB_INSERT(FdSetTree_, &self->mRoot, elem),
                    {
                        errno = EEXIST;
                    });

                elem = 0;
            }

            do
            {
                next = RB_NEXT(FdSetTree_, &self->mRoot, prev);

                prev->mRange.mLhs = prev->mRange.mRhs + 1;
                prev->mRange.mRhs = next ? next->mRange.mLhs - 1 : INT_MAX;

                prev = next;

            } while (prev);
        }
    }

    rc = 0;

Finally:

    FINALLY
    ({
        free(elem);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
insertFdSet(struct FdSet *self, int aFd)
{
    return insertFdSetRange(self, FdRange(aFd, aFd));
}

/* -------------------------------------------------------------------------- */
int
removeFdSet(struct FdSet *self, int aFd)
{
    return removeFdSetRange(self, FdRange(aFd, aFd));
}

/* -------------------------------------------------------------------------- */
int
insertFdSetFile(struct FdSet *self, const struct File *aFile)
{
    return insertFdSetRange(self, FdRange(aFile->mFd, aFile->mFd));
}

/* -------------------------------------------------------------------------- */
int
removeFdSetFile(struct FdSet *self, const struct File *aFile)
{
    return removeFdSetRange(self, FdRange(aFile->mFd, aFile->mFd));
}

/* -------------------------------------------------------------------------- */
int
insertFdSetRange(struct FdSet *self, struct FdRange aRange)
{
    int rc = -1;

    bool inserted = false;

    struct FdSetElement_ *elem = 0;

    ERROR_UNLESS(
        elem = malloc(sizeof(*elem)));

    elem->mRange = aRange;

    ERROR_IF(
        RB_INSERT(FdSetTree_, &self->mRoot, elem),
        {
            errno = EEXIST;
        });
    inserted = true;

    struct FdSetElement_ *prev = RB_PREV(FdSetTree_, &self->mRoot, elem);
    struct FdSetElement_ *next = RB_NEXT(FdSetTree_, &self->mRoot, elem);

    ERROR_UNLESS(
        ! prev || leftFdRangeOf(elem->mRange, prev->mRange),
        {
            errno = EEXIST;
        });

    ERROR_UNLESS(
        ! next || rightFdRangeOf(elem->mRange, next->mRange),
        {
            errno = EEXIST;
        });

    if (prev && leftFdRangeNeighbour(elem->mRange, prev->mRange))
    {
        RB_REMOVE(FdSetTree_, &self->mRoot, elem);

        prev->mRange.mRhs = elem->mRange.mRhs;
        free(elem);

        elem = prev;
    }

    if (next && rightFdRangeNeighbour(elem->mRange, next->mRange))
    {
        RB_REMOVE(FdSetTree_, &self->mRoot, elem);

        next->mRange.mLhs = elem->mRange.mLhs;
        free(elem);

        elem = next;
    }

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
removeFdSetRange(struct FdSet *self, struct FdRange aRange)
{
    int rc = -1;

    struct FdRange  fdRange_;
    struct FdRange *fdRange = 0;

    struct FdSetElement_ find =
    {
        .mRange = aRange,
    };

    ERROR_IF(
        RB_EMPTY(&self->mRoot),
        {
            errno = ENOENT;
        });

    struct FdSetElement_ *elem = RB_NFIND(FdSetTree_, &self->mRoot, &find);

    if ( ! elem)
        elem = RB_MAX(FdSetTree_, &self->mRoot);

    int contained = containsFdRange(elem->mRange, aRange);

    if ( ! contained)
    {
        ERROR_UNLESS(
            elem = RB_PREV(FdSetTree_, &self->mRoot, elem),
            {
                errno = ENOENT;
            });

        ERROR_UNLESS(
            contained = containsFdRange(elem->mRange, aRange),
            {
                errno = ENOENT;
            });
    }

    switch (contained)
    {
    case 1:
        elem->mRange.mLhs = aRange.mRhs + 1;
        break;

    case 3:
        RB_REMOVE(FdSetTree_, &self->mRoot, elem);
        free(elem);
        break;

    case 2:
        elem->mRange.mRhs = aRange.mLhs - 1;
        break;

    default:
        {
          struct FdRange lhSide = FdRange(elem->mRange.mLhs,aRange.mLhs-1);
          struct FdRange rhSide = FdRange(aRange.mRhs+1,    elem->mRange.mRhs);

          fdRange_ = elem->mRange;
          fdRange  = &fdRange_;

          elem->mRange = lhSide;
          ERROR_IF(
              insertFdSetRange(self, rhSide));

          fdRange = 0;
        }
        break;
    }

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc && fdRange)
            elem->mRange = *fdRange;
    });

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
            (err = callFdSetVisitor(aVisitor, elem->mRange),
             -1 == err));

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
struct FdRange
FdRange_(int aLhs, int aRhs)
{
    ensure(0 <= aLhs && aLhs <= aRhs);

    return (struct FdRange)
    {
        .mLhs = aLhs,
        .mRhs = aRhs,
    };
}

/* -------------------------------------------------------------------------- */
static inline bool
containsFdRange_(struct FdRange self, int aFd)
{
    return self.mLhs <= aFd && aFd <= self.mRhs;
}

/* -------------------------------------------------------------------------- */
int
containsFdRange(struct FdRange self, struct FdRange aOther)
{
    int contained = 0;

    if (containsFdRange_(self, aOther.mLhs) &&
        containsFdRange_(self, aOther.mRhs) )
    {
        int lhs = (self.mLhs == aOther.mLhs) << 0;
        int rhs = (self.mRhs == aOther.mRhs) << 1;

        contained = lhs | rhs;

        if ( ! contained)
            contained = -1;
    }

    return contained;
}

/* -------------------------------------------------------------------------- */
bool
leftFdRangeOf(struct FdRange self, struct FdRange aOther)
{
    return aOther.mRhs < self.mLhs;
}

/* -------------------------------------------------------------------------- */
bool
rightFdRangeOf(struct FdRange self, struct FdRange aOther)
{
    return self.mRhs < aOther.mLhs;
}

/* -------------------------------------------------------------------------- */
bool
leftFdRangeNeighbour(struct FdRange self, struct FdRange aOther)
{
    return self.mLhs - 1 == aOther.mRhs;
}

/* -------------------------------------------------------------------------- */
bool
rightFdRangeNeighbour(struct FdRange self, struct FdRange aOther)
{
    return self.mRhs + 1 == aOther.mLhs;
}

/* -------------------------------------------------------------------------- */
