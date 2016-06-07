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

#include "pidsignature_.h"

#include "error_.h"
#include "process_.h"
#include "system_.h"
#include "fd_.h"

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>

/* -------------------------------------------------------------------------- */
static CHECKED int
fetchProcessSignature_(struct Pid aPid, char **aSignature)
{
    int rc = -1;
    int fd = -1;

    char *buf       = 0;
    char *signature = 0;

    const char *incarnation;

    ERROR_UNLESS(
        (incarnation = fetchSystemIncarnation()));

    /* Note that it is expected that forkProcess() will guarantee that
     * the pid of the child process combined with its signature results
     * in a universally unique key. Because the pid is recycled over time
     * (as well as being reused after each reboot), the signature must
     * unambiguously qualify the pid. */

    do
    {
        struct ProcessDirName processDirName;

        initProcessDirName(&processDirName, aPid);

        static const char processStatFileNameFmt_[] = "%s/stat";

        char processStatFileName[strlen(processDirName.mDirName) +
                                 sizeof(processStatFileNameFmt_)];

        sprintf(processStatFileName,
                processStatFileNameFmt_, processDirName.mDirName);

        ERROR_IF(
            (fd = open(processStatFileName, O_RDONLY),
             -1 == fd));

    } while (0);

    ssize_t buflen;
    ERROR_IF(
        (buflen = readFdFully(fd, &buf, 0),
         -1 == buflen));
    ERROR_UNLESS(
        buflen,
        {
            errno = ERANGE;
        });

    char *bufend = buf + buflen;
    char *word;
    ERROR_UNLESS(
        (word = memrchr(buf, ')', buflen)),
        {
            errno = ERANGE;
        });

    for (unsigned ix = 2; 22 > ix; ++ix)
    {
        while (word != bufend && ! isspace((unsigned char) *word))
            ++word;

        ERROR_IF(
            word == bufend,
            {
                errno = ERANGE;
            });

        while (word != bufend && isspace((unsigned char) *word))
            ++word;
    }

    char *end = word;
    while (end != bufend && ! isspace((unsigned char) *end))
        ++end;

    do
    {
        char timestamp[end-word+1];
        memcpy(timestamp, word, end-word);
        timestamp[sizeof(timestamp)-1] = 0;

        static const char signatureFmt[] = "%s:%s";

        size_t signatureLen =
            strlen(incarnation) + sizeof(timestamp) + sizeof(signatureFmt);

        ERROR_UNLESS(
            (signature = malloc(signatureLen)));

        ERROR_IF(
            0 > sprintf(signature, signatureFmt, incarnation, timestamp));

    } while (0);

    *aSignature = signature;
    signature   = 0;

    rc = 0;

Finally:

    FINALLY
    ({
        fd = closeFd(fd);

        free(buf);
        free(signature);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct PidSignature *
createPidSignature(struct Pid aPid, const char *aSignature)
{
    int rc = -1;

    char *signature = 0;

    struct PidSignature *self = 0;
    ERROR_UNLESS(
        self = malloc(sizeof(*self)));

    self->mPid       = aPid;
    self->mSignature = 0;

    if (aSignature)
        ERROR_UNLESS(
            signature = strdup(aSignature));
    else if (self->mPid.mPid)
        ERROR_IF(
            fetchProcessSignature_(aPid, &signature));

    self->mSignature = signature;
    signature        = 0;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            self = destroyPidSignature(self);

        free(signature);
    });

    return self;
}

/* -------------------------------------------------------------------------- */
struct PidSignature *
destroyPidSignature(struct PidSignature *self)
{
    if (self)
    {
        free(self->mSignature);
        free(self);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
