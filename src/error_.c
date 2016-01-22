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

#include "error_.h"
#include "macros_.h"
#include "timekeeping_.h"
#include "fd_.h"

#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/syscall.h>

/* -------------------------------------------------------------------------- */
static unsigned sInit;

/* -------------------------------------------------------------------------- */
static pid_t
gettid(void)
{
    return syscall(SYS_gettid);
}

/* -------------------------------------------------------------------------- */
static void
dprint_(
    int                    aLockErr,
    int                    aErrCode,
    intmax_t               aPid,
    intmax_t               aTid,
    const struct Duration *aElapsed,
    uint64_t               aElapsed_h,
    uint64_t               aElapsed_m,
    uint64_t               aElapsed_s,
    uint64_t               aElapsed_ms,
    const char            *aFile, unsigned aLine,
    const char            *aFmt, va_list aArgs)
{

    if ( ! aFile)
        dprintf(STDERR_FILENO, "%s: ", ownProcessName());
    else
    {
        if (aPid == aTid)
        {
            if (aElapsed->duration.ns)
                dprintf(
                    STDERR_FILENO,
                    "%s: [%04" PRIu64 ":%02" PRIu64
                    ":%02" PRIu64
                    ".%03" PRIu64 " %jd %s:%u] ",
                    ownProcessName(),
                    aElapsed_h, aElapsed_m, aElapsed_s, aElapsed_ms,
                    aPid, aFile, aLine);
            else
                dprintf(
                    STDERR_FILENO,
                    "%s: [%jd %s:%u] ",
                    ownProcessName(),
                    aPid, aFile, aLine);
        }
        else
        {
            if (aElapsed->duration.ns)
                dprintf(
                    STDERR_FILENO,
                    "%s: [%04" PRIu64 ":%02" PRIu64
                    ":%02" PRIu64
                    ".%03" PRIu64 " %jd:%jd %s:%u] ",
                    ownProcessName(),
                    aElapsed_h, aElapsed_m, aElapsed_s, aElapsed_ms,
                    aPid, aTid, aFile, aLine);
            else
                dprintf(
                    STDERR_FILENO,
                    "%s: [%jd:%jd %s:%u] ",
                    ownProcessName(),
                    aPid, aTid, aFile, aLine);
        }

        if (EWOULDBLOCK != aLockErr)
            dprintf(STDERR_FILENO, "- lock error %d - ", aLockErr);
    }

    vdprintf(STDERR_FILENO, aFmt, aArgs);
    if (aErrCode)
        dprintf(STDERR_FILENO, " - errno %d\n", aErrCode);
    else
        dprintf(STDERR_FILENO, "\n");
}

static void
dprintf_(
    int                    aErrCode,
    intmax_t               aPid,
    intmax_t               aTid,
    const struct Duration *aElapsed,
    uint64_t               aElapsed_h,
    uint64_t               aElapsed_m,
    uint64_t               aElapsed_s,
    uint64_t               aElapsed_ms,
    const char            *aFile, unsigned aLine,
    const char            *aFmt, ...)
{
    va_list args;

    va_start(args, aFmt);
    dprint_(EWOULDBLOCK,
            aErrCode,
            aPid, aTid,
            aElapsed, aElapsed_h, aElapsed_m, aElapsed_s, aElapsed_ms,
            aFile, aLine, aFmt, args);
    va_end(args);
}

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
        struct Duration elapsed = ownProcessElapsedTime();

        uint64_t elapsed_ms = MSECS(elapsed.duration).ms;
        uint64_t elapsed_s;
        uint64_t elapsed_m;
        uint64_t elapsed_h;

        elapsed_h  = elapsed_ms / (1000 * 60 * 60);
        elapsed_m  = elapsed_ms % (1000 * 60 * 60) / (1000 * 60);
        elapsed_s  = elapsed_ms % (1000 * 60 * 60) % (1000 * 60) / 1000;
        elapsed_ms = elapsed_ms % (1000 * 60 * 60) % (1000 * 60) % 1000;

        intmax_t pid = getpid();
        intmax_t tid = gettid();

        /* The availability of buffered IO might be lost while a message
         * is being processed since this code might run in a thread
         * that continues to execute while the process is being shut down. */

        int  lockerr;
        bool locked;
        bool buffered;

        if (acquireProcessAppLock())
        {
            lockerr  = errno;
            locked   = false;
            buffered = false;
        }
        else
        {
            lockerr  = EWOULDBLOCK;
            locked   = true;
            buffered = !! sPrintBuf.mFile;
        }

        if ( ! buffered)
        {
            /* Note that there is an old defect which causes dprintf()
             * to race with fork():
             *
             *    https://sourceware.org/bugzilla/show_bug.cgi?id=12847
             *
             * The symptom is that the child process will terminate with
             * SIGSEGV in fresetlockfiles(). */

            dprint_(lockerr,
                    aErrCode,
                    pid, tid,
                    &elapsed, elapsed_h, elapsed_m, elapsed_s, elapsed_ms,
                    aFile, aLine,
                    aFmt, aArgs);
        }
        else
        {
            rewind(sPrintBuf.mFile);

            if ( ! aFile)
                fprintf(sPrintBuf.mFile, "%s: ", ownProcessName());
            else
            {
                if (pid == tid)
                {
                    if (elapsed.duration.ns)
                        fprintf(
                            sPrintBuf.mFile,
                            "%s: [%04" PRIu64 ":%02" PRIu64
                            ":%02" PRIu64
                            ".%03" PRIu64 " %jd %s:%u] ",
                            ownProcessName(),
                            elapsed_h, elapsed_m, elapsed_s, elapsed_ms,
                            pid, aFile, aLine);
                    else
                        fprintf(
                            sPrintBuf.mFile,
                            "%s: [%jd %s:%u] ",
                            ownProcessName(),
                            pid, aFile, aLine);
                }
                else
                {
                    if (elapsed.duration.ns)
                        fprintf(
                            sPrintBuf.mFile,
                            "%s: [%04" PRIu64 ":%02" PRIu64
                            ":%02" PRIu64
                            ".%03" PRIu64 " %jd:%jd %s:%u] ",
                            ownProcessName(),
                            elapsed_h, elapsed_m, elapsed_s, elapsed_ms,
                            pid, tid, aFile, aLine);
                    else
                        fprintf(
                            sPrintBuf.mFile,
                            "%s: [%jd:%jd %s:%u] ",
                            ownProcessName(),
                            pid, tid, aFile, aLine);
                }
            }

            vfprintf(sPrintBuf.mFile, aFmt, aArgs);
            if (aErrCode)
                fprintf(sPrintBuf.mFile, " - errno %d\n", aErrCode);
            else
                fprintf(sPrintBuf.mFile, "\n");
            fflush(sPrintBuf.mFile);

            writeFd(STDERR_FILENO, sPrintBuf.mBuf, sPrintBuf.mSize);
        }

        if (locked)
        {
            if (releaseProcessAppLock())
            {
                dprintf_(
                    errno,
                    pid, tid,
                    &elapsed, elapsed_h, elapsed_m, elapsed_s, elapsed_ms,
                    __FILE__, __LINE__,
                    "Unable to release process lock");
                abort();
            }
        }
    });
}

/* -------------------------------------------------------------------------- */
void
ensure_(const char *aFile, unsigned aLine, ...)
{
    FINALLY
    ({
        va_list args;

        va_start(args, aLine);
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
        print_(aErrCode, 0, 0, aFmt, args);
        va_end(args);
        _exit(EXIT_FAILURE);
    });
}

/* -------------------------------------------------------------------------- */
int
Error_init(void)
{
    int rc = -1;

    struct ProcessAppLock *applock = 0;

    if (1 == ++sInit)
    {
        FILE *file = open_memstream(&sPrintBuf.mBuf, &sPrintBuf.mSize);

        if ( ! file)
            goto Finally;

        applock = createProcessAppLock();

        sPrintBuf.mFile = file;
    }

    rc = 0;

Finally:

    FINALLY
    ({
        destroyProcessAppLock(applock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
Error_exit(void)
{
    int rc = -1;

    struct ProcessAppLock *applock = 0;

    if (0 == --sInit)
    {
        applock = createProcessAppLock();

        if (fclose(sPrintBuf.mFile))
            goto Finally;

        free(sPrintBuf.mBuf);

        sPrintBuf.mFile = 0;
        sPrintBuf.mBuf  = 0;
        sPrintBuf.mSize = 0;
    }

    rc = 0;

Finally:

    FINALLY
    ({
        destroyProcessAppLock(applock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
