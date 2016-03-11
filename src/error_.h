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
#include "test_.h"

#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Finally Unwinding
 *
 * Use ERROR_IF() and ERROR_UNLESS() to wrap calls to functions
 * that have error returns.
 *
 * Only wrap function calls because error simulation will conditionally
 * simulate error returns from these functions.
 *
 * Do not use these to wrap destructors since the destructors will
 * be used on the error return path. Destructors should have void
 * returns to avoid inadvertentl having an error return. */

#define ERROR_IF_(Sense_, Predicate_, ...)           \
    do                                               \
    {                                                \
        /* Do not allow error management within      \
         * FINALLY() blocks. */                      \
                                                     \
        const void *finally_                         \
        __attribute__((__unused__)) = 0;             \
                                                     \
        struct ErrorFrame frame_ =                   \
            ERRORFRAME_INIT( # Predicate_ );         \
                                                     \
        /* Stack unwinding restarts if a new frame   \
         * sequence is started. */                   \
                                                     \
        restartErrorFrameSequence();                 \
                                                     \
        if (testFinally(&frame_) ||                  \
            Sense_ (Predicate_))                     \
        {                                            \
            __VA_ARGS__                              \
                                                     \
            addErrorFrame(&frame_, errno);           \
            goto Error_;                             \
        }                                            \
    }                                                \
    while (0)

#define ERROR_IF(Predicate_, ...)                  \
    ERROR_IF_(/*!!*/, Predicate_, ## __VA_ARGS__)

#define ERROR_UNLESS(Predicate_, ...)              \
    ERROR_IF_(!, Predicate_, ##  __VA_ARGS__)

/* -------------------------------------------------------------------------- */
#define ABORT_IF(Predicate_, ...)                               \
    TERMINATE_IF_(terminate_, /*!!*/, Predicate_, ## __VA_ARGS__)

#define ABORT_UNLESS(Predicate_, ...)                           \
    TERMINATE_IF_(terminate_, !, Predicate_, ## __VA_ARGS__)

#define TERMINATE_IF_(Terminate_, Sense_, Predicate_, ...)      \
    do                                                          \
    {                                                           \
        /* Stack unwinding restarts if a new frame              \
         * sequence is started. */                              \
                                                                \
        struct ErrorFrameSequence frameSequence_ =              \
            pushErrorFrameSequence();                           \
                                                                \
        if (Sense_ (Predicate_))                                \
        {                                                       \
            void (*terminate)(                                  \
                int         aErrCode,                           \
                const char *aFunction,                          \
                const char *aFile,                              \
                unsigned    aLine,                              \
                const char *aFmt, ...)                          \
                __attribute__ ((                                \
                    __format__(__printf__, 5, 6),               \
                    __noreturn__)) = Terminate_;                \
                                                                \
            __VA_ARGS__                                         \
                                                                \
            do                                                  \
                (terminate)(                                    \
                    errno,                                      \
                    __func__, __FILE__, __LINE__,               \
                    "%s", # Predicate_);                        \
            while (0);                                          \
        }                                                       \
                                                                \
        popErrorFrameSequence(frameSequence_);                  \
    }                                                           \
    while (0)

/* -------------------------------------------------------------------------- */
#define FINALLY(...)      \
    do                    \
    {                     \
        int err_ = errno; \
        __VA_ARGS__;      \
        errno = err_;     \
    } while (0)

#define Finally                                 \
        /* Stack unwinding restarts if the      \
         * function completes without error. */ \
                                                \
        restartErrorFrameSequence();            \
                                                \
        int finally_                            \
        __attribute__((__unused__));            \
                                                \
        goto Error_;                            \
    Error_

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

struct ErrorFrameSequence
{
    unsigned mSequence;
};

enum ErrorFrameStackKind
{
    ErrorFrameStackThread = 0,
    ErrorFrameStackSignal,
    ErrorFrameStackKinds,
};

void
addErrorFrame(const struct ErrorFrame *aFrame, int aErrno);

void
restartErrorFrameSequence_(const char *aFile, unsigned aLine);

void
restartErrorFrameSequence(void);

struct ErrorFrameSequence
pushErrorFrameSequence(void);

void
popErrorFrameSequence(struct ErrorFrameSequence aSequence);

enum ErrorFrameStackKind
switchErrorFrameStack(enum ErrorFrameStackKind aStack);

unsigned
ownErrorFrameLevel(void);

const struct ErrorFrame *
ownErrorFrame(enum ErrorFrameStackKind aStack, unsigned aLevel);

void
logErrorFrameSequence(void);

/* -------------------------------------------------------------------------- */
#define breadcrumb Error_breadcrumb_
#define Error_breadcrumb_() \
    debug_(__func__, __FILE__, __LINE__, ".")

#define debug Error_debug_
#define Error_debug_(aLevel, ...)                                       \
    do                                                                  \
        if ((aLevel) < gOptions.mDebug)                                 \
            debug_(__func__, __FILE__, __LINE__, ## __VA_ARGS__);       \
    while (0)

void
debug_(
    const char *aFunction, const char *aFile, unsigned aLine,
    const char *aFmt, ...)
    __attribute__ ((__format__(__printf__, 4, 5)));

/* -------------------------------------------------------------------------- */
#define ensure Error_ensure_
#define Error_ensure_(aPredicate)                               \
    do                                                          \
        if ( ! (aPredicate))                                    \
            ensure_(__func__, __FILE__, __LINE__, # aPredicate); \
    while (0)

void
ensure_(const char *aFunction, const char *aFile, unsigned aLine, ...);

/* -------------------------------------------------------------------------- */
#define warn Error_warn_
#define Error_warn_(aErrCode, ...) \
    warn_((aErrCode), __func__, __FILE__, __LINE__, ## __VA_ARGS__)

void
warn_(
    int aErrCode,
    const char *aFunction, const char *aFile, unsigned aLine,
    const char *aFmt, ...)
    __attribute__ ((__format__(__printf__, 5, 6)));

/* -------------------------------------------------------------------------- */
#define message Error_message_
#define Error_message_(aErrCode, ...) \
    message_((aErrCode), __func__, __FILE__, __LINE__, ## __VA_ARGS__)

void
message_(
    int aErrCode,
    const char *aFunction, const char *aFile, unsigned aLine,
    const char *aFmt, ...)
    __attribute__ ((__format__(__printf__, 5, 6)));

/* -------------------------------------------------------------------------- */
#define terminate(aErrCode, ...) \
    terminate(aErrCode, __func__, __FILE__, __LINE__, ## __VA_ARGS__)

#define Error_terminate_(aErrCode, ...) \
    terminate_((aErrCode), __func__, __FILE__, __LINE__, ## __VA_ARGS__)

void
terminate_(
    int aErrCode,
    const char *aFunction, const char *aFile, unsigned aLine,
    const char *aFmt, ...)
    __attribute__ ((__format__(__printf__, 5, 6), __noreturn__));

/* -------------------------------------------------------------------------- */
int
Error_init(void);

void
Error_exit(void);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* ERROR_H */
