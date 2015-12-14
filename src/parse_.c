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

#include "parse_.h"
#include "macros_.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

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
    if (parseLongLong_(aArg, &value))
        goto Finally;
    *aValue = value;

    if (*aValue - value)
    {
        errno = EINVAL;
        goto Finally;
    }

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
    if (parseUnsignedLongLong_(aArg, &value))
        goto Finally;
    *aValue = value;

    if (*aValue - value)
    {
        errno = EINVAL;
        goto Finally;
    }

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
    if (parseUnsignedLongLong_(aArg, &value))
        goto Finally;
    *aValue = value;

    if (*aValue - value)
    {
        errno = EINVAL;
        goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
parsePid(const char *aArg, pid_t *aValue)
{
    int rc = -1;

    unsigned long long value;
    if (parseUnsignedLongLong_(aArg, &value))
        goto Finally;
    *aValue = value;

    if (*aValue - value || 0 > *aValue)
    {
        errno = EINVAL;
        goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
