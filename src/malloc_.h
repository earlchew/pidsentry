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
#ifndef MALLOC_H
#define MALLOC_H

#define malloc         __libc_malloc
#define valloc         __libc_valloc
#define pvalloc        __libc_pvalloc
#define realloc        __libc_realloc
#define calloc         __libc_calloc
#define memalign       __libc_memalign
#define aligned_alloc  __libc_memalign
#define posix_memalign __posix_memalign
#define free           __libc_free
#define cfree          __libc_free
#include <stdlib.h>
#include <malloc.h>
#undef  malloc
#undef  valloc
#undef  pvalloc
#undef  calloc
#undef  realloc
#undef  memalign
#undef  aligned_alloc
#undef  posix_memalign
#undef  free
#undef  cfree

#include <stddef.h>

/* -------------------------------------------------------------------------- */
void *
malloc(size_t aSize);

void *
valloc(size_t aSize);

void *
pvalloc(size_t aSize);

void *
memalign(size_t aAlign, size_t aSize);

void *
aligned_alloc(size_t aAlign, size_t aSize);

int
posix_memalign(void **aBlock, size_t aAlign, size_t aSize);

void *
calloc(size_t aSize, size_t aElems);

void
free(void *aBlock);

void
cfree(void *aBlock);

void *
realloc(void *aBlock, size_t aSize);

/* -------------------------------------------------------------------------- */

#endif /* MALLOC_H */
