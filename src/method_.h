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

#include "lambda_.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
#ifndef __cplusplus
#define METHOD_CTOR_(Const_, Struct_)
#else
#define METHOD_CTOR_(Const_, Struct_)                   \
    explicit Struct_(CONCAT(Struct_, T_) aMethod,       \
                     Const_ void        *aObject)       \
    : mMethod(aMethod),                                 \
      mObject(aObject)                                  \
    { }
#endif

/* -------------------------------------------------------------------------- */
#define METHOD_ARGS_(...) , ## __VA_ARGS__

/* -------------------------------------------------------------------------- */
#define METHOD_TRAMPOLINE(                                               \
    Method_, Object_, Name_, Return_, Const_, ArgList_, CallList_)       \
(*({                                                                     \
    typedef Const_ __typeof__(*(Object_)) *ObjectT_;                     \
                                                                         \
    Return_ (*Validate_)(ObjectT_ METHOD_ARGS_ ArgList_) = (Method_);    \
                                                                         \
    __typeof__(Name_(0,0)) Instance_ = Name_(                            \
        ! Validate_                                                      \
        ? 0                                                              \
        : LAMBDA(                                                        \
            Return_, (Const_ void *Self_ METHOD_ARGS_ ArgList_),         \
            {                                                            \
                Return_ (*method_)(                                      \
                  ObjectT_ METHOD_ARGS_ ArgList_) = (Method_);           \
                                                                         \
                return method_(                                          \
                    (ObjectT_) Self_ METHOD_ARGS_ CallList_);            \
            }),                                                          \
        (Object_));                                                      \
                                                                         \
    &Instance_;                                                          \
}))

/* -------------------------------------------------------------------------- */
void
methodEnsure_(const char *aFunction, const char *aFile, unsigned aLine,
              const char *aPredicate)
    __attribute__ ((__noreturn__));

#define METHOD_ENSURE_(aPredicate)                                      \
    do                                                                  \
        if ( ! (aPredicate))                                            \
            methodEnsure_(__func__, __FILE__, __LINE__, # aPredicate);  \
    while (0);

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_VoidMethod    void
#define METHOD_CONST_VoidMethod
#define METHOD_ARG_LIST_VoidMethod  ()
#define METHOD_CALL_LIST_VoidMethod ()

#define METHOD_NAME      VoidMethod
#define METHOD_RETURN    METHOD_RETURN_VoidMethod
#define METHOD_CONST     METHOD_CONST_VoidMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_VoidMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_VoidMethod
#include "method_.h"

#define VoidMethod(Method_, Object_)            \
    METHOD_TRAMPOLINE(                          \
        Method_, Object_,                       \
        VoidMethod_,                            \
        METHOD_RETURN_VoidMethod,               \
        METHOD_CONST_VoidMethod,                \
        METHOD_ARG_LIST_VoidMethod,             \
        METHOD_CALL_LIST_VoidMethod)

/* -------------------------------------------------------------------------- */
#define METHOD_DEFINITION
#define METHOD_RETURN_VoidIntMethod    void
#define METHOD_CONST_VoidIntMethod
#define METHOD_ARG_LIST_VoidIntMethod  (int aArg_)
#define METHOD_CALL_LIST_VoidIntMethod (aArg_)

#define METHOD_NAME      VoidIntMethod
#define METHOD_RETURN    METHOD_RETURN_VoidIntMethod
#define METHOD_CONST     METHOD_CONST_VoidIntMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_VoidIntMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_VoidIntMethod
#include "method_.h"

#define VoidIntMethod(Method_, Object_)         \
    METHOD_TRAMPOLINE(                          \
        Method_, Object_,                       \
        VoidIntMethod_,                         \
        METHOD_RETURN_VoidIntMethod,            \
        METHOD_CONST_VoidIntMethod,             \
        METHOD_ARG_LIST_VoidIntMethod,          \
        METHOD_CALL_LIST_VoidIntMethod)

#ifdef __cplusplus
}
#endif

#endif /* METHOD_H */

/* -------------------------------------------------------------------------- */
#ifdef METHOD_DEFINITION
#undef METHOD_DEFINITION

#include "macros_.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef METHOD_RETURN (*CONCAT(METHOD_NAME, T_))(
    METHOD_CONST void *self EXPAND(METHOD_ARGS_ METHOD_ARG_LIST));

static __inline__ struct METHOD_NAME
CONCAT(METHOD_NAME, _) (CONCAT(METHOD_NAME, T_) aMethod,
                        METHOD_CONST void      *aObject);

struct METHOD_NAME
{
    METHOD_CTOR_(METHOD_CONST, METHOD_NAME)

    CONCAT(METHOD_NAME, T_) mMethod;
    METHOD_CONST void      *mObject;
};

static __inline__ struct METHOD_NAME
CONCAT(METHOD_NAME, _) (CONCAT(METHOD_NAME, T_) aMethod,
                        METHOD_CONST void      *aObject)
{
    METHOD_ENSURE_(aMethod || ! aObject);

    /* In C++ programs, this initialiser will use the struct ctor, so
     * the struct ctor must not use this function to avoid death
     * by recursion. */

    return (struct METHOD_NAME)
    {
        mMethod : aMethod,
        mObject : aObject,
    };
}

static __inline__ METHOD_RETURN
CONCAT(call, METHOD_NAME) (struct METHOD_NAME self
                           EXPAND(METHOD_ARGS_ METHOD_ARG_LIST))
{
    METHOD_ENSURE_(self.mMethod);

    return self.mMethod(self.mObject EXPAND(METHOD_ARGS_ METHOD_CALL_LIST));
}

static __inline__ bool
CONCAT(CONCAT(own, METHOD_NAME), Nil)(struct METHOD_NAME self)
{
    return ! self.mMethod;
}

static __inline__ struct METHOD_NAME
CONCAT(METHOD_NAME, Nil)(void)
{
    return CONCAT(METHOD_NAME, _)(0, 0);
}

#ifdef __cplusplus
}
#endif

/* -------------------------------------------------------------------------- */

#undef METHOD_NAME
#undef METHOD_RETURN
#undef METHOD_CONST
#undef METHOD_ARG_LIST
#undef METHOD_CALL_LIST

#endif /* METHOD_DEFINITION */
