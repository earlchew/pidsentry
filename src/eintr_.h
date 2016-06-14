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
#ifndef EINTR_H
#define EINTR_H

#include "compiler_.h"

#ifdef EINTR_MODULE_DEFN_
#define pread          __pread
#define pwrite         __pwrite
#define preadv         __preadv
#define pwritev        __pwritev
#define read           __read
#define readv          __readv
#define write          __write
#define writev         __writev
#endif

#include <unistd.h>
#include <sys/uio.h>

#ifdef EINTR_MODULE_DEFN_
#undef  pread
#undef  preadv
#undef  pwrite
#undef  pwritev
#undef  read
#undef  readv
#undef  write
#undef  writev
#endif

/* -------------------------------------------------------------------------- */
struct EintrModule
{
    struct EintrModule *mModule;
};

/* -------------------------------------------------------------------------- */
#ifndef EINTR_MODULE_DEFN_
#define EINTR_FUNCTION_DECL_(Return_, Name_, Signature_) \
    Return_ Name_ ## _eintr Signature_;                  \
    struct EintrModule
#else
#define EINTR_FUNCTION_DECL_(Return_, Name_, Signature_) \
    Return_ Name_           Signature_;                  \
    Return_ Name_ ## _eintr Signature_;                  \
    struct EintrModule
#endif

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DECL_(
    ssize_t, read,
    (int aFd, void *aBuf, size_t aCount));

EINTR_FUNCTION_DECL_(
    ssize_t, write,
    (int aFd, const void *aBuf, size_t aCount));

EINTR_FUNCTION_DECL_(
    ssize_t, pread,
    (int aFd, void *aBuf, size_t aCount, off_t aOffset));

EINTR_FUNCTION_DECL_(
    ssize_t, pwrite,
    (int aFd, const void *aBuf, size_t aCount, off_t aOffset));

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DECL_(
    ssize_t, readv,
    (int aFd, const struct iovec *aVec, int aCount));

EINTR_FUNCTION_DECL_(
    ssize_t, writev,
    (int aFd, const struct iovec *aVec, int aCount));

EINTR_FUNCTION_DECL_(
    ssize_t, preadv,
    (int aFd, const struct iovec *aVec, int aCount, off_t aOffset));

EINTR_FUNCTION_DECL_(
    ssize_t, pwritev,
    (int aFd, const struct iovec *aVec, int aCount, off_t aOffset));

/* -------------------------------------------------------------------------- */
CHECKED int
Eintr_init(struct EintrModule *self);

CHECKED struct EintrModule *
Eintr_exit(struct EintrModule *self);

/* -------------------------------------------------------------------------- */

#endif /* EINTR_H */
