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

#include "parse_.h"
#include "macros_.h"
#include "error_.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* -------------------------------------------------------------------------- */
static int
parseUnsignedLongLong_(const char *aArg, unsigned long long *aValue)
{
    int rc = -1;

    if (isdigit((unsigned char) *aArg))
    {
        char *endptr = 0;

        errno   = 0;
        *aValue = strtoull(aArg, &endptr, 10);

        if ( ! *endptr && (ULLONG_MAX != *aValue || ERANGE != errno))
            rc = 0;
    }

    if (rc)
        errno = EINVAL;

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
parseLongLong_(const char *aArg, long long *aValue)
{
    int rc = -1;

    if (isdigit((unsigned char) *aArg) || *aArg == '-' || *aArg == '+')
    {
        char *endptr = 0;

        errno   = 0;
        *aValue = strtoll(aArg, &endptr, 10);

        if ( ! *endptr && (LLONG_MAX != *aValue || ERANGE != errno))
            rc = 0;
    }

    if (rc)
        errno = EINVAL;

    return rc;
}

/* -------------------------------------------------------------------------- */
int
parseInt(const char *aArg, int *aValue)
{
    int rc = -1;

    long long value;
    ERROR_IF(
        parseLongLong_(aArg, &value));
    *aValue = value;

    ERROR_IF(
        *aValue - value,
        {
            errno = EINVAL;
        });

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
parseUInt(const char *aArg, unsigned *aValue)
{
    int rc = -1;

    unsigned long long value;
    ERROR_IF(
        parseUnsignedLongLong_(aArg, &value));
    *aValue = value;

    ERROR_IF(
        *aValue - value,
        {
            errno = EINVAL;
        });

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
parseUInt64(const char *aArg, uint64_t *aValue)
{
    int rc = -1;

    unsigned long long value;
    ERROR_IF(
        parseUnsignedLongLong_(aArg, &value));
    *aValue = value;

    ERROR_IF(
        *aValue - value,
        {
            errno = EINVAL;
        });

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
parsePid(const char *aArg, struct Pid *aValue)
{
    int rc = -1;

    unsigned long long value;
    ERROR_IF(
        parseUnsignedLongLong_(aArg, &value));
    *aValue = Pid(value);

    ERROR_IF(
        aValue->mPid - value || 0 > aValue->mPid,
        {
            errno = EINVAL;
        });

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
createParseArgListCSV(struct ParseArgList *self, const char *aArg)
{
    int rc = -1;

    self->mArgs = 0;
    self->mArgv = 0;
    self->mArgc = 0;

    if (aArg)
    {
        /* Count the number of separators to determine the number of
         * words in the list. There is some ambiguity in the case that there
         * is only one word, but that will be fixed up at the end. */

        size_t wordcount = 1;
        for (size_t ix = 0; aArg[ix]; ++ix)
        {
            if (',' == aArg[ix])
                ++wordcount;
        }

        ERROR_UNLESS(
            (self->mArgs = strdup(aArg)));

        ERROR_UNLESS(
            (self->mArgv = malloc(sizeof(*self->mArgv) * (wordcount + 1))));

        for (char *chptr = self->mArgs; ; )
        {
            while (*chptr && isspace((unsigned char) *chptr))
                ++chptr;

            char *headptr = chptr;

            self->mArgv[self->mArgc++] = headptr;

            char *tailptr = 0;

            while (*chptr)
            {
                if (',' == *chptr)
                {
                    tailptr  = chptr;
                    *chptr++ = 0;
                    break;
                }
                ++chptr;
            }

            if ( ! tailptr)
                tailptr = chptr;

            ensure( ! *tailptr);

            while (tailptr != headptr)
            {
                if ( ! isspace((unsigned char) tailptr[-1]))
                    break;
                --tailptr;
            }

            *tailptr = 0;

            if (self->mArgc == wordcount)
            {
                ensure( ! *chptr);
                break;
            }
        }

        ensure(self->mArgc == wordcount);

        self->mArgv[self->mArgc] = 0;
    }

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            free(self->mArgs);
            free(self->mArgv);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct ParseArgList *
closeParseArgList(struct ParseArgList *self)
{
    if (self)
    {
        free(self->mArgs);
        free(self->mArgv);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
