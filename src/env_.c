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
#include "error_.h"

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

/* -------------------------------------------------------------------------- */
int
deleteEnv(const char *aName)
{
    int rc = -1;

    ERROR_UNLESS(
        getenv(aName),
        {
            errno = ENOENT;
        });

    ERROR_IF(
        unsetenv(aName));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
getEnvString(const char *aName, const char **aValue)
{
    int rc = -1;

    ERROR_UNLESS(
        *aValue = getenv(aName),
        {
            errno = ENOENT;
        });

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

    ERROR_IF(
        setenv(aName, aValue, 1));

    const char *env;

    ERROR_IF(
        getEnvString(aName, &env));

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

    ERROR_IF(
        getEnvString(aName, &env));

    ERROR_IF(
        parseInt(env, aValue));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
setEnvInt(const char *aName, int aValue)
{
    const char *rc = 0;

    char value[sizeof("-") + sizeof(aValue) * CHAR_BIT];

    ERROR_IF(
        0 > sprintf(value, "%d", aValue));

    const char *env = 0;

    ERROR_UNLESS(
        (env = setEnvString(aName, value)));

    rc = env;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
getEnvUInt(const char *aName, unsigned *aValue)
{
    int rc = -1;

    const char *env;

    ERROR_IF(
        getEnvString(aName, &env));

    ERROR_IF(
        parseUInt(env, aValue));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
setEnvUInt(const char *aName, unsigned aValue)
{
    const char *rc = 0;

    char value[sizeof(aValue) * CHAR_BIT];

    ERROR_IF(
        0 > sprintf(value, "%u", aValue));

    const char *env = 0;

    ERROR_UNLESS(
        (env = setEnvString(aName, value)));

    rc = env;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
getEnvUInt64(const char *aName, uint64_t *aValue)
{
    int rc = -1;

    const char *env;

    ERROR_IF(
        getEnvString(aName, &env));

    ERROR_IF(
        parseUInt64(env, aValue));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
setEnvUInt64(const char *aName, uint64_t aValue)
{
    const char *rc = 0;

    char value[sizeof("-") + sizeof(aValue) * CHAR_BIT];

    ERROR_IF(
        0 > sprintf(value, "%" PRIu64, aValue));

    const char *env = 0;

    ERROR_UNLESS(
        (env = setEnvString(aName, value)));

    rc = env;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
int
getEnvPid(const char *aName, pid_t *aValue)
{
    int rc = -1;

    const char *env;

    ERROR_IF(
        getEnvString(aName, &env));

    ERROR_IF(
        parsePid(env, aValue));

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
setEnvPid(const char *aName, pid_t aValue)
{
    const char *rc = 0;

    char value[sizeof("-") + sizeof(aValue) * CHAR_BIT];

    ERROR_IF(
        0 > sprintf(value, "%jd", (intmax_t) aValue));

    const char *env = 0;

    ERROR_UNLESS(
        (env = setEnvString(aName, value)));

    rc = env;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
