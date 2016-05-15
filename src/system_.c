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

#include "system_.h"
#include "fd_.h"
#include "error_.h"
#include "macros_.h"

#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>

/* -------------------------------------------------------------------------- */
static const char     *bootIncarnation_;
static int             bootIncarnationErr_;
static pthread_once_t  bootIncarnationOnce_ = PTHREAD_ONCE_INIT;

static void
fetchSystemIncarnation_(void)
{
    int   rc  = -1;
    int   fd  = -1;
    char *buf = 0;

    static const char procBootId[] = "/proc/sys/kernel/random/boot_id";

    ERROR_IF(
        (fd = open(procBootId, O_RDONLY),
         -1 == fd));

    ssize_t buflen;
    ERROR_IF(
        (buflen = readFdFully(fd, &buf, 64),
         -1 == buflen));
    ERROR_UNLESS(
        buflen,
        {
            errno = EINVAL;
        });

    char *end = memchr(buf, '\n', buflen);
    if (end)
        buflen = end - buf;
    else
        end = buf + buflen;

    char *bootIncarnation;
    ERROR_UNLESS(
        (bootIncarnation = malloc(buflen + 1)));

    memcpy(bootIncarnation, buf, buflen);
    bootIncarnation[buflen] = 0;

    bootIncarnation_ = bootIncarnation;

    rc = 0;

Finally:

    FINALLY
    ({
        fd = closeFd(fd);

        free(buf);
    });

    if (rc)
        bootIncarnationErr_ = errno;
}

const char *
fetchSystemIncarnation(void)
{
    ABORT_IF(
        (errno = pthread_once(&bootIncarnationOnce_, fetchSystemIncarnation_)),
        {
            terminate(
                errno,
                "Unable to fetch system incarnation");
        });

    if ( ! bootIncarnation_)
        errno = bootIncarnationErr_;

    return bootIncarnation_;
}

/* -------------------------------------------------------------------------- */
