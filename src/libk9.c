/* -*- c-basic-offset:4; indent-tabs-mode:nil -*- vi: set sw=4 et: */
/*
// Copyright (c) 2015, Earl Chew
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

#include "libk9.h"

#include <stdio.h>
#include <unistd.h>

extern char **_dl_argv;

/* -------------------------------------------------------------------------- */
int
k9so()
{
    return 0;
}

/* -------------------------------------------------------------------------- */
static void  __attribute__((constructor))
libk9_init()
{
    dprintf(2, "@@@ ENTER %d %s\n", (int) getpid(), _dl_argv[0]);

    char **p = _dl_argv;

    while (*p)
        ++p;
    ++p;

    for ( ; *p; ++p)
    {
        if ((*p)[0] == 'L' && (*p)[1] == 'D')
        {
            dprintf(2, "%s\n", *p);
            (*p)[sizeof("LD_PRELOAD=")] = 'X';
        }
    }
    dprintf(2, "DONE\n");
}

/* -------------------------------------------------------------------------- */
static void __attribute__((destructor))
libk9_exit()
{
    dprintf(2, "@@@ EXIT %d\n", (int) getpid());
}

/* -------------------------------------------------------------------------- */