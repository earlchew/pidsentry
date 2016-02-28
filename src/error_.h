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
#ifndef ERROR_H
#define ERROR_H

#include "options_.h"
#include "process_.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
#define FINALLY(...)      \
    do                    \
    {                     \
        int err_ = errno; \
        __VA_ARGS__;      \
        errno = err_;     \
    } while (0)

#define FINALLY_IF(Predicate_, ...)                  \
    do                                               \
    {                                                \
        struct ErrorFrame frame_ =                   \
            ERRORFRAME_INIT( # Predicate_ );         \
                                                     \
        unsigned frameLevel_ = ownErrorFrameLevel(); \
                                                     \
        if ((Predicate_))                            \
        {                                            \
            __VA_ARGS__;                             \
            if ( ! frameLevel_)                      \
                pushErrorFrameLevel(&frame_, errno); \
            goto Finally;                            \
        }                                            \
                                                     \
        resetErrorFrameLevel();                      \
    }                                                \
    while (0)

#define TERMINATE_IF(Predicate_, ...)                           \
    do                                                          \
    {                                                           \
        if ((Predicate_))                                       \
        {                                                       \
            __VA_ARGS__;                                        \
            Error_terminate_(errno, "%s", # Predicate_);        \
        }                                                       \
    }                                                           \
    while (0)

#define TERMINATE_UNLESS(Predicate_, ...)                       \
    do                                                          \
    {                                                           \
        if ( ! (Predicate_))                                    \
        {                                                       \
            __VA_ARGS__;                                        \
            Error_terminate_(errno, "%s", # Predicate_);        \
        }                                                       \
    }                                                           \
    while (0)

/* -------------------------------------------------------------------------- */
struct ErrorFrame
{
    const char *mFile;
    unsigned    mLine;
    const char *mName;
    const char *mText;
    int         mErrno;
};

#define ERRORFRAME_INIT(aText) { __FILE__, __LINE__, __func__, aText }

struct ErrorFrameLevel
{
    unsigned mLevel;
};

enum ErrorFrameStackKind
{
    ErrorFrameStackThread,
    ErrorFrameStackSignal,
    ErrorFrameStackKinds,
};

void
pushErrorFrameLevel(const struct ErrorFrame *aFrame, int aErrno);

void
resetErrorFrameLevel(void);

enum ErrorFrameStackKind
switchErrorFrameStack(enum ErrorFrameStackKind aStack);

unsigned
ownErrorFrameLevel(void);

const struct ErrorFrame *
ownErrorFrame(enum ErrorFrameStackKind aStack, unsigned aLevel);

void
logErrorFrameWarning(void);

/* -------------------------------------------------------------------------- */
#define breadcrumb Error_breadcrumb_
#define Error_breadcrumb_() \
    debug_(__FILE__, __LINE__, ".")

#define debug Error_debug_
#define Error_debug_(aLevel, ...)                       \
    do                                                  \
        if ((aLevel) < gOptions.mDebug)                 \
            debug_(__FILE__, __LINE__, ## __VA_ARGS__); \
    while (0)

void
debug_(
    const char *aFile, unsigned aLine,
    const char *aFmt, ...)
    __attribute__ ((__format__(__printf__, 3, 4)));

/* -------------------------------------------------------------------------- */
#define ensure Error_ensure_
#define Error_ensure_(aPredicate)                               \
    do                                                          \
        if ( ! (aPredicate))                                    \
            ensure_(__FILE__, __LINE__, # aPredicate);          \
    while (0)

void
ensure_(const char *aFile, unsigned aLine, ...);

/* -------------------------------------------------------------------------- */
#define warn Error_warn_
#define Error_warn_(aErrCode, ...) \
    warn_((aErrCode), __FILE__, __LINE__, ## __VA_ARGS__)

void
warn_(
    int aErrCode,
    const char *aFile, unsigned aLine,
    const char *aFmt, ...)
    __attribute__ ((__format__(__printf__, 4, 5)));

/* -------------------------------------------------------------------------- */
#define terminate Error_terminate_
#define Error_terminate_(aErrCode, ...) \
    terminate_((aErrCode), __FILE__, __LINE__, ## __VA_ARGS__)

void
terminate_(
    int aErrCode,
    const char *aFile, unsigned aLine,
    const char *aFmt, ...)
    __attribute__ ((__format__(__printf__, 4, 5), __noreturn__));

/* -------------------------------------------------------------------------- */
int
Error_init(void);

int
Error_exit(void);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* ERROR_H */
