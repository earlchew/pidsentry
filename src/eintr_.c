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

#include "eintr_.h"
#include "dl_.h"
#include "error_.h"
#include "test_.h"

#include <unistd.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------- */
static unsigned moduleInit_;

/* -------------------------------------------------------------------------- */
/* Interrupted System Calls
 *
 * These interceptors provide a way to inject EINTR to obtain substantially
 * more test coverage when unit tests are run. */

struct SystemCall
{
    const char *mName;
    uintptr_t   mAddr;
};

enum SystemCallKind
{
    SYSTEMCALL_READ,
    SYSTEMCALL_KINDS
};

static struct SystemCall systemCall_[SYSTEMCALL_KINDS] =
{
    [SYSTEMCALL_READ] = { "read" },
};

/* -------------------------------------------------------------------------- */
#define SYSCALL_EINTR(Kind_, Function_)                         \
    ({                                                          \
        uintptr_t syscall_ = interruptSystemCall((Kind_));      \
                                                                \
        if ( ! syscall_)                                        \
        {                                                       \
            errno = EINTR;                                      \
            return -1;                                          \
        }                                                       \
                                                                \
        (DECLTYPE(Function_) *) syscall_;                       \
    })

/* -------------------------------------------------------------------------- */
static uintptr_t
initSystemCall(struct SystemCall *self)
{
    uintptr_t addr = self->mAddr;

    if ( ! addr)
    {
        const char *err;

        char *libName = findDlSymbol(self->mName, &addr, &err);

        ensure(libName);

        free(libName);

        self->mAddr = addr;
    }

    return addr;
}

/* -------------------------------------------------------------------------- */
static uintptr_t
interruptSystemCall(enum SystemCallKind aKind)
{
    struct SystemCall *sysCall = &systemCall_[aKind];

    uintptr_t addr = 0;

    if (testAction(TestLevelRace))
        debug(0, "inject EINTR into %s", sysCall->mName);
    else
        addr = initSystemCall(sysCall);

    return addr;
}

/* -------------------------------------------------------------------------- */
ssize_t
read(int aFd, void *aBuf, size_t aCount)
{
    return
        SYSCALL_EINTR(SYSTEMCALL_READ, read)(aFd, aBuf, aCount);
}

/* -------------------------------------------------------------------------- */
int
Eintr_init(struct EintrModule *self)
{
    int rc = -1;

    self->mModule = self;

    if ( ! moduleInit_)
    {
        for (size_t sx = 0; NUMBEROF(systemCall_) > sx; ++sx)
        {
            ERROR_UNLESS(
                initSystemCall(&systemCall_[sx]));
        }
    }

    ++moduleInit_;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
struct EintrModule *
Eintr_exit(struct EintrModule *self)
{
    if (self)
    {
        --moduleInit_;
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
