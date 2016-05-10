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

/* -------------------------------------------------------------------------- */
#ifndef __cplusplus
#define METHOD_CTOR_(Struct_)
#else
#define METHOD_CTOR_(Struct_) METHOD_CTOR_1_(Struct_)
#define METHOD_CTOR_1_(Struct_)                            \
    explicit Struct_(Struct_ ## T_ aMethod, void *aObject) \
    { *this = Struct_ ## _(aMethod, aObject); }
#endif

/* -------------------------------------------------------------------------- */
#define METHOD_ARGS_(...) , ## __VA_ARGS__

/* -------------------------------------------------------------------------- */
#define METHOD_TRAMPOLINE(Method_, Object_, Name_, ArgList_, CallList_)  \
({                                                                       \
    typedef __typeof__((Object_)) ObjectT_;                              \
                                                                         \
    void (*Validate_)(ObjectT_ METHOD_ARGS_ ArgList_) = (Method_);       \
                                                                         \
    Name_(                                                               \
        ! Validate_                                                      \
        ? 0                                                              \
        : LAMBDA(                                                        \
            void, (void *Self_ METHOD_ARGS_ ArgList_),                   \
            {                                                            \
                void (*method_)(                                         \
                  ObjectT_ METHOD_ARGS_ ArgList_) = (Method_);           \
                                                                         \
                method_(                                                 \
                    (ObjectT_) Self_ METHOD_ARGS_ CallList_);            \
            }),                                                          \
        (Object_));                                                      \
})

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_ARG_LIST_VoidMethod  ()
#define METHOD_CALL_LIST_VoidMethod ()

#define METHOD_NAME      VoidMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_VoidMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_VoidMethod
#include "method_.h"

#define VoidMethod(Method_, Object_)            \
    METHOD_TRAMPOLINE(                          \
        Method_, Object_,                       \
        VoidMethod_,                            \
        METHOD_ARG_LIST_VoidMethod,             \
        METHOD_CALL_LIST_VoidMethod)

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_ARG_LIST_VoidIntMethod  (int aArg_)
#define METHOD_CALL_LIST_VoidIntMethod (aArg_)

#define METHOD_NAME      VoidIntMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_VoidIntMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_VoidIntMethod
#include "method_.h"

#define VoidIntMethod(Method_, Object_)         \
    METHOD_TRAMPOLINE(                          \
        Method_, Object_,                       \
        VoidIntMethod_,                         \
        METHOD_ARG_LIST_VoidIntMethod,          \
        METHOD_CALL_LIST_VoidIntMethod)

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* METHOD_H */

/* -------------------------------------------------------------------------- */
#ifdef METHOD_DEFINITION
#undef METHOD_DEFINITION

#include "macros_.h"
#include "error_.h"

typedef void (*CONCAT(METHOD_NAME, T_))(void *self
                                        EXPAND(METHOD_ARGS_ METHOD_ARG_LIST));

static __inline__ struct METHOD_NAME
CONCAT(METHOD_NAME, _) (CONCAT(METHOD_NAME, T_) aMethod, void *aObject);

struct METHOD_NAME
{
    METHOD_CTOR_(METHOD_NAME)

    CONCAT(METHOD_NAME, T_) mMethod;
    void                   *mObject;
};

static __inline__ struct METHOD_NAME
CONCAT(METHOD_NAME, _) (CONCAT(METHOD_NAME, T_) aMethod, void *aObject)
{
    Error_ensure_(aMethod || ! aObject);

    return (struct METHOD_NAME)
    {
        mMethod : aMethod,
        mObject : aObject,
    };
}

static __inline__ void
CONCAT(call, METHOD_NAME) (struct METHOD_NAME self
                           EXPAND(METHOD_ARGS_ METHOD_ARG_LIST))
{
    Error_ensure_(self.mMethod);

    self.mMethod(self.mObject EXPAND(METHOD_ARGS_ METHOD_CALL_LIST));
}

static __inline__ bool
CONCAT(CONCAT(own, METHOD_NAME), Nil)(struct METHOD_NAME self)
{
    return ! self.mMethod;
}

#undef METHOD_NAME
#undef METHOD_ARG_LIST
#undef METHOD_CALL_LIST

#endif /* METHOD_DEFINITION */
