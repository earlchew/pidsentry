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
#include "thread_.h"

#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
static unsigned moduleInit_;

static struct ErrorUnwindFrame_ __thread errorUnwind_;

static struct
{
    /* Carefully crafted so that new threads will see an initialised
     * instance that can be used immediately. In particular, note that
     * threads cannot be created from signal context, so ErrorFrameStackThread
     * must be zero. */

    struct
    {
        unsigned          mLevel;
        unsigned          mSequence;
        struct ErrorFrame mFrame[64];
    } mStack_[ErrorFrameStackKinds], *mStack;

} __thread errorStack_;

static void
initErrorFrame_(void)
{
    ensure(0 == ErrorFrameStackThread);

    if ( ! errorStack_.mStack)
        errorStack_.mStack = &errorStack_.mStack_[ErrorFrameStackThread];
}

/* -------------------------------------------------------------------------- */
void
addErrorFrame(const struct ErrorFrame *aFrame, int aErrno)
{
    initErrorFrame_();

    unsigned level = errorStack_.mStack->mLevel++;

    if (NUMBEROF(errorStack_.mStack->mFrame) <= level)
        abort();

    errorStack_.mStack->mFrame[level]        = *aFrame;
    errorStack_.mStack->mFrame[level].mErrno = aErrno;
}

/* -------------------------------------------------------------------------- */
void
restartErrorFrameSequence(void)
{
    initErrorFrame_();

    errorStack_.mStack->mLevel = errorStack_.mStack->mSequence;
}

/* -------------------------------------------------------------------------- */
struct ErrorFrameSequence
pushErrorFrameSequence(void)
{
    initErrorFrame_();

    unsigned sequence = errorStack_.mStack->mSequence;

    errorStack_.mStack->mSequence = errorStack_.mStack->mLevel;

    return (struct ErrorFrameSequence)
    {
        .mSequence = sequence,
    };
}

/* -------------------------------------------------------------------------- */
void
popErrorFrameSequence(struct ErrorFrameSequence aSequence)
{
    restartErrorFrameSequence();

    errorStack_.mStack->mSequence = aSequence.mSequence;
}

/* -------------------------------------------------------------------------- */
enum ErrorFrameStackKind
switchErrorFrameStack(enum ErrorFrameStackKind aStack)
{
    initErrorFrame_();

    enum ErrorFrameStackKind stackKind =
        errorStack_.mStack - &errorStack_.mStack_[0];

    errorStack_.mStack = &errorStack_.mStack_[aStack];

    return stackKind;
}

/* -------------------------------------------------------------------------- */
unsigned
ownErrorFrameLevel(void)
{
    initErrorFrame_();

    return errorStack_.mStack->mLevel;
}

/* -------------------------------------------------------------------------- */
const struct ErrorFrame *
ownErrorFrame(enum ErrorFrameStackKind aStack, unsigned aLevel)
{
    initErrorFrame_();

    return
        (aLevel >= errorStack_.mStack->mLevel)
        ? 0
        : &errorStack_.mStack->mFrame[aLevel];
}

/* -------------------------------------------------------------------------- */
void
logErrorFrameSequence(void)
{
    initErrorFrame_();

    unsigned seqLen =
        errorStack_.mStack->mLevel - errorStack_.mStack->mSequence;

    for (unsigned ix = 0; ix < seqLen; ++ix)
    {
        unsigned frame = errorStack_.mStack->mSequence + ix;

        warn_(
            errorStack_.mStack->mFrame[frame].mErrno,
            errorStack_.mStack->mFrame[frame].mName,
            errorStack_.mStack->mFrame[frame].mFile,
            errorStack_.mStack->mFrame[frame].mLine,
            "Error frame %u - %s",
            seqLen - ix - 1,
            errorStack_.mStack->mFrame[frame].mText);
    }
}

/* -------------------------------------------------------------------------- */
struct ErrorUnwindFrame_ *
pushErrorUnwindFrame_(void)
{
    struct ErrorUnwindFrame_ *self = &errorUnwind_;

    ++self->mActive;

    return self;
}

/* -------------------------------------------------------------------------- */
void
popErrorUnwindFrame_(struct ErrorUnwindFrame_ *self)
{
    ensure(self->mActive);

    --self->mActive;
}

/* -------------------------------------------------------------------------- */
struct ErrorUnwindFrame_ *
ownErrorUnwindActiveFrame_(void)
{
    struct ErrorUnwindFrame_ *self = &errorUnwind_;

    return self->mActive ? self : 0;
}

/* -------------------------------------------------------------------------- */
static int
tryErrTextLength_(int aErrCode, size_t *aSize)
{
    int rc = -1;

    char errCodeText[*aSize];

    const char *errText;
    ERROR_IF(
        (errno = 0,
         errText = strerror_r(aErrCode, errCodeText, sizeof(errCodeText)),
         errno));

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

    size_t textCapacity = testMode(TestLevelRace) ? 2 : 128;

    while (1)
    {
        size_t textSize = textCapacity;

        ERROR_IF(
            tryErrTextLength_(aErrCode, &textSize));

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
    struct Pid             aPid,
    struct Tid             aTid,
    const struct Duration *aElapsed,
    uint64_t               aElapsed_h,
    uint64_t               aElapsed_m,
    uint64_t               aElapsed_s,
    uint64_t               aElapsed_ms,
    const char            *aFunction,
    const char            *aFile,
    unsigned               aLine,
    const char            *aFmt, va_list aArgs)
{

    if ( ! aFile)
        dprintf(STDERR_FILENO, "%s: ", ownProcessName());
    else
    {
        if (aPid.mPid == aTid.mTid)
        {
            if (aElapsed->duration.ns)
                dprintf(
                    STDERR_FILENO,
                    "%s: [%04" PRIu64 ":%02" PRIu64
                    ":%02" PRIu64
                    ".%03" PRIu64 " %" PRId_Pid " %s %s:%u] ",
                    ownProcessName(),
                    aElapsed_h, aElapsed_m, aElapsed_s, aElapsed_ms,
                    FMTd_Pid(aPid), aFunction, aFile, aLine);
            else
                dprintf(
                    STDERR_FILENO,
                    "%s: [%" PRId_Pid " %s %s:%u] ",
                    ownProcessName(),
                    FMTd_Pid(aPid), aFunction, aFile, aLine);
        }
        else
        {
            if (aElapsed->duration.ns)
                dprintf(
                    STDERR_FILENO,
                    "%s: [%04" PRIu64 ":%02" PRIu64
                    ":%02" PRIu64
                    ".%03" PRIu64 " %" PRId_Pid ":%" PRId_Tid " %s %s:%u] ",
                    ownProcessName(),
                    aElapsed_h, aElapsed_m, aElapsed_s, aElapsed_ms,
                    FMTd_Pid(aPid), FMTd_Tid(aTid), aFunction, aFile, aLine);
            else
                dprintf(
                    STDERR_FILENO,
                    "%s: [%" PRId_Pid ":%" PRId_Tid " %s %s:%u] ",
                    ownProcessName(),
                    FMTd_Pid(aPid), FMTd_Tid(aTid), aFunction, aFile, aLine);
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
    struct Pid             aPid,
    struct Tid             aTid,
    const struct Duration *aElapsed,
    uint64_t               aElapsed_h,
    uint64_t               aElapsed_m,
    uint64_t               aElapsed_s,
    uint64_t               aElapsed_ms,
    const char            *aFunction,
    const char            *aFile,
    unsigned               aLine,
    const char            *aFmt, ...)
{
    va_list args;

    va_start(args, aFmt);
    dprint_(EWOULDBLOCK,
            aErrCode, aErrText,
            aPid, aTid,
            aElapsed, aElapsed_h, aElapsed_m, aElapsed_s, aElapsed_ms,
            aFunction, aFile, aLine, aFmt, args);
    va_end(args);
}

/* -------------------------------------------------------------------------- */
static struct {
    char  *mBuf;
    size_t mSize;
    FILE  *mFile;
} printBuf_;

static void
print_(
    int aErrCode,
    const char *aFunction, const char *aFile, unsigned aLine,
    const char *aFmt, va_list aArgs)
{
    FINALLY
    ({
        struct Pid pid = ownProcessId();
        struct Tid tid = ownThreadId();

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
            buffered = !! printBuf_.mFile;
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
                    aFunction, aFile, aLine,
                    aFmt, aArgs);
        }
        else
        {
            rewind(printBuf_.mFile);

            if ( ! aFile)
                fprintf(printBuf_.mFile, "%s: ", ownProcessName());
            else
            {
                if (pid.mPid == tid.mTid)
                {
                    if (elapsed.duration.ns)
                        fprintf(
                            printBuf_.mFile,
                            "%s: [%04" PRIu64 ":%02" PRIu64
                            ":%02" PRIu64
                            ".%03" PRIu64 " %" PRId_Pid " %s %s:%u] ",
                            ownProcessName(),
                            elapsed_h, elapsed_m, elapsed_s, elapsed_ms,
                            FMTd_Pid(pid), aFunction, aFile, aLine);
                    else
                        fprintf(
                            printBuf_.mFile,
                            "%s: [%" PRId_Pid " %s %s:%u] ",
                            ownProcessName(),
                            FMTd_Pid(pid), aFunction, aFile, aLine);
                }
                else
                {
                    if (elapsed.duration.ns)
                        fprintf(
                            printBuf_.mFile,
                            "%s: [%04" PRIu64 ":%02" PRIu64
                            ":%02" PRIu64
                            ".%03" PRIu64 " %" PRId_Pid ":%" PRId_Tid " "
                            "%s %s:%u] ",
                            ownProcessName(),
                            elapsed_h, elapsed_m, elapsed_s, elapsed_ms,
                            FMTd_Pid(pid), FMTd_Tid(tid),
                            aFunction, aFile, aLine);
                    else
                        fprintf(
                            printBuf_.mFile,
                            "%s: [%" PRId_Pid ":%" PRId_Tid " %s %s:%u] ",
                            ownProcessName(),
                            FMTd_Pid(pid), FMTd_Tid(tid),
                            aFunction, aFile, aLine);
                }
            }

            vfprintf(printBuf_.mFile, aFmt, aArgs);
            if ( ! aErrCode)
                fprintf(printBuf_.mFile, "\n");
            else if (errText)
                fprintf(
                    printBuf_.mFile, " - errno %d [%s]\n", aErrCode, errText);
            else
                fprintf(printBuf_.mFile, " - errno %d\n", aErrCode);
            fflush(printBuf_.mFile);

            writeFd(STDERR_FILENO, printBuf_.mBuf, printBuf_.mSize);
        }

        if (locked)
        {
            if (releaseProcessAppLock())
            {
                dprintf_(
                    errno, 0,
                    pid, tid,
                    &elapsed, elapsed_h, elapsed_m, elapsed_s, elapsed_ms,
                    __func__, __FILE__, __LINE__,
                    "Unable to release process lock");
                abort();
            }
        }
    });
}

/* -------------------------------------------------------------------------- */
void
ensure_(const char *aFunction, const char *aFile, unsigned aLine, ...)
{
    FINALLY
    ({
        va_list args;

        va_start(args, aLine);
        print_(0, aFunction, aFile, aLine, "Assertion failure - %s", args);
        va_end(args);

        while (1)
            abort();
    });
}

/* -------------------------------------------------------------------------- */
void
debug_(
    const char *aFunction, const char *aFile, unsigned aLine,
    const char *aFmt, ...)
{
    FINALLY
    ({
        va_list args;

        struct ErrorFrameSequence frameSequence =
            pushErrorFrameSequence();

        va_start(args, aFmt);
        print_(0, aFunction, aFile, aLine, aFmt, args);
        va_end(args);

        popErrorFrameSequence(frameSequence);
    });
}

/* -------------------------------------------------------------------------- */
void
warn_(
    int aErrCode,
    const char *aFunction, const char *aFile, unsigned aLine,
    const char *aFmt, ...)
{
    FINALLY
    ({
        struct ErrorFrameSequence frameSequence =
            pushErrorFrameSequence();

        va_list args;

        va_start(args, aFmt);
        print_(aErrCode, aFunction, aFile, aLine, aFmt, args);
        va_end(args);

        popErrorFrameSequence(frameSequence);
    });

    struct ErrorUnwindFrame_ *unwindFrame = ownErrorUnwindActiveFrame_();

    if (unwindFrame)
        longjmp(unwindFrame->mJmpBuf, 1);
}

/* -------------------------------------------------------------------------- */
void
message_(
    int aErrCode,
    const char *aFunction, const char *aFile, unsigned aLine,
    const char *aFmt, ...)
{
    FINALLY
    ({
        va_list args;

        struct ErrorFrameSequence frameSequence =
            pushErrorFrameSequence();

        va_start(args, aFmt);
        print_(aErrCode, 0, 0, 0, aFmt, args);
        va_end(args);

        popErrorFrameSequence(frameSequence);
    });
}

/* -------------------------------------------------------------------------- */
void
terminate_(
    int aErrCode,
    const char *aFunction, const char *aFile, unsigned aLine,
    const char *aFmt, ...)
{
    FINALLY
    ({
        struct ErrorFrameSequence frameSequence =
            pushErrorFrameSequence();

        va_list args;

        if (gOptions.mDebug)
        {
            va_start(args, aFmt);
            print_(aErrCode, aFunction, aFile, aLine, aFmt, args);
            va_end(args);
        }

        popErrorFrameSequence(frameSequence);

        va_start(args, aFmt);
        print_(aErrCode, 0, 0, 0, aFmt, args);
        va_end(args);

        abortProcess();
    });
}

/* -------------------------------------------------------------------------- */
int
Error_init(void)
{
    int rc = -1;

    struct ProcessAppLock *appLock = 0;

    if (1 == ++moduleInit_)
    {
        FILE *file;
        ERROR_UNLESS(
            (file = open_memstream(&printBuf_.mBuf, &printBuf_.mSize)));

        appLock = createProcessAppLock();

        printBuf_.mFile = file;
    }

    rc = 0;

Finally:

    FINALLY
    ({
        destroyProcessAppLock(appLock);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
void
Error_exit(void)
{
    if (0 == --moduleInit_)
    {
        struct ProcessAppLock *appLock = createProcessAppLock();

        FILE *file = printBuf_.mFile;

        printBuf_.mFile = 0;
        printBuf_.mBuf  = 0;
        printBuf_.mSize = 0;

        ABORT_IF(fclose(file));

        free(printBuf_.mBuf);

        destroyProcessAppLock(appLock);
    }
}

/* -------------------------------------------------------------------------- */
