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
#ifndef FDSET_H
#define FDSET_H

#include "compiler_.h"

#include "tree_.h"

#include <stdio.h>

#include <sys/types.h>

/* -------------------------------------------------------------------------- */
BEGIN_C_SCOPE;
struct File;
struct FdRange;

struct FdRange
FdRange_(int aLhs, int aRhs);

struct FdRange
{
#ifdef __cplusplus
    FdRange(int aLhs, int aRhs)
    { *this = FdRange_(aLhs, aRhs); }
#endif

    int mLhs;
    int mRhs;
};
END_C_SCOPE;

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_FdSetVisitor    int
#define METHOD_CONST_FdSetVisitor
#define METHOD_ARG_LIST_FdSetVisitor  (struct FdRange aFdRange_)
#define METHOD_CALL_LIST_FdSetVisitor (aFdRange_)

#define METHOD_NAME      FdSetVisitor
#define METHOD_RETURN    METHOD_RETURN_FdSetVisitor
#define METHOD_CONST     METHOD_CONST_FdSetVisitor
#define METHOD_ARG_LIST  METHOD_ARG_LIST_FdSetVisitor
#define METHOD_CALL_LIST METHOD_CALL_LIST_FdSetVisitor
#include "method_.h"

#define FdSetVisitor(Object_, Method_)          \
    METHOD_TRAMPOLINE(                          \
        Object_, Method_,                       \
        FdSetVisitor_,                          \
        METHOD_RETURN_FdSetVisitor,             \
        METHOD_CONST_FdSetVisitor,              \
        METHOD_ARG_LIST_FdSetVisitor,           \
        METHOD_CALL_LIST_FdSetVisitor)

/* -------------------------------------------------------------------------- */
BEGIN_C_SCOPE;

struct FdSetElement_
{
    RB_ENTRY(FdSetElement_) mTree;

    struct FdRange mRange;
};

typedef RB_HEAD(FdSetTree_, FdSetElement_) FdSetTreeT_;

struct FdSet
{
    FdSetTreeT_ mRoot;

    size_t mSize;
};

#define FDSET_INITIALIZER(FdSet_) \
    { .mRoot = RB_INITIALIZER((FdSet_).mRoot) }

/* -------------------------------------------------------------------------- */
CHECKED int
createFdSet(struct FdSet *self);

struct FdSet *
closeFdSet(struct FdSet *self);

int
printFdSet(const struct FdSet *self, FILE *aFile);

void
clearFdSet(struct FdSet *self);

CHECKED int
invertFdSet(struct FdSet *self);

CHECKED int
insertFdSetRange(struct FdSet *self, struct FdRange aRange);

CHECKED int
removeFdSetRange(struct FdSet *self, struct FdRange aRange);

CHECKED int
insertFdSetFile(struct FdSet *self, const struct File *aFile);

CHECKED int
removeFdSetFile(struct FdSet *self, const struct File *aFile);

CHECKED ssize_t
visitFdSet(const struct FdSet *self, struct FdSetVisitor aVisitor);

/* -------------------------------------------------------------------------- */
#ifndef __cplusplus
static inline struct FdRange
FdRange(int aLhs, int aRhs)
{
    return FdRange_(aLhs, aRhs);
}
#endif

int
containsFdRange(struct FdRange self, struct FdRange aOther);

bool
leftFdRangeOf(struct FdRange self, struct FdRange aOther);

bool
rightFdRangeOf(struct FdRange self, struct FdRange aOther);

bool
leftFdRangeNeighbour(struct FdRange self, struct FdRange aOther);

bool
rightFdRangeNeighbour(struct FdRange self, struct FdRange aOther);

/* -------------------------------------------------------------------------- */

END_C_SCOPE;

#endif /* FDSET_H */
