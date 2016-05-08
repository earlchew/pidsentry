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
#ifndef METHOD_H
#define METHOD_H

#include <stdbool.h>

#include "lambda_.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __cplusplus
#define METHOD_CTOR_(Struct_)
#else
#define METHOD_CTOR_(Struct_)                              \
    explicit Struct_(Struct_ ## T_ aMethod, void *aObject) \
    { *this = Struct_ ## _(aMethod, aObject); }
#endif

void
failMethodCtor_(void);

/* -------------------------------------------------------------------------- */
typedef void (*VoidMethodT_)(void *self);

struct VoidMethod
VoidMethod_(VoidMethodT_ aMethod, void *aObject);

struct VoidMethod
{
    METHOD_CTOR_(VoidMethod)

    void        *mObject;
    VoidMethodT_ mMethod;
};

#ifdef __cplusplus
#define VoidMethod(Method_, Object_) VoidMethod_(Method_, Object_)
#else
#define VoidMethod(Method_, Object_)                    \
({                                                      \
    typedef __typeof__((Object_)) ObjectT_;             \
                                                        \
    void (*Validate_)(ObjectT_) = (Method_);            \
                                                        \
    VoidMethod_(                                        \
        ! Validate_                                     \
        ? 0                                             \
        : LAMBDA(                                       \
            void, (void *Self_),                        \
            {                                           \
                void (*method_)(ObjectT_) = (Method_);  \
                                                        \
                method_((ObjectT_) Self_);              \
            }),                                         \
        (Object_));                                     \
})
#endif

void
callVoidMethod(struct VoidMethod self);

bool
ownVoidMethodNil(struct VoidMethod self);

/* -------------------------------------------------------------------------- */
typedef void (*VoidIntMethodT_)(void *self, int aArg);

struct VoidIntMethod
VoidIntMethod_(VoidIntMethodT_ aMethod, void *aObject);

struct VoidIntMethod
{
    METHOD_CTOR_(VoidIntMethod)

    void           *mObject;
    VoidIntMethodT_ mMethod;
};

#ifndef __cplusplus
static inline struct VoidIntMethod
VoidIntMethod(VoidIntMethodT_ aMethod, void *aObject)
{
    return VoidIntMethod_(aMethod, aObject);
}
#endif

void
callVoidIntMethod(struct VoidIntMethod self, int aArg);

bool
ownVoidIntMethodNil(struct VoidIntMethod self);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* METHOD_H */
