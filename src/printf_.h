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
#ifndef PRINTF_H
#define PRINTF_H

#include "compiler_.h"
#include "method_.h"

#include <stdio.h>
#include <stdarg.h>

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_PrintfMethod    int
#define METHOD_CONST_PrintfMethod     const
#define METHOD_ARG_LIST_PrintfMethod  (FILE *aFile_)
#define METHOD_CALL_LIST_PrintfMethod (aFile_)

#define METHOD_NAME      PrintfMethod_
#define METHOD_RETURN    METHOD_RETURN_PrintfMethod
#define METHOD_CONST     METHOD_CONST_PrintfMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_PrintfMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_PrintfMethod
#include "method_.h"

#define PrintfMethod_(Method_, Object_)         \
    METHOD_TRAMPOLINE(                          \
        Method_, Object_,                       \
        PrintfMethod__,                         \
        METHOD_RETURN_PrintfMethod,             \
        METHOD_CONST_PrintfMethod,              \
        METHOD_ARG_LIST_PrintfMethod,           \
        METHOD_CALL_LIST_PrintfMethod)

/* -------------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

struct Type;

struct PrintfModule
{
    struct PrintfModule *mModule;
};

/* -------------------------------------------------------------------------- */
extern const struct Type * const printfMethodType_;

#define PrintfMethod(Method_, Object_)                  \
({                                                      \
    struct PrintfMethod printfMethod_ =                 \
    {                                                   \
        mType   : &printfMethodType_,                   \
        mMethod : PrintfMethod_((Method_), (Object_)),  \
    };                                                  \
                                                        \
    &printfMethod_;                                     \
})

struct PrintfMethod
{
    const struct Type * const *mType;

    struct PrintfMethod_ mMethod;
};

/* -------------------------------------------------------------------------- */
#define PRIs_Method "%%p<struct PrintfMethod>%%"
#define FMTs_Method(Method_, Object_) ( PrintfMethod((Method_), (Object_)) )

/* -------------------------------------------------------------------------- */
int
xprintf(const char *aFmt, ...)
    __attribute__ ((__format__(__printf__, 1, 2)));

int
xfprintf(FILE *aFile, const char *aFmt, ...)
    __attribute__ ((__format__(__printf__, 2, 3)));

CHECKED int
xsnprintf(char *aBuf, size_t aSize, const char *aFmt, ...)
    __attribute__ ((__format__(__printf__, 3, 4)));

int
xdprintf(int aFd, const char *aFmt, ...)
    __attribute__ ((__format__(__printf__, 2, 3)));

/* -------------------------------------------------------------------------- */
int
xvfprintf(FILE *aFile, const char *aFmt, va_list);

CHECKED int
xvsnprintf(char *aBuf, size_t aSize, const char *aFmt, va_list);

int
xvdprintf(int aFd, const char *aFmt, va_list);

/* -------------------------------------------------------------------------- */
CHECKED int
Printf_init(struct PrintfModule *self);

void
Printf_exit(struct PrintfModule *self);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* PRINTF_H */
