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
#include "test_.h"

#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <sys/syscall.h>

/* -------------------------------------------------------------------------- */
static unsigned sInit_;

static struct
{
    struct
    {
        unsigned          mLevel;
        struct ErrorFrame mFrame[64];
    } mStack_[ErrorFrameStackKinds], *mStack;

} __thread sErrorStack_;

/* -------------------------------------------------------------------------- */
void
pushErrorFrameLevel(const struct ErrorFrame *aFrame, int aErrno)
{
    if ( ! sErrorStack_.mStack)
        sErrorStack_.mStack = &sErrorStack_.mStack_[ErrorFrameStackThread];

    unsigned level = sErrorStack_.mStack->mLevel++;

    if (NUMBEROF(sErrorStack_.mStack->mFrame) <= level)
        abort();

    sErrorStack_.mStack->mFrame[level]        = *aFrame;
    sErrorStack_.mStack->mFrame[level].mErrno = aErrno;
}

/* -------------------------------------------------------------------------- */
void
resetErrorFrameLevel(void)
{
    if ( ! sErrorStack_.mStack)
        sErrorStack_.mStack = &sErrorStack_.mStack_[ErrorFrameStackThread];

    sErrorStack_.mStack->mLevel = 0;
}

/* -------------------------------------------------------------------------- */
enum ErrorFrameStackKind
switchErrorFrameStack(enum ErrorFrameStackKind aStack)
{
    if ( ! sErrorStack_.mStack)
        sErrorStack_.mStack = &sErrorStack_.mStack_[ErrorFrameStackThread];

    enum ErrorFrameStackKind stackKind =
        sErrorStack_.mStack - &sErrorStack_.mStack_[0];

    sErrorStack_.mStack = &sErrorStack_.mStack_[aStack];

    return stackKind;
}

/* -------------------------------------------------------------------------- */
unsigned
ownErrorFrameLevel(void)
{
    if ( ! sErrorStack_.mStack)
        sErrorStack_.mStack = &sErrorStack_.mStack_[ErrorFrameStackThread];

    return sErrorStack_.mStack->mLevel;
}

/* -------------------------------------------------------------------------- */
const struct ErrorFrame *
ownErrorFrame(enum ErrorFrameStackKind aStack, unsigned aLevel)
{
    if ( ! sErrorStack_.mStack)
        sErrorStack_.mStack = &sErrorStack_.mStack_[ErrorFrameStackThread];

    return
        (aLevel >= sErrorStack_.mStack->mLevel)
        ? 0
        : &sErrorStack_.mStack->mFrame[aLevel];
}

/* -------------------------------------------------------------------------- */
void
logErrorFrameWarning(void)
{
    if ( ! sErrorStack_.mStack)
        sErrorStack_.mStack = &sErrorStack_.mStack_[ErrorFrameStackThread];

    for (unsigned ix = 0; ix < sErrorStack_.mStack->mLevel; ++ix)
    {
        warn_(
            sErrorStack_.mStack->mFrame[ix].mErrno,
            sErrorStack_.mStack->mFrame[ix].mFile,
            sErrorStack_.mStack->mFrame[ix].mLine,
            "Error frame %u - %s - %s",
            sErrorStack_.mStack->mLevel - ix - 1,
            sErrorStack_.mStack->mFrame[ix].mName,
            sErrorStack_.mStack->mFrame[ix].mText);
    }
}

/* -------------------------------------------------------------------------- */
static pid_t
gettid_(void)
{
    return syscall(SYS_gettid);
}

/* -------------------------------------------------------------------------- */
static int
tryErrTextLength_(int aErrCode, size_t *aSize)
{
    int rc = -1;

    char errCodeText[*aSize];

    errno = 0;
    const char *errText =
        strerror_r(aErrCode, errCodeText, sizeof(errCodeText));
    if (errno)
        goto Finally;

    *aSize = errCodeText != errText ? 1 : strlen(errCodeText);

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

static size_t
findErrTextLength_(int aErrCode)
{
    size_t rc = 0;

    size_t textCapacity = testMode(0) ? 2 : 128;

    while (1)
    {
        size_t textSize = textCapacity;

        if (tryErrTextLength_(aErrCode, &textSize))
            goto Finally;

        if (textCapacity > textSize)
        {
            rc = textSize;
            break;
        }

        textSize = 2 * textCapacity;
        ensure(textCapacity < textSize);

        textCapacity = textSize;
    }

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
dprint_(
    int                    aLockErr,
    int                    aErrCode,
    const char            *aErrText,
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
    if ( ! aErrCode)
        dprintf(STDERR_FILENO, "\n");
    else if (aErrText)
        dprintf(STDERR_FILENO, " - errno %d [%s]\n", aErrCode, aErrText);
    else
        dprintf(STDERR_FILENO, " - errno %d\n", aErrCode);
}

static void
dprintf_(
    int                    aErrCode,
    const char            *aErrText,
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
            aErrCode, aErrText,
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
        intmax_t pid = getpid();
        intmax_t tid = gettid_();

        /* The availability of buffered IO might be lost while a message
         * is being processed since this code might run in a thread
         * that continues to execute while the process is being shut down. */

        int  lockerr;
        bool locked;
        bool buffered;

        if (acquireProcessAppLock())
        {
            lockerr  = errno ? errno : EPERM;
            locked   = false;
            buffered = false;
        }
        else
        {
            lockerr  = EWOULDBLOCK;
            locked   = true;
            buffered = !! sPrintBuf.mFile;
        }

        struct Duration elapsed = ownProcessElapsedTime();

        uint64_t elapsed_ms = MSECS(elapsed.duration).ms;
        uint64_t elapsed_s;
        uint64_t elapsed_m;
        uint64_t elapsed_h;

        elapsed_h  = elapsed_ms / (1000 * 60 * 60);
        elapsed_m  = elapsed_ms % (1000 * 60 * 60) / (1000 * 60);
        elapsed_s  = elapsed_ms % (1000 * 60 * 60) % (1000 * 60) / 1000;
        elapsed_ms = elapsed_ms % (1000 * 60 * 60) % (1000 * 60) % 1000;

        const char *errText    = "";
        size_t      errTextLen = 0;

        if (aErrCode)
            errTextLen = findErrTextLength_(aErrCode);

        char errTextBuffer[1+errTextLen];

        if (errTextLen)
        {
            /* Annoyingly strerror() is not thread safe, so is pretty
             * much unusable in any contemporary context since there
             * is no way to be absolutely sure that there is no other
             * thread attempting to use it. */

            errno = 0;
            const char *errTextMsg =
                strerror_r(aErrCode, errTextBuffer, sizeof(errTextBuffer));
            if ( ! errno)
                errText = errTextMsg;
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
                    aErrCode, errText,
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
            if ( ! aErrCode)
                fprintf(sPrintBuf.mFile, "\n");
            else if (errText)
                fprintf(
                    sPrintBuf.mFile, " - errno %d [%s]\n", aErrCode, errText);
            else
                fprintf(sPrintBuf.mFile, " - errno %d\n", aErrCode);
            fflush(sPrintBuf.mFile);

            writeFd(STDERR_FILENO, sPrintBuf.mBuf, sPrintBuf.mSize);
        }

        if (locked)
        {
            if (releaseProcessAppLock())
            {
                dprintf_(
                    errno, 0,
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

        if (gOptions.mDebug)
        {
            va_start(args, aFmt);
            print_(aErrCode, aFile, aLine, aFmt, args);
            va_end(args);
        }

        logErrorFrameWarning();

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

    if (1 == ++sInit_)
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

    if (0 == --sInit_)
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
