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

#include "dl_.h"
#include "error_.h"
#include "macros_.h"

#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <link.h>

/* -------------------------------------------------------------------------- */
struct DlSymbolVisitor_
{
    uintptr_t mSoAddr;
    char     *mSoPath;
};

static int
dlSymbolVisitor_(struct dl_phdr_info *aInfo, size_t aSize, void *aVisitor)
{
    int rc = -1;

    struct DlSymbolVisitor_ *visitor = aVisitor;

    for (unsigned ix = 0; ix < aInfo->dlpi_phnum; ++ix)
    {
        uintptr_t addr = aInfo->dlpi_addr + aInfo->dlpi_phdr[ix].p_vaddr;
        size_t    size = aInfo->dlpi_phdr[ix].p_memsz;

        if (addr <= visitor->mSoAddr && visitor->mSoAddr < addr + size)
        {
            if (aInfo->dlpi_name)
            {
                char *sopath = strdup(aInfo->dlpi_name);

                if ( ! sopath)
                    terminate(
                        errno,
                        "Unable to duplicate string '%s'", aInfo->dlpi_name);

                visitor->mSoPath = sopath;

                rc = 1;
            }

            goto Finally;
        }
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
findDlSymbol(const char *aSymName, uintptr_t *aSymAddr, const char **aErr)
{
    const char *rc = 0;

    if (aErr)
        *aErr = 0;

    /* PIC implementations resolve symbols to an intermediate thunk.
     * Repeatedly try to resolve the symbol to find the actual
     * implementation of the symbol. */

    void *symbol;

    {
        dlerror();
        void       *next = dlsym(RTLD_DEFAULT, aSymName);
        const char *err  = dlerror();

        if (err)
        {
            if (aErr)
                *aErr = err;
            goto Finally;
        }

        do
        {
            symbol = next;
            next   = dlsym(RTLD_NEXT, aSymName);
            err    = dlerror();
        } while ( ! err && symbol != next && next);
    }

    struct DlSymbolVisitor_ visitor =
    {
        .mSoAddr = (uintptr_t) symbol,
        .mSoPath = 0,
    };

    if (0 < dl_iterate_phdr(dlSymbolVisitor_, &visitor))
    {
        if (aSymAddr)
            *aSymAddr = visitor.mSoAddr;
        rc = visitor.mSoPath;
    }

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
