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
#define accept          accept_
#define accept4         accept4_
#define close           close_
#define connect         connect_
#define epoll_pwait     epoll_pwait_
#define epoll_wait      epoll_wait_
#define fcntl           fcntl_
#define flock           flock_
#define ioctl           ioctl_
#define mq_receive      mq_receive_
#define mq_send         mq_send_
#define mq_timedreceive mq_timedreceive_
#define mq_timedsend    mq_timedsend_
#define msgrcv          msgrcv_
#define msgsnd          msgsnd_
#define open            open_
#define pause           pause_
#define poll            poll_
#define ppoll           ppoll_
#define pread           pread_
#define pwrite          pwrite_
#define preadv          preadv_
#define pselect         pselect_
#define pwritev         pwritev_
#define read            read_
#define readv           readv_
#define recv            recv_
#define recvfrom        recvfrom_
#define recvmsg         recvmsg_
#define select          select_
#define sem_wait        sem_wait_
#define sem_timedwait   sem_timedwait_
#define semop           semop_
#define semtimedop      semtimedop_
#define send            send_
#define sendto          sendto_
#define sendmsg         sendmsg_
#define sigsuspend      sigsuspend_
#define sigtimedwait    sigtimedwait_
#define sigwaitinfo     sigwaitinfo_
#define wait            wait_
#define wait3           wait3_
#define wait4           wait4_
#define waitid          waitid_
#define waitpid         waitpid_
#define write           write_
#define writev          writev_
#endif

#include <fcntl.h>
#include <mqueue.h>
#include <poll.h>
#include <semaphore.h>
#include <signal.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/msg.h>
#include <sys/select.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>

#ifdef EINTR_MODULE_DEFN_
#undef accept
#undef accept4
#undef close
#undef connect
#undef epoll_pwait
#undef epoll_wait
#undef fcntl
#undef flock
#undef ioctl
#undef mq_receive
#undef mq_send
#undef mq_timedreceive
#undef mq_timedsend
#undef msgrcv
#undef msgsnd
#undef open
#undef pause
#undef poll
#undef ppoll
#undef pread
#undef preadv
#undef pselect
#undef pwrite
#undef pwritev
#undef read
#undef readv
#undef recv
#undef recvfrom
#undef recvmsg
#undef select
#undef sem_wait
#undef sem_timedwait
#undef semop
#undef semtimedop
#undef send
#undef sendto
#undef sendmsg
#undef sigsuspend
#undef sigtimedwait
#undef sigwaitinfo
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
    int, epoll_pwait, (int aFd, struct epoll_event *aEvents,
                       int aMaxEvents, int aTimeout, const sigset_t *aMask));

EINTR_FUNCTION_DECL_(
    int, epoll_wait, (int aFd, struct epoll_event *aEvents,
                      int aMaxEvents, int aTimeout));

EINTR_FUNCTION_DECL_(
    int, fcntl, (int aFd, int aCmd, ...));

EINTR_FUNCTION_DECL_(
    int, flock, (int aFd, int aOp));

EINTR_FUNCTION_DECL_(
    int, ioctl,
    (int aFd, EINTR_IOCTL_REQUEST_T_ aRequest, ...));

EINTR_FUNCTION_DECL_(
    ssize_t, mq_receive,
    (mqd_t aMq, char *aMsgPtr, size_t aMsgLen, unsigned *aPriority));

EINTR_FUNCTION_DECL_(
    int, mq_send,
    (mqd_t aMq, const char *aMsgPtr, size_t aMsgLen, unsigned aPriority));

EINTR_FUNCTION_DECL_(
    ssize_t, mq_timedreceive,
    (mqd_t aMq, char *aMsgPtr, size_t aMsgLen, unsigned *aPriority,
     const struct timespec *aTimeout));

EINTR_FUNCTION_DECL_(
    int, mq_timedsend,
    (mqd_t aMq, const char *aMsgPtr, size_t aMsgLen, unsigned aPriority,
     const struct timespec *aTimeout));

EINTR_FUNCTION_DECL_(
    ssize_t, msgrcv, (
        int aMsgId, void *aMsg, size_t aSize, long aType, int aFlag));

EINTR_FUNCTION_DECL_(
    int, msgsnd, (int aMsgId, const void *aMsg, size_t aSize, int aFlag));

EINTR_FUNCTION_DECL_(
    int, open, (const char *aPath, int aFlags, ...));

EINTR_FUNCTION_DECL_(
    int, pause, (void));

EINTR_FUNCTION_DECL_(
    int, poll, (struct pollfd *aFds, nfds_t aNumFds, int aTimeout));

EINTR_FUNCTION_DECL_(
    int, ppoll, (struct pollfd *aFds, nfds_t aNumFds,
                 const struct timespec *aTimeout, const sigset_t *aMask));

EINTR_FUNCTION_DECL_(
    ssize_t, pread,
    (int aFd, void *aBuf, size_t aCount, off_t aOffset));

EINTR_FUNCTION_DECL_(
    ssize_t, preadv,
    (int aFd, const struct iovec *aVec, int aCount, off_t aOffset));

EINTR_FUNCTION_DECL_(
    int, pselect, (int aNumFds, fd_set *aRdFds, fd_set *aWrFds, fd_set *aExFds,
                   struct timeval *aTimeout, const sigset_t *aMask));

EINTR_FUNCTION_DECL_(
    ssize_t, pwrite,
    (int aFd, const void *aBuf, size_t aCount, off_t aOffset));

EINTR_FUNCTION_DECL_(
    ssize_t, pwritev,
    (int aFd, const struct iovec *aVec, int aCount, off_t aOffset));

EINTR_FUNCTION_DECL_(
    ssize_t, read,
    (int aFd, void *aBuf, size_t aCount));

EINTR_FUNCTION_DECL_(
    ssize_t, readv,
    (int aFd, const struct iovec *aVec, int aCount));

EINTR_FUNCTION_DECL_(
    ssize_t, recv, (int aFd, void *aBufPtr, size_t aBufLen, int aFlags));

EINTR_FUNCTION_DECL_(
    ssize_t, recvfrom, (int aFd, void *aBufPtr, size_t aBufLen, int aFlags,
                        struct sockaddr *aAddr, socklen_t *aAddrLen));

EINTR_FUNCTION_DECL_(
    ssize_t, recvmsg, (int aFd, struct msghdr *aMsg, int aFlags));

EINTR_FUNCTION_DECL_(
    int, select, (int aNumFds, fd_set *aRdFds, fd_set *aWrFds,
                  fd_set *aExFds, struct timeval *aTimeout));

EINTR_FUNCTION_DECL_(
    int, sem_wait, (sem_t *aSem));

EINTR_FUNCTION_DECL_(
    int, sem_timedwait, (sem_t *aSem, const struct timespec *aDeadline));

EINTR_FUNCTION_DECL_(
    int, semop, (int aSemId, struct sembuf *aOps, unsigned aNumOps));

EINTR_FUNCTION_DECL_(
    int, semtimedop, (int aSemId, struct sembuf *aOps, unsigned aNumOps,
                      struct timespec *aTimeout));

EINTR_FUNCTION_DECL_(
    ssize_t, send, (int aFd, const void *aBufPtr, size_t aBufLen, int aFlags));

EINTR_FUNCTION_DECL_(
    ssize_t, sendto, (int aFd, const void *aBufPtr, size_t aBufLen, int aFlags,
                      const struct sockaddr *aAddr, socklen_t aAddrLen));

EINTR_FUNCTION_DECL_(
    ssize_t, sendmsg, (int aFd, const struct msghdr *aMsg, int aFlags));

EINTR_FUNCTION_DECL_(
    int, sigsuspend, (const sigset_t *aSet));

EINTR_FUNCTION_DECL_(
    int, sigtimedwait, (const sigset_t *aSet, siginfo_t *aInfo,
                        const struct timespec *aTimeout));

EINTR_FUNCTION_DECL_(
    int, sigwaitinfo, (const sigset_t *aSet, siginfo_t *aInfo));

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

EINTR_FUNCTION_DECL_(
    ssize_t, write,
    (int aFd, const void *aBuf, size_t aCount));

EINTR_FUNCTION_DECL_(
    ssize_t, writev,
    (int aFd, const struct iovec *aVec, int aCount));

/* -------------------------------------------------------------------------- */
CHECKED int
Eintr_init(struct EintrModule *self);

CHECKED struct EintrModule *
Eintr_exit(struct EintrModule *self);

bool
Eintr_active(void);

/* -------------------------------------------------------------------------- */

#endif /* EINTR_H */
