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
    Eintr_, Enum_, Return_, Name_, Signature_, Args_, Predicate_)       \
                                                                        \
Return_                                                                 \
Name_ Signature_                                                        \
{                                                                       \
    return Name_ ## _eintr Args_;                                       \
}                                                                       \
                                                                        \
Return_                                                                 \
Name_ ## _eintr Signature_                                              \
{                                                                       \
    return                                                              \
        SYSCALL_EINTR_(Eintr_, Enum_, Name_, Predicate_) Args_;         \
}                                                                       \
                                                                        \
static bool                                                             \
Name_ ## _check_(void)                                                  \
{                                                                       \
    return __builtin_types_compatible_p(                                \
        DECLTYPE(Name_), DECLTYPE(Name_ ## _));                         \
}                                                                       \
                                                                        \
struct EintrModule

/* -------------------------------------------------------------------------- */
#define EINTR_RETRY_FUNCTION_DEFN_(                                     \
    Eintr_, Enum_, Return_, Name_, Signature_, Args_, Predicate_)       \
                                                                        \
Return_                                                                 \
Name_ Signature_                                                        \
{                                                                       \
    SYSCALL_RESTART_(Enum_, Name_, Args_);                              \
}                                                                       \
                                                                        \
Return_                                                                 \
Name_ ## _eintr Signature_                                              \
{                                                                       \
    return                                                              \
        SYSCALL_EINTR_(Eintr_, Enum_, Name_, Predicate_) Args_;         \
}                                                                       \
                                                                        \
static bool                                                             \
Name_ ## _check_(void)                                                  \
{                                                                       \
    return __builtin_types_compatible_p(                                \
        DECLTYPE(Name_), DECLTYPE(Name_ ## _));                         \
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

enum SystemCallKind
{
    SYSTEMCALL_ACCEPT,
    SYSTEMCALL_ACCEPT4,
    SYSTEMCALL_CLOSE,
    SYSTEMCALL_CONNECT,
    SYSTEMCALL_EPOLLWAIT,
    SYSTEMCALL_EPOLLPWAIT,
    SYSTEMCALL_FCNTL,
    SYSTEMCALL_FLOCK,
    SYSTEMCALL_IOCTL,
    SYSTEMCALL_OPEN,
    SYSTEMCALL_MQRECEIVE,
    SYSTEMCALL_MQSEND,
    SYSTEMCALL_MQTIMEDRECEIVE,
    SYSTEMCALL_MQTIMEDSEND,
    SYSTEMCALL_PAUSE,
    SYSTEMCALL_PREAD,
    SYSTEMCALL_PREADV,
    SYSTEMCALL_PWRITE,
    SYSTEMCALL_PWRITEV,
    SYSTEMCALL_READ,
    SYSTEMCALL_READV,
    SYSTEMCALL_RECV,
    SYSTEMCALL_RECVFROM,
    SYSTEMCALL_RECVMSG,
    SYSTEMCALL_SEMWAIT,
    SYSTEMCALL_SEMTIMEDWAIT,
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
    SYSTEMCALL_KINDS
};

static uintptr_t
invokeSystemCall(enum SystemCallKind aKind);

static uintptr_t
interruptSystemCall(enum SystemCallKind aKind);

/* -------------------------------------------------------------------------- */
#define SYSCALL_EINTR_(Eintr_, Kind_, Function_, Predicate_) \
    ({                                                       \
        uintptr_t syscall_;                                  \
                                                             \
        if ( ! (Eintr_) || (Predicate_))                     \
            syscall_ = invokeSystemCall((Kind_));            \
        else                                                 \
        {                                                    \
            syscall_ = interruptSystemCall((Kind_));         \
                                                             \
            if ( ! syscall_)                                 \
            {                                                \
                errno = (Eintr_);                            \
                return -1;                                   \
            }                                                \
        }                                                    \
                                                             \
        (DECLTYPE(Function_) *) syscall_;                    \
    })

/* -------------------------------------------------------------------------- */
#define SYSCALL_RESTART_(Kind_, Function_, Args_)               \
    do                                                          \
    {                                                           \
        while (1)                                               \
        {                                                       \
            uintptr_t syscall_ = invokeSystemCall((Kind_));     \
                                                                \
            AUTO(rc, ((DECLTYPE(Function_) *) syscall_) Args_); \
                                                                \
            if (-1 != rc || EINTR != errno)                     \
                return rc;                                      \
        }                                                       \
    } while (0)

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_ACCEPT,
    int,
    accept,
    (int aFd, struct sockaddr *aAddr, socklen_t *aAddrLen),
    (aFd, aAddr, aAddrLen),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_ACCEPT4,
    int,
    accept4,
    (int aFd, struct sockaddr *aAddr, socklen_t *aAddrLen, int aOptions),
    (aFd, aAddr, aAddrLen, aOptions),
    false);

/* -------------------------------------------------------------------------- */
static bool
close_check_(void)
{
    return __builtin_types_compatible_p(
        DECLTYPE(close), DECLTYPE(close_));
}

int
close(int aFd)
{
    int rc;

    do
        rc = close_eintr(aFd);
    while (rc && EINTR == errno);

    return rc && EINPROGRESS != errno ? -1 : 0;
}

int
close_eintr(int aFd)
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

    uintptr_t close_ = interruptSystemCall(SYSTEMCALL_CLOSE);

    if ( ! close_)
    {
        errno = EINTR;
        return -1;
    }

    return
        ! ((DECLTYPE(close) *) close_)(aFd)
        ? 0
#ifdef __linux__
        : EINTR == errno ? 0 /* https://lwn.net/Articles/576478/ */
#endif
        : EINPROGRESS == errno ? 0 : -1;
}

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_CONNECT,
    int,
    connect,
    (int aFd, const struct sockaddr *aAddr, socklen_t aAddrLen),
    (aFd, aAddr, aAddrLen),
    false);

/* -------------------------------------------------------------------------- */
static int
local_epoll_wait_(
    int aFd, struct epoll_event *aEvents, int aMaxEvents, int aTimeout)
{
    errno = ENOSYS;
    return -1;
}

EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_EPOLLWAIT,
    static int,
    local_epoll_wait,
    (int aFd, struct epoll_event *aEvents, int aMaxEvents, int aTimeout),
    (aFd, aEvents, aMaxEvents, aTimeout),
    false);

int
epoll_wait(
    int aFd, struct epoll_event *aEvents, int aMaxEvents, int aTimeout)
{
    if (0 ==aTimeout || -1 == aTimeout)
        return local_epoll_wait(aFd, aEvents, aMaxEvents, aTimeout);

    int rc;

    struct MonotonicDeadline deadline = MONOTONICDEADLINE_INIT;
    struct Duration          period   = Duration(NSECS(MilliSeconds(aTimeout)));
    struct Duration          remaining;

    do
    {
        rc = 0;

        if (monotonicDeadlineTimeExpired(&deadline, period, &remaining, 0))
            break;

        rc = local_epoll_wait_eintr(
            aFd, aEvents, aMaxEvents, MSECS(remaining.duration).ms);

    } while (-1 == rc && EINTR == errno);

    return rc;
}

int
epoll_wait_eintr(
    int aFd, struct epoll_event *aEvents, int aMaxEvents, int aTimeout)
{
    return local_epoll_wait_eintr(aFd, aEvents, aMaxEvents, aTimeout);
}

/* -------------------------------------------------------------------------- */
static int
local_epoll_pwait_(
    int aFd, struct epoll_event *aEvents, int aMaxEvents, int aTimeout,
    const sigset_t *aMask)
{
    errno = ENOSYS;
    return -1;
}

EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_EPOLLPWAIT,
    static int,
    local_epoll_pwait,
    (int aFd, struct epoll_event *aEvents, int aMaxEvents, int aTimeout,
     const sigset_t *aMask),
    (aFd, aEvents, aMaxEvents, aTimeout, aMask),
    false);

int
epoll_pwait(
    int aFd, struct epoll_event *aEvents, int aMaxEvents, int aTimeout,
    const sigset_t *aMask)
{
    if (0 ==aTimeout || -1 == aTimeout)
        return local_epoll_pwait(aFd, aEvents, aMaxEvents, aTimeout, aMask);

    int rc;

    struct MonotonicDeadline deadline = MONOTONICDEADLINE_INIT;
    struct Duration          period   = Duration(NSECS(MilliSeconds(aTimeout)));
    struct Duration          remaining;

    do
    {
        rc = 0;

        if (monotonicDeadlineTimeExpired(&deadline, period, &remaining, 0))
            break;

        rc = local_epoll_pwait_eintr(
            aFd, aEvents, aMaxEvents, MSECS(remaining.duration).ms, aMask);

    } while (-1 == rc && EINTR == errno);

    return rc;
}

int
epoll_pwait_eintr(
    int aFd, struct epoll_event *aEvents, int aMaxEvents, int aTimeout,
    const sigset_t *aMask)
{
    return local_epoll_pwait_eintr(aFd, aEvents, aMaxEvents, aTimeout, aMask);
}

/* -------------------------------------------------------------------------- */
static bool
fcntl_check_(void)
{
    return __builtin_types_compatible_p(
        DECLTYPE(fcntl), DECLTYPE(fcntl_));
}

static int
fcntl_call_(uintptr_t aFcntl, int aFd, int aCmd, va_list aArgs)
{
    int rc = -1;

    AUTO(fcntlp, (DECLTYPE(fcntl) *) aFcntl);

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
fcntl(int aFd, int aCmd, ...)
{
    int rc;

    va_list args;

    uintptr_t fcntl_ = invokeSystemCall(SYSTEMCALL_FCNTL);

    do
    {
        va_start(args, aCmd);
        rc = fcntl_call_(fcntl_, aFd, aCmd, args);
        va_end(args);
    }
    while (-1 == rc && EINTR == errno);

    return rc;
}

int
fcntl_eintr(int aFd, int aCmd, ...)
{
    int rc;

    uintptr_t fcntl_;

    if (F_SETLKW != aCmd)
        fcntl_ = invokeSystemCall(SYSTEMCALL_FCNTL);
    else
    {
        fcntl_ = interruptSystemCall(SYSTEMCALL_FCNTL);

        if ( ! fcntl_)
        {
            errno = EINTR;
            return -1;
        }
    }

    va_list args;

    va_start(args, aCmd);
    rc = fcntl_call_(fcntl_, aFd, aCmd, args);
    va_end(args);

    return rc;
}

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_FLOCK,
    int,
    flock,
    (int aFd, int aOp),
    (aFd, aOp),
    (LOCK_UN | LOCK_NB) == aOp || LOCK_UN == aOp);

/* -------------------------------------------------------------------------- */
static int
local_ioctl_(int aFd, EINTR_IOCTL_REQUEST_T_ aRequest, void *aArg)
{
    errno = ENOSYS;
    return -1;
}

EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_IOCTL,
    static int,
    local_ioctl,
    (int aFd, EINTR_IOCTL_REQUEST_T_ aRequest, void *aArg),
    (aFd, aRequest, aArg),
    false);

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
    return local_ ## Name_(aFd, aRequest, arg);                 \
}                                                               \
struct EintrModule

EINTR_IOCTL_DEFN_(ioctl);
EINTR_IOCTL_DEFN_(ioctl_eintr);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_MQRECEIVE,
    ssize_t,
    mq_receive,
    (mqd_t aMq, char *aMsgPtr, size_t aMsgLen, unsigned *aPriority),
    (aMq, aMsgPtr, aMsgLen, aPriority),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_MQSEND,
    int,
    mq_send,
    (mqd_t aMq, const char *aMsgPtr, size_t aMsgLen, unsigned aPriority),
    (aMq, aMsgPtr, aMsgLen, aPriority),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_MQTIMEDRECEIVE,
    ssize_t,
    mq_timedreceive,
    (mqd_t aMq, char *aMsgPtr, size_t aMsgLen, unsigned *aPriority,
     const struct timespec *aTimeout),
    (aMq, aMsgPtr, aMsgLen, aPriority, aTimeout),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_MQTIMEDSEND,
    int,
    mq_timedsend,
    (mqd_t aMq, const char *aMsgPtr, size_t aMsgLen, unsigned aPriority,
     const struct timespec *aTimeout),
    (aMq, aMsgPtr, aMsgLen, aPriority, aTimeout),
    false);

/* -------------------------------------------------------------------------- */
static int
local_open_(const char *aPath, int aFlags, mode_t aMode)
{
    errno = ENOSYS;
    return -1;
}

EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_OPEN,
    static int,
    local_open,
    (const char *aPath, int aFlags, mode_t aMode),
    (aPath, aFlags, aMode),
    false);

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
    return local_ ## Name_(aPath, aFlags, mode);        \
}                                                       \
struct EintrModule

EINTR_OPEN_DEFN_(open);
EINTR_OPEN_DEFN_(open_eintr);

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
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_PREAD,
    ssize_t,
    pread,
    (int aFd, void *aBuf, size_t aCount, off_t aOffset),
    (aFd, aBuf, aCount, aOffset),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_PREADV,
    ssize_t,
    preadv,
    (int aFd, const struct iovec *aVec, int aCount, off_t aOffset),
    (aFd, aVec, aCount, aOffset),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_PWRITE,
    ssize_t,
    pwrite,
    (int aFd, const void *aBuf, size_t aCount, off_t aOffset),
    (aFd, aBuf, aCount, aOffset),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_PWRITEV,
    ssize_t,
    pwritev,
    (int aFd, const struct iovec *aVec, int aCount, off_t aOffset),
    (aFd, aVec, aCount, aOffset),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_READ,
    ssize_t,
    read,
    (int aFd, void *aBuf, size_t aCount),
    (aFd, aBuf, aCount),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_READV,
    ssize_t,
    readv,
    (int aFd, const struct iovec *aVec, int aCount),
    (aFd, aVec, aCount),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_RECV,
    ssize_t,
    recv,
    (int aFd, void *aBufPtr, size_t aBufLen, int aOptions),
    (aFd, aBufPtr, aBufLen, aOptions),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_RECVFROM,
    ssize_t,
    recvfrom,
    (int aFd, void *aBufPtr, size_t aBufLen, int aOptions,
     struct sockaddr *aAddr, socklen_t *aAddrLen),
    (aFd, aBufPtr, aBufLen, aOptions, aAddr, aAddrLen),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_RECVMSG,
    ssize_t,
    recvmsg,
    (int aFd, struct msghdr *aMsg, int aOptions),
    (aFd, aMsg, aOptions),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SEMWAIT,
    int,
    sem_wait,
    (sem_t *aSem),
    (aSem),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SEMTIMEDWAIT,
    int,
    sem_timedwait,
    (sem_t *aSem, const struct timespec *aDeadline),
    (aSem, aDeadline),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SEND,
    ssize_t,
    send,
    (int aFd, const void *aBufPtr, size_t aBufLen, int aOptions),
    (aFd, aBufPtr, aBufLen, aOptions),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_SENDTO,
    ssize_t,
    sendto,
    (int aFd, const void *aBufPtr, size_t aBufLen, int aOptions,
     const struct sockaddr *aAddr, socklen_t aAddrLen),
    (aFd, aBufPtr, aBufLen, aOptions, aAddr, aAddrLen),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
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
    SYSTEMCALL_SIGSUSPEND,
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
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_WAIT,
    pid_t,
    wait,
    (int *aStatus),
    (aStatus),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_WAIT3,
    pid_t,
    wait3,
    (int *aStatus, int aOptions, struct rusage *aRusage),
    (aStatus, aOptions, aRusage),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_WAIT4,
    pid_t,
    wait4,
    (pid_t aPid, int *aStatus, int aOptions, struct rusage *aRusage),
    (aPid, aStatus, aOptions, aRusage),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_WAITID,
    int,
    waitid,
    (idtype_t aIdType, id_t aId, siginfo_t *aInfo, int aOptions),
    (aIdType, aId, aInfo, aOptions),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_WAITPID,
    pid_t,
    waitpid,
    (pid_t aPid, int *aStatus, int aOptions),
    (aPid, aStatus, aOptions),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_WRITE,
    ssize_t,
    write,
    (int aFd, const void *aBuf, size_t aCount),
    (aFd, aBuf, aCount),
    false);

/* -------------------------------------------------------------------------- */
EINTR_RETRY_FUNCTION_DEFN_(
    EINTR,
    SYSTEMCALL_WRITEV,
    ssize_t,
    writev,
    (int aFd, const struct iovec *aVec, int aCount),
    (aFd, aVec, aCount),
    false);

/* -------------------------------------------------------------------------- */
#define SYSCALL_ENTRY_(Prefix_, Name_)        \
    { Prefix_ ## Name_ ## _check_, STRINGIFY(Name_), }

struct SystemCall
{
    bool      (*mCheck)(void);
    const char *mName;
    uintptr_t   mAddr;
};

static struct SystemCall systemCall_[SYSTEMCALL_KINDS] =
{
    [SYSTEMCALL_ACCEPT]         = SYSCALL_ENTRY_(, accept),
    [SYSTEMCALL_ACCEPT4]        = SYSCALL_ENTRY_(, accept4),
    [SYSTEMCALL_CLOSE]          = SYSCALL_ENTRY_(, close),
    [SYSTEMCALL_CONNECT]        = SYSCALL_ENTRY_(, connect),
    [SYSTEMCALL_EPOLLWAIT]      = SYSCALL_ENTRY_(local_, epoll_wait),
    [SYSTEMCALL_EPOLLPWAIT]     = SYSCALL_ENTRY_(local_, epoll_pwait),
    [SYSTEMCALL_FCNTL]          = SYSCALL_ENTRY_(, fcntl),
    [SYSTEMCALL_FLOCK]          = SYSCALL_ENTRY_(, flock),
    [SYSTEMCALL_IOCTL]          = SYSCALL_ENTRY_(local_, ioctl),
    [SYSTEMCALL_MQRECEIVE]      = SYSCALL_ENTRY_(, mq_receive),
    [SYSTEMCALL_MQSEND]         = SYSCALL_ENTRY_(, mq_send),
    [SYSTEMCALL_MQTIMEDRECEIVE] = SYSCALL_ENTRY_(, mq_timedreceive),
    [SYSTEMCALL_MQTIMEDSEND]    = SYSCALL_ENTRY_(, mq_timedsend),
    [SYSTEMCALL_OPEN]           = SYSCALL_ENTRY_(local_, open),
    [SYSTEMCALL_PAUSE]          = SYSCALL_ENTRY_(, pause),
    [SYSTEMCALL_PREAD]          = SYSCALL_ENTRY_(, pread),
    [SYSTEMCALL_PREADV]         = SYSCALL_ENTRY_(, preadv),
    [SYSTEMCALL_PWRITE]         = SYSCALL_ENTRY_(, pwrite),
    [SYSTEMCALL_PWRITEV]        = SYSCALL_ENTRY_(, pwritev),
    [SYSTEMCALL_READ]           = SYSCALL_ENTRY_(, read),
    [SYSTEMCALL_READV]          = SYSCALL_ENTRY_(, readv),
    [SYSTEMCALL_RECV]           = SYSCALL_ENTRY_(, recv),
    [SYSTEMCALL_RECVFROM]       = SYSCALL_ENTRY_(, recvfrom),
    [SYSTEMCALL_RECVMSG]        = SYSCALL_ENTRY_(, recvmsg),
    [SYSTEMCALL_SEMWAIT]        = SYSCALL_ENTRY_(, sem_wait),
    [SYSTEMCALL_SEMTIMEDWAIT]   = SYSCALL_ENTRY_(, sem_timedwait),
    [SYSTEMCALL_SEND]           = SYSCALL_ENTRY_(, send),
    [SYSTEMCALL_SENDTO]         = SYSCALL_ENTRY_(, sendto),
    [SYSTEMCALL_SENDMSG]        = SYSCALL_ENTRY_(, sendmsg),
    [SYSTEMCALL_SIGSUSPEND]     = SYSCALL_ENTRY_(, sigsuspend),
    [SYSTEMCALL_SIGTIMEDWAIT]   = SYSCALL_ENTRY_(, sigtimedwait),
    [SYSTEMCALL_SIGWAITINFO]    = SYSCALL_ENTRY_(, sigwaitinfo),
    [SYSTEMCALL_WAIT]           = SYSCALL_ENTRY_(, wait),
    [SYSTEMCALL_WAIT3]          = SYSCALL_ENTRY_(, wait3),
    [SYSTEMCALL_WAIT4]          = SYSCALL_ENTRY_(, wait4),
    [SYSTEMCALL_WAITID]         = SYSCALL_ENTRY_(, waitid),
    [SYSTEMCALL_WAITPID]        = SYSCALL_ENTRY_(, waitpid),
    [SYSTEMCALL_WRITE]          = SYSCALL_ENTRY_(, write),
    [SYSTEMCALL_WRITEV]         = SYSCALL_ENTRY_(, writev),
};

/* -------------------------------------------------------------------------- */
static uintptr_t
initSystemCall(struct SystemCall *self)
{
    uintptr_t addr = self->mAddr;

    if ( ! addr)
    {
        ensure(self->mCheck());

        const char *err;

        char *libName = findDlSymbol(self->mName, &addr, &err);

        ensure(libName);

        free(libName);

        self->mAddr = addr;
    }

    return addr;
}

/* -------------------------------------------------------------------------- */
static uintptr_t
interruptSystemCall(enum SystemCallKind aKind)
{
    struct SystemCall *sysCall = &systemCall_[aKind];

    uintptr_t addr = 0;

    if (testAction(TestLevelRace) && 1 > random() % 10)
        debug(0, "inject EINTR into %s", sysCall->mName);
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
    {
        for (size_t sx = 0; NUMBEROF(systemCall_) > sx; ++sx)
        {
            ERROR_UNLESS(
                initSystemCall(&systemCall_[sx]));
        }
    }

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
#if 0
           * read(2), readv(2), write(2), writev(2),  and  ioctl(2)  calls  on
             "slow"  devices.   A  "slow" device is one where the I/O call may
             block for an indefinite time, for example, a terminal,  pipe,  or
             socket.   (A  disk is not a slow device according to this defini-
             tion.)  If an I/O call on a slow device has  already  transferred
             some data by the time it is interrupted by a signal handler, then
             the call will return a success status (normally,  the  number  of
             bytes transferred).

           * open(2),  if  it  can  block  (e.g.,  when  opening  a  FIFO; see
             fifo(7)).

           * wait(2), wait3(2), wait4(2), waitid(2), and waitpid(2).

           * Socket interfaces: accept(2), connect(2),  recv(2),  recvfrom(2),
             recvmsg(2),  send(2), sendto(2), and sendmsg(2), unless a timeout
             has been set on the socket (see below).

           * File locking interfaces: flock(2) and fcntl(2) F_SETLKW.

           * POSIX   message   queue   interfaces:   mq_receive(3),   mq_time-
             dreceive(3), mq_send(3), and mq_timedsend(3).

           * futex(2)  FUTEX_WAIT  (since  Linux  2.6.22;  beforehand,  always
             failed with EINTR).

           * POSIX  semaphore  interfaces:  sem_wait(3)  and  sem_timedwait(3)
             (since Linux 2.6.22; beforehand, always failed with EINTR).
       The following interfaces are never restarted after being interrupted by
       a signal handler, regardless of the use of SA_RESTART; they always fail
       with the error EINTR when interrupted by a signal handler:

           * Socket  interfaces,  when  a  timeout  has been set on the socket
             using  setsockopt(2):  accept(2),   recv(2),   recvfrom(2),   and
             recvmsg(2), if a receive timeout (SO_RCVTIMEO) has been set; con-
             nect(2), send(2), sendto(2), and sendmsg(2), if  a  send  timeout
             (SO_SNDTIMEO) has been set.

           * Interfaces  used  to  wait  for signals: pause(2), sigsuspend(2),
             sigtimedwait(2), and sigwaitinfo(2).

           * File   descriptor   multiplexing    interfaces:    epoll_wait(2),
             epoll_pwait(2), poll(2), ppoll(2), select(2), and pselect(2).

           * System V IPC interfaces: msgrcv(2), msgsnd(2), semop(2), and sem-
             timedop(2).

           * Sleep   interfaces:   clock_nanosleep(2),    nanosleep(2),    and
             usleep(3).

           * read(2) from an inotify(7) file descriptor.

           * io_getevents(2).

       The  sleep(3) function is also never restarted if interrupted by a han-
       dler, but gives a success return: the number of  seconds  remaining  to
       sleep.
           * Socket interfaces, when a timeout has  been  set  on  the  socket
             using   setsockopt(2):   accept(2),   recv(2),  recvfrom(2),  and
             recvmsg(2), if a receive timeout (SO_RCVTIMEO) has been set; con-
             nect(2),  send(2),  sendto(2),  and sendmsg(2), if a send timeout
             (SO_SNDTIMEO) has been set.

           * epoll_wait(2), epoll_pwait(2).

           * semop(2), semtimedop(2).

           * sigtimedwait(2), sigwaitinfo(2).

           * read(2) from an inotify(7) file descriptor.

           * Linux 2.6.21 and earlier: futex(2) FUTEX_WAIT,  sem_timedwait(3),
             sem_wait(3).

           * Linux 2.6.8 and earlier: msgrcv(2), msgsnd(2).

           * Linux 2.4 and earlier: nanosleep(2).
#endif
