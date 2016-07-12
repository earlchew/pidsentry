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
#ifndef EINTR_H
#define EINTR_H

#include "compiler_.h"

#ifdef EINTR_MODULE_DEFN_
#define accept   accept_
#define accept4  accept4_
#define close    close_
#define connect  connect_
#define ioctl    ioctl_
#define open     open_
#define pread    pread_
#define pwrite   pwrite_
#define preadv   preadv_
#define pwritev  pwritev_
#define read     read_
#define readv    readv_
#define recv     recv_
#define recvfrom recvfrom_
#define recvmsg  recvmsg_
#define send     send_
#define sendto   sendto_
#define wait     wait_
#define wait3    wait3_
#define wait4    wait4_
#define waitid   waitid_
#define waitpid  waitpid_
#define write    write_
#define writev   writev_
#endif

#include <unistd.h>

#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/socket.h>

#ifdef EINTR_MODULE_DEFN_
#undef accept
#undef accept4
#undef close
#undef connect
#undef ioctl
#undef open
#undef pread
#undef preadv
#undef pwrite
#undef pwritev
#undef read
#undef readv
#undef recv
#undef recvfrom
#undef recvmsg
#undef send
#undef sendto
#undef wait
#undef wait3
#undef wait4
#undef waitid
#undef waitpid
#undef write
#undef writev
#endif

#include <stdbool.h>

/* -------------------------------------------------------------------------- */
struct EintrModule
{
    struct EintrModule *mModule;
};

/* -------------------------------------------------------------------------- */
#ifndef EINTR_MODULE_DEFN_
#define EINTR_FUNCTION_DECL_(Return_, Name_, Signature_) \
    Return_ Name_ ## _eintr Signature_;                  \
    struct EintrModule
#else
#define EINTR_FUNCTION_DECL_(Return_, Name_, Signature_) \
    Return_ Name_           Signature_;                  \
    Return_ Name_ ## _eintr Signature_;                  \
    struct EintrModule
#endif

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DECL_(
    int, accept, (int aFd, struct sockaddr *aAddr, socklen_t *aAddrLen));

EINTR_FUNCTION_DECL_(
    int, accept4,
    (int aFd, struct sockaddr *aAddr, socklen_t *aAddLen, int aOptions));

EINTR_FUNCTION_DECL_(
    int, close, (int aFd));

EINTR_FUNCTION_DECL_(
    int, connect, (int aFd, const struct sockaddr *aAddr, socklen_t aAddrLen));

EINTR_FUNCTION_DECL_(
    int, open, (const char *aPath, int aFlags, ...));

EINTR_FUNCTION_DECL_(
    ssize_t, recv, (int aFd, void *aBufPtr, size_t aBufLen, int aFlags));

EINTR_FUNCTION_DECL_(
    ssize_t, recvfrom, (int aFd, void *aBufPtr, size_t aBufLen, int aFlags,
                        struct sockaddr *aAddr, socklen_t *aAddrLen));

EINTR_FUNCTION_DECL_(
    ssize_t, recvmsg, (int aFd, struct msghdr *aMsg, int aFlags));

EINTR_FUNCTION_DECL_(
    ssize_t, send, (int aFd, const void *aBufPtr, size_t aBufLen, int aFlags));

EINTR_FUNCTION_DECL_(
    ssize_t, sendto, (int aFd, const void *aBufPtr, size_t aBufLen, int aFlags,
                      const struct sockaddr *aAddr, socklen_t aAddrLen));

EINTR_FUNCTION_DECL_(
    pid_t, wait, (int *aStatus));

EINTR_FUNCTION_DECL_(
    pid_t, wait3, (int *aStatus, int aOptions, struct rusage *aRusage));

EINTR_FUNCTION_DECL_(
    pid_t, wait4,
    (pid_t aPid, int *aStatus, int aOptions, struct rusage *aRusage));

EINTR_FUNCTION_DECL_(
    int, waitid,
    (idtype_t aIdType, id_t aId, siginfo_t *aInfo, int aOptions));

EINTR_FUNCTION_DECL_(
    pid_t, waitpid,
    (pid_t aPid, int *aStatus, int aOptions));

/* -------------------------------------------------------------------------- */
#ifndef __GLIBC__
#define EINTR_IOCTL_REQUEST_T_ int
#else
/* Unfortunately the header file does not match the documentation:
 *
 *     https://sourceware.org/bugzilla/show_bug.cgi?id=14362
 *
 * It does not seem that this will be fixed any time soon. */

#define EINTR_IOCTL_REQUEST_T_ unsigned long int
#endif

EINTR_FUNCTION_DECL_(
    int, ioctl,
    (int aFd, EINTR_IOCTL_REQUEST_T_ aRequest, ...));

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DECL_(
    ssize_t, read,
    (int aFd, void *aBuf, size_t aCount));

EINTR_FUNCTION_DECL_(
    ssize_t, write,
    (int aFd, const void *aBuf, size_t aCount));

EINTR_FUNCTION_DECL_(
    ssize_t, pread,
    (int aFd, void *aBuf, size_t aCount, off_t aOffset));

EINTR_FUNCTION_DECL_(
    ssize_t, pwrite,
    (int aFd, const void *aBuf, size_t aCount, off_t aOffset));

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DECL_(
    ssize_t, readv,
    (int aFd, const struct iovec *aVec, int aCount));

EINTR_FUNCTION_DECL_(
    ssize_t, writev,
    (int aFd, const struct iovec *aVec, int aCount));

EINTR_FUNCTION_DECL_(
    ssize_t, preadv,
    (int aFd, const struct iovec *aVec, int aCount, off_t aOffset));

EINTR_FUNCTION_DECL_(
    ssize_t, pwritev,
    (int aFd, const struct iovec *aVec, int aCount, off_t aOffset));

/* -------------------------------------------------------------------------- */
CHECKED int
Eintr_init(struct EintrModule *self);

CHECKED struct EintrModule *
Eintr_exit(struct EintrModule *self);

bool
Eintr_active(void);

/* -------------------------------------------------------------------------- */

#endif /* EINTR_H */
