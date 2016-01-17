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

#include "env_.h"
#include "parse_.h"
#include "macros_.h"

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>

/* -------------------------------------------------------------------------- */
int
deleteEnv(const char *aName)
{
    int rc = -1;

    if ( ! getenv(aName))
    {
        errno = ENOENT;
        goto Finally;
    }

    rc = unsetenv(aName);

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
getEnvString(const char *aName, const char **aValue)
{
    int rc = -1;

    *aValue = getenv(aName);

    if ( ! *aValue)
    {
        errno = ENOENT;
        goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
setEnvString(const char *aName, const char *aValue)
{
    const char *rc = 0;

    if (setenv(aName, aValue, 1))
        goto Finally;

    const char *env;

    if (getEnvString(aName, &env))
        goto Finally;

    rc = env;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
getEnvInt(const char *aName, int *aValue)
{
    int rc = -1;

    const char *env;

    if (getEnvString(aName, &env))
        goto Finally;

    rc = parseInt(env, aValue);

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
setEnvInt(const char *aName, int aValue)
{
    char value[sizeof("-") + sizeof(aValue) * CHAR_BIT];

    if (0 > sprintf(value, "%d", aValue))
        return 0;

    return setEnvString(aName, value);
}

/* -------------------------------------------------------------------------- */
int
getEnvUInt(const char *aName, unsigned *aValue)
{
    int rc = -1;

    const char *env;

    if (getEnvString(aName, &env))
        goto Finally;

    rc = parseUInt(env, aValue);

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
setEnvUInt(const char *aName, unsigned aValue)
{
    char value[sizeof(aValue) * CHAR_BIT];

    if (0 > sprintf(value, "%u", aValue))
        return 0;

    return setEnvString(aName, value);
}

/* -------------------------------------------------------------------------- */
int
getEnvUInt64(const char *aName, uint64_t *aValue)
{
    int rc = -1;

    const char *env;

    if (getEnvString(aName, &env))
        goto Finally;

    rc = parseUInt64(env, aValue);

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
setEnvUInt64(const char *aName, uint64_t aValue)
{
    char value[sizeof("-") + sizeof(aValue) * CHAR_BIT];

    if (0 > sprintf(value, "%" PRIu64, aValue))
        return 0;

    return setEnvString(aName, value);
}

/* -------------------------------------------------------------------------- */
int
getEnvPid(const char *aName, pid_t *aValue)
{
    int rc = -1;

    const char *env;

    if (getEnvString(aName, &env))
        goto Finally;

    rc = parsePid(env, aValue);

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
setEnvPid(const char *aName, pid_t aValue)
{
    char value[sizeof("-") + sizeof(aValue) * CHAR_BIT];

    if (0 > sprintf(value, "%jd", (intmax_t) aValue))
        return 0;

    return setEnvString(aName, value);
}

/* -------------------------------------------------------------------------- */
