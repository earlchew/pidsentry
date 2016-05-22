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
#ifndef COMPILER_H
#define COMPILER_H

#include "macros_.h"

/* -------------------------------------------------------------------------- */
/* Scoping in C++
 *
 * Provide a clean way to scope areas containing C declarations when using
 * a C++ compiler.
 */
#ifdef __cplusplus
#define BEGIN_C_SCOPE struct CppScope_; extern "C" { struct CScope_
#define END_C_SCOPE   } struct CppScope_
#else
#define BEGIN_C_SCOPE struct CScope_
#define END_C_SCOPE   struct CScope_
#endif

/* -------------------------------------------------------------------------- */
/* Checked Return
 *
 * Where there are return code that need to be checked, this decorator
 * is used to have the compiler enforce policy. */

#define CHECKED __attribute__((__warn_unused_result__))

/* -------------------------------------------------------------------------- */
/* Abort
 *
 * Prefer to have the application call abortProcess() directly rather than
 * abort(). See abort_.c for the rationale. */

static __inline__ void
abort_(void)
    __attribute__((__deprecated__));

static __inline__ void
abort_(void)
{ }

#define abort(Arg_) IFEMPTY(abort_(), abort(Arg_), Arg_)

#endif /* COMPILER_H */
