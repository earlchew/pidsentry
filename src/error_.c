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

#include "error_.h"
#include "macros_.h"
#include "process_.h"
#include "fd_.h"

#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
static struct {
    char  *mBuf;
    size_t mSize;
    FILE  *mFile;
} sPrintBuf;

static void
print_(
    int aErrCode,
    const char *aFile, unsigned aLine,
    const char *aFmt, va_list aArgs)
{
    FINALLY
    ({
        uint64_t elapsed   = ownProcessElapsedTime();
        uint64_t elapsed_s = elapsed / (1000 * 1000 * 1000);
        uint64_t elapsed_m;
        uint64_t elapsed_h;

        elapsed_h = elapsed_s / (60 * 60);
        elapsed_m = elapsed_s % (60 * 60) / 60;
        elapsed_s = elapsed_s % (60 * 60) % 60;

        if (lockProcessLock())
        {
            int err = errno;

            dprintf(
                STDERR_FILENO,
                "%s: [%03" PRIu64 ":%02" PRIu64 ":%02" PRIu64" %jd %s:%u] ",
                ownProcessName(),
                elapsed_h, elapsed_m, elapsed_s,
                (intmax_t) getpid(),
                aFile, aLine);

            if (EWOULDBLOCK != err)
                dprintf(STDERR_FILENO, "- lock error %d - ", err);

            vdprintf(STDERR_FILENO, aFmt, aArgs);
            if (aErrCode)
                dprintf(STDERR_FILENO, " - errno %d\n", aErrCode);
            else
                dprintf(STDERR_FILENO, "\n");
        }
        else
        {
            rewind(sPrintBuf.mFile);
            fprintf(
                sPrintBuf.mFile,
                "%s: [%03" PRIu64 ":%02" PRIu64 ":%02" PRIu64" %jd %s:%u] ",
                ownProcessName(),
                elapsed_h, elapsed_m, elapsed_s,
                (intmax_t) getpid(),
                aFile, aLine);
            vfprintf(sPrintBuf.mFile, aFmt, aArgs);
            if (aErrCode)
                fprintf(sPrintBuf.mFile, " - errno %d\n", aErrCode);
            else
                fprintf(sPrintBuf.mFile, "\n");
            fflush(sPrintBuf.mFile);

            writeFd(STDERR_FILENO, sPrintBuf.mBuf, sPrintBuf.mSize);

            unlockProcessLock();
        }
    });
}

/* -------------------------------------------------------------------------- */
void
ensure_(
    const char *aFile, unsigned aLine,
    const char *aFmt, ...)
{
    FINALLY
    ({
        va_list args;

        va_start(args, aFmt);
        print_(0, aFile, aLine, "Assertion failure - %s", args);
        va_end(args);

        while (1)
            abort();
    });
}

/* -------------------------------------------------------------------------- */
void
debug_(
    const char *aFile, unsigned aLine,
    const char *aFmt, ...)
{
    FINALLY
    ({
        va_list args;

        va_start(args, aFmt);
        print_(0, aFile, aLine, aFmt, args);
        va_end(args);
    });
}

/* -------------------------------------------------------------------------- */
void
warn_(
    int aErrCode,
    const char *aFile, unsigned aLine,
    const char *aFmt, ...)
{
    FINALLY
    ({
        va_list args;

        va_start(args, aFmt);
        print_(aErrCode, aFile, aLine, aFmt, args);
        va_end(args);
    });
}

/* -------------------------------------------------------------------------- */
void
terminate_(
    int aErrCode,
    const char *aFile, unsigned aLine,
    const char *aFmt, ...)
{
    FINALLY
    ({
        va_list args;

        va_start(args, aFmt);
        print_(aErrCode, aFile, aLine, aFmt, args);
        va_end(args);
        _exit(1);
    });
}

/* -------------------------------------------------------------------------- */
int
Error_init(void)
{
    int rc = -1;

    sPrintBuf.mFile = open_memstream(&sPrintBuf.mBuf, &sPrintBuf.mSize);

    if ( ! sPrintBuf.mFile)
        goto Finally;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
Error_exit(void)
{
    int rc = -1;

    if (fclose(sPrintBuf.mFile))
        goto Finally;

    free(sPrintBuf.mBuf);

    sPrintBuf.mFile = 0;
    sPrintBuf.mBuf  = 0;
    sPrintBuf.mSize = 0;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
