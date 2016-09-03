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

#define EINTR_MODULE_DEFN_
#include "eintr_.h"
#include "dl_.h"
#include "error_.h"
#include "timekeeping_.h"
#include "test_.h"

#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>

#include <sys/file.h>

/* -------------------------------------------------------------------------- */
static unsigned moduleInit_;

/* -------------------------------------------------------------------------- */
#define EINTR_FUNCTION_DEFN_(                                           \
    Eintr_, Enum_, Return_, Name_, Signature_, Args_, Predicate_, ...)  \
                                                                        \
/* Verify that the function signature of the interceptor matches the    \
 * declared function signature. */                                      \
                                                                        \
static DECLTYPE(Name_ ## _) *Name_ ## _check_ UNUSED = Name_;           \
                                                                        \
Return_                                                                 \
Name_ Signature_                                                        \
{                                                                       \
    return                                                              \
        SYSCALL_EINTR_(                                                 \
            Eintr_, Enum_, Name_, Predicate_, ## __VA_ARGS__) Args_;    \
}                                                                       \
                                                                        \
Return_                                                                 \
Name_ ## _raw Signature_                                                \
{                                                                       \
    SYSCALL_RAW_(Enum_, Name_, Args_);                                  \
}                                                                       \
                                                                        \
struct EintrModule

/* -------------------------------------------------------------------------- */
/* Interrupted System Calls
 *
 * These interceptors provide key functionality in two key areas:
 *
 * o Retry EINTR so that application code can be agnostic about SA_RESTART
 * o Inject EINTR so improve unit test coverage in application code
 *
 * Using close() as an example, application code calling close() will
 * never expect to see EINTR The interceptor here will retry close() if
 * EINTR is received from the underlying implementation.
 *
 * Application code can call close_eintr() if it wishes to deal with
 * EINTR directly. This might be the case where the application is running
 * an event loop, and should be updating deadlines when IO is interrupted,
 * of if the application must deal with SIGALRM. */

struct SystemCall
{
    const char *mName;
    uintptr_t   mAddr;
};

enum SystemCallKind
{
    SYSTEMCALL_ACCEPT,
    SYSTEMCALL_ACCEPT4,
    SYSTEMCALL_CLOCKNANOSLEEP,
    SYSTEMCALL_CLOSE,
    SYSTEMCALL_CONNECT,
    SYSTEMCALL_EPOLLWAIT,
    SYSTEMCALL_EPOLLPWAIT,
    SYSTEMCALL_FCNTL,
    SYSTEMCALL_FLOCK,
    SYSTEMCALL_IOCTL,
    SYSTEMCALL_MQRECEIVE,
    SYSTEMCALL_MQSEND,
    SYSTEMCALL_MQTIMEDRECEIVE,
    SYSTEMCALL_MQTIMEDSEND,
    SYSTEMCALL_MSGRCV,
    SYSTEMCALL_MSGSND,
    SYSTEMCALL_NANOSLEEP,
    SYSTEMCALL_OPEN,
    SYSTEMCALL_PAUSE,
    SYSTEMCALL_POLL,
    SYSTEMCALL_PPOLL,
    SYSTEMCALL_PREAD,
    SYSTEMCALL_PREADV,
    SYSTEMCALL_PSELECT,
    SYSTEMCALL_PWRITE,
    SYSTEMCALL_PWRITEV,
    SYSTEMCALL_READ,
    SYSTEMCALL_READV,
    SYSTEMCALL_RECV,
    SYSTEMCALL_RECVFROM,
    SYSTEMCALL_RECVMSG,
    SYSTEMCALL_SLEEP,
    SYSTEMCALL_SELECT,
    SYSTEMCALL_SEMWAIT,
    SYSTEMCALL_SEMTIMEDWAIT,
    SYSTEMCALL_SEMOP,
    SYSTEMCALL_SEMTIMEDOP,
    SYSTEMCALL_SEND,
    SYSTEMCALL_SENDTO,
    SYSTEMCALL_SENDMSG,
    SYSTEMCALL_SIGSUSPEND,
    SYSTEMCALL_SIGTIMEDWAIT,
    SYSTEMCALL_SIGWAITINFO,
    SYSTEMCALL_WAIT,
    SYSTEMCALL_WAIT3,
    SYSTEMCALL_WAIT4,
    SYSTEMCALL_WAITID,
    SYSTEMCALL_WAITPID,
    SYSTEMCALL_WRITE,
    SYSTEMCALL_WRITEV,
    SYSTEMCALL_USLEEP,
    SYSTEMCALL_KINDS
};

static uintptr_t
invokeSystemCall(enum SystemCallKind aKind);

static uintptr_t
interruptSystemCall(enum SystemCallKind aKind, const char *aErrName);

/* -------------------------------------------------------------------------- */
#define SYSCALL_EINTR_(Eintr_, Kind_, Function_, Predicate_, ...)   \
    ({                                                              \
        uintptr_t syscall_;                                         \
                                                                    \
        if ( ! (Eintr_) || (Predicate_))                            \
            syscall_ = invokeSystemCall((Kind_));                   \
        else                                                        \
        {                                                           \
            syscall_ = interruptSystemCall((Kind_), 0);             \
                                                                    \
            if ( ! syscall_)                                        \
            {                                                       \
                do { __VA_ARGS__ } while (0);                       \
                                                                    \
                errno = (Eintr_);                                   \
                return -1;                                          \
            }                                                       \
        }                                                           \
                                                                    \
        (DECLTYPE(Function_) *) syscall_;                           \
    })

/* -------------------------------------------------------------------------- */
#define SYSCALL_RAW_(Kind_, Function_, Args_)                   \
    do                                                          \
    {                                                           \
        uintptr_t syscall_ = invokeSystemCall((Kind_));         \
                                                                \
        AUTO(rc, ((DECLTYPE(Function_) *) syscall_) Args_);     \
                                                                \
        return rc;                                              \
                                                                \
    } while (0)


/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_ACCEPT,
    int,
    accept,
    (int aFd, struct sockaddr *aAddr, socklen_t *aAddrLen),
    (aFd, aAddr, aAddrLen),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_ACCEPT4,
    int,
    accept4,
    (int aFd, struct sockaddr *aAddr, socklen_t *aAddrLen, int aOptions),
    (aFd, aAddr, aAddrLen, aOptions),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_CLOCKNANOSLEEP,
    int,
    clock_nanosleep,
    (clockid_t aClockId, int aFlags,
     const struct timespec *aReq, struct timespec *aRem),
    (aClockId, aFlags, aReq, aRem),
    false,
    {
        if (TIMER_ABSTIME == aFlags && aRem)
            *aRem = *aReq;

        return EINTR;
    });

/* -------------------------------------------------------------------------- */
static int
close_raw_(int aFd, uintptr_t aCloseAddr)
{
    /* From http://austingroupbugs.net/view.php?id=529
     *
     * If close( ) is interrupted by a signal that is to be caught, then it
     * is unspecified whether it returns -1 with errno set to [EINTR] with
     * fildes remaining open, or returns -1 with errno set to [EINPROGRESS]
     * with fildes being closed, or returns 0 to indicate successful
     * completion; except that if POSIX_CLOSE_RESTART is defined as 0, then
     * the option of returning -1 with errno set to [EINTR] and fildes
     * remaining open shall not occur. If close() returns -1 with errno set
     * to [EINTR], it is unspecified whether fildes can subsequently be
     * passed to any function except close( ) or posix_close( ) without error.
     * For all other error situations (except for [EBADF] where fildes was
     * invalid), fildes shall be closed. If fildes was closed even though
     * the close operation is incomplete, the close operation shall continue
     * asynchronously and the process shall have no further ability to track
     * the completion or final status of the close operation. */

    int rc = ((DECLTYPE(close) *) aCloseAddr)(aFd);

    rc = ! rc
        ? 0
#ifndef POSIX_CLOSE_RESTART
#ifdef __linux__
        : EINTR == errno ? 0 /* https://lwn.net/Articles/576478/ */
#endif
#endif
        : rc;

    return rc;
}

int
close_raw(int aFd)
{
    return close_raw_(aFd, invokeSystemCall(SYSTEMCALL_CLOSE));
}

int
close(int aFd)
{
    uintptr_t closeAddr = interruptSystemCall(SYSTEMCALL_CLOSE, "EINPROGRESS");

    int rc =
        close_raw_(
            aFd,
            closeAddr ? closeAddr : invokeSystemCall(SYSTEMCALL_CLOSE));

    if ( ! rc && ! closeAddr)
    {
        errno = EINPROGRESS;
        rc = -1;
    }

    return rc;
}

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_CONNECT,
    int,
    connect,
    (int aFd, const struct sockaddr *aAddr, socklen_t aAddrLen),
    (aFd, aAddr, aAddrLen),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_EPOLLWAIT,
    int,
    epoll_wait,
    (int aFd, struct epoll_event *aEvents, int aMaxEvents, int aTimeout),
    (aFd, aEvents, aMaxEvents, aTimeout),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_EPOLLPWAIT,
    int,
    epoll_pwait,
    (int aFd, struct epoll_event *aEvents, int aMaxEvents, int aTimeout,
     const sigset_t *aMask),
    (aFd, aEvents, aMaxEvents, aTimeout, aMask),
    false);

/* -------------------------------------------------------------------------- */
static int
fcntl_raw_(uintptr_t aFcntlAddr, int aFd, int aCmd, va_list aArgs)
{
    int rc = -1;

    AUTO(fcntlp, (DECLTYPE(fcntl) *) aFcntlAddr);

    switch (aCmd)
    {
    default:
        errno = ENOSYS;
        break;

#ifdef F_GETPIPE_SZ
    case F_GETPIPE_SZ:
#endif
#ifdef F_GETLEASE
    case F_GETLEASE:
#endif
#ifdef F_GETSIG
    case F_GETSIG:
#endif
#ifdef F_DUPFD_CLOEXEC
    case F_DUPFD_CLOEXEC:
#endif
    case F_DUPFD:
    case F_GETFD:
    case F_GETFL:
    case F_GETOWN:
        rc = fcntlp(aFd, aCmd);
        break;

#ifdef F_SETPIPE_SZ
    case F_SETPIPE_SZ:
#endif
#ifdef F_NOTIFY
    case F_NOTIFY:
#endif
#ifdef F_SETLEASE
    case F_SETLEASE:
#endif
#ifdef F_SETSIG
    case F_SETSIG:
#endif
    case F_SETFD:
    case F_SETFL:
    case F_SETOWN:
        {
            int arg = va_arg(aArgs, int);

            rc = fcntlp(aFd, aCmd, arg);
        }
        break;

#ifdef F_GETOWN_EX
    case F_GETOWN_EX:
#endif
#ifdef F_SETOWN_EX
    case F_SETOWN_EX:
#endif
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        {
            void *arg = va_arg(aArgs, void *);

            rc = fcntlp(aFd, aCmd, arg);
        }
        break;
    }

    return rc;
}

int
fcntl_raw(int aFd, int aCmd, ...)
{
    int rc;

    va_list args;

    va_start(args, aCmd);
    rc = fcntl_raw_(invokeSystemCall(SYSTEMCALL_FCNTL), aFd, aCmd, args);
    va_end(args);

    return rc;
}

int
fcntl(int aFd, int aCmd, ...)
{
    int rc;

    uintptr_t fcntlAddr;

    if (F_SETLKW != aCmd)
        fcntlAddr = invokeSystemCall(SYSTEMCALL_FCNTL);
    else
    {
        fcntlAddr = interruptSystemCall(SYSTEMCALL_FCNTL, 0);

        if ( ! fcntlAddr)
        {
            errno = EINTR;
            return -1;
        }
    }

    va_list args;

    va_start(args, aCmd);
    rc = fcntl_raw_(fcntlAddr, aFd, aCmd, args);
    va_end(args);

    return rc;
}

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_FLOCK,
    int,
    flock,
    (int aFd, int aOp),
    (aFd, aOp),
    (LOCK_UN | LOCK_NB) == aOp || LOCK_UN == aOp);

/* -------------------------------------------------------------------------- */
static int
local_ioctl_raw_(int aFd, EINTR_IOCTL_REQUEST_T_ aRequest, void *aArg)
{
    SYSCALL_RAW_(SYSTEMCALL_IOCTL, ioctl, (aFd, aRequest, aArg));
}

static int
local_ioctl_(int aFd, EINTR_IOCTL_REQUEST_T_ aRequest, void *aArg)
{
    return
        SYSCALL_EINTR_(
            EINTR, SYSTEMCALL_IOCTL, ioctl, false)(aFd, aRequest, aArg);
}

#define EINTR_IOCTL_DEFN_(Name_)                                \
int                                                             \
Name_(int aFd, EINTR_IOCTL_REQUEST_T_ aRequest, ...)            \
{                                                               \
    void *arg;                                                  \
                                                                \
    va_list argp;                                               \
                                                                \
    va_start(argp, aRequest);                                   \
    arg = va_arg(argp, void *);                                 \
    va_end(argp);                                               \
                                                                \
    return local_ ## Name_ ## _(aFd, aRequest, arg);            \
}                                                               \
struct EintrModule

EINTR_IOCTL_DEFN_(ioctl);
EINTR_IOCTL_DEFN_(ioctl_raw);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_MQRECEIVE,
    ssize_t,
    mq_receive,
    (mqd_t aMq, char *aMsgPtr, size_t aMsgLen, unsigned *aPriority),
    (aMq, aMsgPtr, aMsgLen, aPriority),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_MQSEND,
    int,
    mq_send,
    (mqd_t aMq, const char *aMsgPtr, size_t aMsgLen, unsigned aPriority),
    (aMq, aMsgPtr, aMsgLen, aPriority),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_MQTIMEDRECEIVE,
    ssize_t,
    mq_timedreceive,
    (mqd_t aMq, char *aMsgPtr, size_t aMsgLen, unsigned *aPriority,
     const struct timespec *aTimeout),
    (aMq, aMsgPtr, aMsgLen, aPriority, aTimeout),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_MQTIMEDSEND,
    int,
    mq_timedsend,
    (mqd_t aMq, const char *aMsgPtr, size_t aMsgLen, unsigned aPriority,
     const struct timespec *aTimeout),
    (aMq, aMsgPtr, aMsgLen, aPriority, aTimeout),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_MSGRCV,
    ssize_t,
    msgrcv,
    (int aMsgId, void *aMsg, size_t aSize, long aType, int aFlag),
    (aMsgId, aMsg, aSize, aType, aFlag),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_MSGSND,
    int,
    msgsnd,
    (int aMsgId, const void *aMsg, size_t aSize, int aFlag),
    (aMsgId, aMsg, aSize, aFlag),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_NANOSLEEP,
    int,
    nanosleep,
    (const struct timespec *aReq, struct timespec *aRem),
    (aReq, aRem),
    false,
    {
        if (aRem)
            *aRem = *aReq;
    });

/* -------------------------------------------------------------------------- */
static int
local_open_raw_(const char *aPath, int aFlags, mode_t aMode)
{
    SYSCALL_RAW_(SYSTEMCALL_OPEN, open, (aPath, aFlags, aMode));
}

static int
local_open_(const char *aPath, int aFlags, mode_t aMode)
{
    return
        SYSCALL_EINTR_(
            EINTR, SYSTEMCALL_OPEN, open, false)(aPath, aFlags, aMode);
}

#define EINTR_OPEN_DEFN_(Name_)                         \
int                                                     \
Name_(const char *aPath, int aFlags, ...)               \
{                                                       \
    mode_t mode;                                        \
                                                        \
    va_list argp;                                       \
                                                        \
    va_start(argp, aFlags);                             \
    mode = (                                            \
        sizeof(mode_t) < sizeof(int)                    \
        ? va_arg(argp, int)                             \
        : va_arg(argp, mode_t));                        \
    va_end(argp);                                       \
                                                        \
    return local_ ## Name_ ## _(aPath, aFlags, mode);   \
}                                                       \
struct EintrModule

EINTR_OPEN_DEFN_(open);
EINTR_OPEN_DEFN_(open_raw);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_PAUSE,
    int,
    pause,
    (void),
    (),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_POLL,
    int,
    poll,
    (struct pollfd *aFds, nfds_t aNumFds, int aTimeout),
    (aFds, aNumFds, aTimeout),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_PPOLL,
    int,
    ppoll,
    (struct pollfd *aFds, nfds_t aNumFds,
     const struct timespec *aTimeout, const sigset_t *aMask),
    (aFds, aNumFds, aTimeout, aMask),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_PREAD,
    ssize_t,
    pread,
    (int aFd, void *aBuf, size_t aCount, off_t aOffset),
    (aFd, aBuf, aCount, aOffset),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_PREADV,
    ssize_t,
    preadv,
    (int aFd, const struct iovec *aVec, int aCount, off_t aOffset),
    (aFd, aVec, aCount, aOffset),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_PSELECT,
    int,
    pselect,
    (int aNumFds, fd_set *aRdFds, fd_set *aWrFds, fd_set *aExFds,
     const struct timespec *aTimeout, const sigset_t *aMask),
    (aNumFds, aRdFds, aWrFds, aExFds, aTimeout, aMask),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_PWRITE,
    ssize_t,
    pwrite,
    (int aFd, const void *aBuf, size_t aCount, off_t aOffset),
    (aFd, aBuf, aCount, aOffset),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_PWRITEV,
    ssize_t,
    pwritev,
    (int aFd, const struct iovec *aVec, int aCount, off_t aOffset),
    (aFd, aVec, aCount, aOffset),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_READ,
    ssize_t,
    read,
    (int aFd, void *aBuf, size_t aCount),
    (aFd, aBuf, aCount),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_READV,
    ssize_t,
    readv,
    (int aFd, const struct iovec *aVec, int aCount),
    (aFd, aVec, aCount),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_RECV,
    ssize_t,
    recv,
    (int aFd, void *aBufPtr, size_t aBufLen, int aOptions),
    (aFd, aBufPtr, aBufLen, aOptions),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_RECVFROM,
    ssize_t,
    recvfrom,
    (int aFd, void *aBufPtr, size_t aBufLen, int aOptions,
     struct sockaddr *aAddr, socklen_t *aAddrLen),
    (aFd, aBufPtr, aBufLen, aOptions, aAddr, aAddrLen),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_RECVMSG,
    ssize_t,
    recvmsg,
    (int aFd, struct msghdr *aMsg, int aOptions),
    (aFd, aMsg, aOptions),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SELECT,
    int,
    select,
    (int aNumFds, fd_set *aRdFds, fd_set *aWrFds, fd_set *aExFds,
     struct timeval *aTimeout),
    (aNumFds, aRdFds, aWrFds, aExFds, aTimeout),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SEMWAIT,
    int,
    sem_wait,
    (sem_t *aSem),
    (aSem),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SEMTIMEDWAIT,
    int,
    sem_timedwait,
    (sem_t *aSem, const struct timespec *aDeadline),
    (aSem, aDeadline),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SEMOP,
    int,
    semop,
    (int aSemId, struct sembuf *aOps, unsigned aNumOps),
    (aSemId, aOps, aNumOps),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SEMTIMEDOP,
    int,
    semtimedop,
    (int aSemId, struct sembuf *aOps, unsigned aNumOps,
     const struct timespec *aTimeout),
    (aSemId, aOps, aNumOps, aTimeout),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SEND,
    ssize_t,
    send,
    (int aFd, const void *aBufPtr, size_t aBufLen, int aOptions),
    (aFd, aBufPtr, aBufLen, aOptions),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SENDTO,
    ssize_t,
    sendto,
    (int aFd, const void *aBufPtr, size_t aBufLen, int aOptions,
     const struct sockaddr *aAddr, socklen_t aAddrLen),
    (aFd, aBufPtr, aBufLen, aOptions, aAddr, aAddrLen),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SENDMSG,
    ssize_t,
    sendmsg,
    (int aFd, const struct msghdr *aMsg, int aOptions),
    (aFd, aMsg, aOptions),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SIGSUSPEND,
    int,
    sigsuspend,
    (const sigset_t *aSet),
    (aSet),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SIGWAITINFO,
    int,
    sigwaitinfo,
    (const sigset_t *aSet, siginfo_t *aInfo),
    (aSet, aInfo),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SIGTIMEDWAIT,
    int,
    sigtimedwait,
    (const sigset_t *aSet, siginfo_t *aInfo, const struct timespec *aTimeout),
    (aSet, aInfo, aTimeout),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SLEEP,
    unsigned,
    sleep,
    (unsigned aTimeout),
    (aTimeout),
    false,
    {
        return aTimeout;
    });

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_WAIT,
    pid_t,
    wait,
    (int *aStatus),
    (aStatus),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_WAIT3,
    pid_t,
    wait3,
    (int *aStatus, int aOptions, struct rusage *aRusage),
    (aStatus, aOptions, aRusage),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_WAIT4,
    pid_t,
    wait4,
    (pid_t aPid, int *aStatus, int aOptions, struct rusage *aRusage),
    (aPid, aStatus, aOptions, aRusage),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_WAITID,
    int,
    waitid,
    (idtype_t aIdType, id_t aId, siginfo_t *aInfo, int aOptions),
    (aIdType, aId, aInfo, aOptions),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_WAITPID,
    pid_t,
    waitpid,
    (pid_t aPid, int *aStatus, int aOptions),
    (aPid, aStatus, aOptions),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_WRITE,
    ssize_t,
    write,
    (int aFd, const void *aBuf, size_t aCount),
    (aFd, aBuf, aCount),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_WRITEV,
    ssize_t,
    writev,
    (int aFd, const struct iovec *aVec, int aCount),
    (aFd, aVec, aCount),
    false);

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_USLEEP,
    int,
    usleep,
    (useconds_t aTimeout),
    (aTimeout),
    false);

/* -------------------------------------------------------------------------- */
static uintptr_t
initSystemCall(struct SystemCall *self)
{
    uintptr_t addr = self->mAddr;

    if ( ! addr)
    {
        const char *err;

        char *libName = findDlSymbol(self->mName, &addr, &err);

        ensure(libName);

        free(libName);

        self->mAddr = addr;
    }

    return addr;
}

/* -------------------------------------------------------------------------- */
#define SYSCALL_ENTRY_(Name_) { STRINGIFY(Name_), }

static struct SystemCall systemCall_[SYSTEMCALL_KINDS] =
{
    [SYSTEMCALL_ACCEPT]         = SYSCALL_ENTRY_(accept),
    [SYSTEMCALL_ACCEPT4]        = SYSCALL_ENTRY_(accept4),
    [SYSTEMCALL_CLOCKNANOSLEEP] = SYSCALL_ENTRY_(clock_nanosleep),
    [SYSTEMCALL_CLOSE]          = SYSCALL_ENTRY_(close),
    [SYSTEMCALL_CONNECT]        = SYSCALL_ENTRY_(connect),
    [SYSTEMCALL_EPOLLWAIT]      = SYSCALL_ENTRY_(epoll_wait),
    [SYSTEMCALL_EPOLLPWAIT]     = SYSCALL_ENTRY_(epoll_pwait),
    [SYSTEMCALL_FCNTL]          = SYSCALL_ENTRY_(fcntl),
    [SYSTEMCALL_FLOCK]          = SYSCALL_ENTRY_(flock),
    [SYSTEMCALL_IOCTL]          = SYSCALL_ENTRY_(ioctl),
    [SYSTEMCALL_MQRECEIVE]      = SYSCALL_ENTRY_(mq_receive),
    [SYSTEMCALL_MQSEND]         = SYSCALL_ENTRY_(mq_send),
    [SYSTEMCALL_MQTIMEDRECEIVE] = SYSCALL_ENTRY_(mq_timedreceive),
    [SYSTEMCALL_MQTIMEDSEND]    = SYSCALL_ENTRY_(mq_timedsend),
    [SYSTEMCALL_MSGRCV]         = SYSCALL_ENTRY_(msgrcv),
    [SYSTEMCALL_MSGSND]         = SYSCALL_ENTRY_(msgsnd),
    [SYSTEMCALL_NANOSLEEP]      = SYSCALL_ENTRY_(nanosleep),
    [SYSTEMCALL_OPEN]           = SYSCALL_ENTRY_(open),
    [SYSTEMCALL_PAUSE]          = SYSCALL_ENTRY_(pause),
    [SYSTEMCALL_POLL]           = SYSCALL_ENTRY_(poll),
    [SYSTEMCALL_PPOLL]          = SYSCALL_ENTRY_(ppoll),
    [SYSTEMCALL_PREAD]          = SYSCALL_ENTRY_(pread),
    [SYSTEMCALL_PREADV]         = SYSCALL_ENTRY_(preadv),
    [SYSTEMCALL_PSELECT]        = SYSCALL_ENTRY_(pselect),
    [SYSTEMCALL_PWRITE]         = SYSCALL_ENTRY_(pwrite),
    [SYSTEMCALL_PWRITEV]        = SYSCALL_ENTRY_(pwritev),
    [SYSTEMCALL_READ]           = SYSCALL_ENTRY_(read),
    [SYSTEMCALL_READV]          = SYSCALL_ENTRY_(readv),
    [SYSTEMCALL_RECV]           = SYSCALL_ENTRY_(recv),
    [SYSTEMCALL_RECVFROM]       = SYSCALL_ENTRY_(recvfrom),
    [SYSTEMCALL_RECVMSG]        = SYSCALL_ENTRY_(recvmsg),
    [SYSTEMCALL_SELECT]         = SYSCALL_ENTRY_(select),
    [SYSTEMCALL_SEMWAIT]        = SYSCALL_ENTRY_(sem_wait),
    [SYSTEMCALL_SEMTIMEDWAIT]   = SYSCALL_ENTRY_(sem_timedwait),
    [SYSTEMCALL_SEMOP]          = SYSCALL_ENTRY_(semop),
    [SYSTEMCALL_SEMTIMEDOP]     = SYSCALL_ENTRY_(semtimedop),
    [SYSTEMCALL_SEND]           = SYSCALL_ENTRY_(send),
    [SYSTEMCALL_SENDTO]         = SYSCALL_ENTRY_(sendto),
    [SYSTEMCALL_SENDMSG]        = SYSCALL_ENTRY_(sendmsg),
    [SYSTEMCALL_SIGSUSPEND]     = SYSCALL_ENTRY_(sigsuspend),
    [SYSTEMCALL_SIGTIMEDWAIT]   = SYSCALL_ENTRY_(sigtimedwait),
    [SYSTEMCALL_SIGWAITINFO]    = SYSCALL_ENTRY_(sigwaitinfo),
    [SYSTEMCALL_SLEEP]          = SYSCALL_ENTRY_(sleep),
    [SYSTEMCALL_WAIT]           = SYSCALL_ENTRY_(wait),
    [SYSTEMCALL_WAIT3]          = SYSCALL_ENTRY_(wait3),
    [SYSTEMCALL_WAIT4]          = SYSCALL_ENTRY_(wait4),
    [SYSTEMCALL_WAITID]         = SYSCALL_ENTRY_(waitid),
    [SYSTEMCALL_WAITPID]        = SYSCALL_ENTRY_(waitpid),
    [SYSTEMCALL_WRITE]          = SYSCALL_ENTRY_(write),
    [SYSTEMCALL_WRITEV]         = SYSCALL_ENTRY_(writev),
    [SYSTEMCALL_USLEEP]         = SYSCALL_ENTRY_(usleep),
};

/* -------------------------------------------------------------------------- */
EARLY_INITIALISER(
    eintr_,
    ({
        for (size_t sx = 0; NUMBEROF(systemCall_) > sx; ++sx)
        {
            ABORT_UNLESS(
                initSystemCall(&systemCall_[sx]));
        }
    }),
    ({ }));

/* -------------------------------------------------------------------------- */
static uintptr_t
interruptSystemCall(enum SystemCallKind aKind, const char *aErrName)
{
    struct SystemCall *sysCall = &systemCall_[aKind];

    uintptr_t addr = 0;

    if (testAction(TestLevelRace) && 1 > random() % 10)
        debug(0,
              "inject %s into %s",
              (aErrName ? aErrName : "EINTR"),
              sysCall->mName);
    else
        addr = initSystemCall(sysCall);

    return addr;
}

/* -------------------------------------------------------------------------- */
static uintptr_t
invokeSystemCall(enum SystemCallKind aKind)
{
    return initSystemCall(&systemCall_[aKind]);
}

/* -------------------------------------------------------------------------- */
bool
Eintr_active(void)
{
    return testMode(TestLevelRace);
}

/* -------------------------------------------------------------------------- */
int
Eintr_init(struct EintrModule *self)
{
    int rc = -1;

    self->mModule = self;

    if ( ! moduleInit_)
    { }

    ++moduleInit_;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
struct EintrModule *
Eintr_exit(struct EintrModule *self)
{
    if (self)
    {
        --moduleInit_;
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
