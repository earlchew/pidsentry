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

#include "malloc_.h"
#include "thread_.h"
#include "error_.h"


/* -------------------------------------------------------------------------- */
/* Memory allocators
 *
 * The standard memory allocators are not when used in signal handlers
 * because the mutex used for the synchronisation could be interrupted
 * mid-cycle, or re-entered from the signal handler itself.
 *
 * Render the memory allocators safe by protecting the calls with
 * so that signals cannot be delivered in the thread in which the
 * allocator is running. Calls to the allocator from signal handlers
 * running in other threads are synchronised by the mutex in the
 * allocator itself. */

/* -------------------------------------------------------------------------- */
void *
malloc(size_t aSize)
{
    void *block = 0;

    struct ThreadSigMask threadSigMask;

    struct ThreadSigMask *sigMask = pushThreadSigMask(
        &threadSigMask, ThreadSigMaskBlock, 0);

    ERROR_UNLESS(
        (block = __libc_malloc(aSize)));

Finally:

    FINALLY
    ({
        sigMask = popThreadSigMask(sigMask);
    });

    return block;
}

/* -------------------------------------------------------------------------- */
void *
valloc(size_t aSize)
{
    void *block = 0;

    struct ThreadSigMask  threadSigMask_;
    struct ThreadSigMask *threadSigMask =
        pushThreadSigMask(&threadSigMask_, ThreadSigMaskBlock, 0);

    ERROR_UNLESS(
        (block = __libc_valloc(aSize)));

Finally:

    FINALLY
    ({
        threadSigMask = popThreadSigMask(threadSigMask);
    });

    return block;
}

/* -------------------------------------------------------------------------- */
void *
pvalloc(size_t aSize)
{
    void *block = 0;

    struct ThreadSigMask  threadSigMask_;
    struct ThreadSigMask *threadSigMask =
        pushThreadSigMask(&threadSigMask_, ThreadSigMaskBlock, 0);

    ERROR_UNLESS(
        (block = __libc_pvalloc(aSize)));

Finally:

    FINALLY
    ({
        threadSigMask = popThreadSigMask(threadSigMask);
    });

    return block;
}

/* -------------------------------------------------------------------------- */
void
free(void *aBlock)
{
    struct ThreadSigMask  threadSigMask_;
    struct ThreadSigMask *threadSigMask =
        pushThreadSigMask(&threadSigMask_, ThreadSigMaskBlock, 0);

    __libc_free(aBlock);

    threadSigMask = popThreadSigMask(threadSigMask);
}

/* -------------------------------------------------------------------------- */
void
cfree(void *aBlock)
{
    free(aBlock);
}

/* -------------------------------------------------------------------------- */
void *
memalign(size_t aAlign, size_t aSize)
{
    void *block = 0;

    struct ThreadSigMask  threadSigMask_;
    struct ThreadSigMask *threadSigMask =
        pushThreadSigMask(&threadSigMask_, ThreadSigMaskBlock, 0);

    ERROR_UNLESS(
        (block = __libc_memalign(aAlign, aSize)));

Finally:

    FINALLY
    ({
        threadSigMask = popThreadSigMask(threadSigMask);
    });

    return block;
}

/* -------------------------------------------------------------------------- */
void *
aligned_alloc(size_t aAlign, size_t aSize)
{
    return memalign(aAlign, aSize);
}

/* -------------------------------------------------------------------------- */
void *
realloc(void *aBlock, size_t aSize)
{
    void *block = 0;

    struct ThreadSigMask  threadSigMask_;
    struct ThreadSigMask *threadSigMask =
        pushThreadSigMask(&threadSigMask_, ThreadSigMaskBlock, 0);

    ERROR_UNLESS(
        (block = __libc_realloc(aBlock, aSize)));

Finally:

    FINALLY
    ({
        threadSigMask = popThreadSigMask(threadSigMask);
    });

    return block;
}

/* -------------------------------------------------------------------------- */
void *
calloc(size_t aSize, size_t aElems)
{
    void *block = 0;

    struct ThreadSigMask  threadSigMask_;
    struct ThreadSigMask *threadSigMask =
        pushThreadSigMask(&threadSigMask_, ThreadSigMaskBlock, 0);

    ERROR_UNLESS(
        (block = __libc_calloc(aSize, aElems)));

Finally:

    FINALLY
    ({
        threadSigMask = popThreadSigMask(threadSigMask);
    });

    return block;
}

/* -------------------------------------------------------------------------- */
int
posix_memalign(void **aBlock, size_t aAlign, size_t aSize)
{
    int rc;

    struct ThreadSigMask  threadSigMask_;
    struct ThreadSigMask *threadSigMask =
        pushThreadSigMask(&threadSigMask_, ThreadSigMaskBlock, 0);

    size_t words = aAlign / sizeof(void *);

    ERROR_IF(
        (aAlign % sizeof(void *)) || ! aAlign || (words & (words-1)),
        {
            rc = EINVAL;
        });

    void *block;
    ERROR_UNLESS(
        (block = __libc_memalign(aAlign, aSize)),
        {
            rc = errno;
        });

    *aBlock = block;

    rc = 0;

Finally:

    FINALLY
    ({
        threadSigMask = popThreadSigMask(threadSigMask);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
