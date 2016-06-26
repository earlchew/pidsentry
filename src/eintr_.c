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
#include "test_.h"

#include <unistd.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------- */
static unsigned moduleInit_;

/* -------------------------------------------------------------------------- */
#define EINTR_FUNCTION_DEFN_(Enum_, Return_, Name_, Signature_, Args_)  \
                                                                        \
Return_                                                                 \
Name_ Signature_                                                        \
{                                                                       \
    SYSCALL_RESTART(Enum_, Name_, Args_);                               \
}                                                                       \
                                                                        \
Return_                                                                 \
Name_ ## _eintr Signature_                                              \
{                                                                       \
    return                                                              \
        SYSCALL_EINTR(Enum_, Name_) Args_;                              \
}                                                                       \
                                                                        \
struct EintrModule

/* -------------------------------------------------------------------------- */
/* Interrupted System Calls
 *
 * These interceptors provide a way to inject EINTR to obtain substantially
 * more test coverage when unit tests are run. */

struct SystemCall
{
    const char *mName;
    uintptr_t   mAddr;
};

enum SystemCallKind
{
    SYSTEMCALL_PREAD,
    SYSTEMCALL_PREADV,
    SYSTEMCALL_PWRITE,
    SYSTEMCALL_PWRITEV,
    SYSTEMCALL_READ,
    SYSTEMCALL_READV,
    SYSTEMCALL_WRITE,
    SYSTEMCALL_WRITEV,
    SYSTEMCALL_KINDS
};

static struct SystemCall systemCall_[SYSTEMCALL_KINDS] =
{
    [SYSTEMCALL_PREAD]   = { "pread" },
    [SYSTEMCALL_PREADV]  = { "preadv" },
    [SYSTEMCALL_PWRITE]  = { "pwrite" },
    [SYSTEMCALL_PWRITEV] = { "pwritev" },
    [SYSTEMCALL_READ]    = { "read" },
    [SYSTEMCALL_READV]   = { "readv" },
    [SYSTEMCALL_WRITE]   = { "write" },
    [SYSTEMCALL_WRITEV]  = { "writev" },
};

/* -------------------------------------------------------------------------- */
#define SYSCALL_EINTR(Kind_, Function_)                         \
    ({                                                          \
        uintptr_t syscall_ = interruptSystemCall((Kind_));      \
                                                                \
        if ( ! syscall_)                                        \
        {                                                       \
            errno = EINTR;                                      \
            return -1;                                          \
        }                                                       \
                                                                \
        (DECLTYPE(Function_) *) syscall_;                       \
    })

/* -------------------------------------------------------------------------- */
#define SYSCALL_RESTART(Kind_, Function_, Args_)                \
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
EINTR_FUNCTION_DEFN_(
    SYSTEMCALL_PREAD,
    ssize_t,
    pread,
    (int aFd, void *aBuf, size_t aCount, off_t aOffset),
    (aFd, aBuf, aCount, aOffset));

EINTR_FUNCTION_DEFN_(
    SYSTEMCALL_PWRITE,
    ssize_t,
    pwrite,
    (int aFd, const void *aBuf, size_t aCount, off_t aOffset),
    (aFd, aBuf, aCount, aOffset));

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    SYSTEMCALL_READ,
    ssize_t,
    read,
    (int aFd, void *aBuf, size_t aCount),
    (aFd, aBuf, aCount));

EINTR_FUNCTION_DEFN_(
    SYSTEMCALL_WRITE,
    ssize_t,
    write,
    (int aFd, const void *aBuf, size_t aCount),
    (aFd, aBuf, aCount));

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    SYSTEMCALL_READV,
    ssize_t,
    readv,
    (int aFd, const struct iovec *aVec, int aCount),
    (aFd, aVec, aCount));

EINTR_FUNCTION_DEFN_(
    SYSTEMCALL_WRITEV,
    ssize_t,
    writev,
    (int aFd, const struct iovec *aVec, int aCount),
    (aFd, aVec, aCount));

/* -------------------------------------------------------------------------- */
EINTR_FUNCTION_DEFN_(
    SYSTEMCALL_PREADV,
    ssize_t,
    preadv,
    (int aFd, const struct iovec *aVec, int aCount, off_t aOffset),
    (aFd, aVec, aCount, aOffset));

EINTR_FUNCTION_DEFN_(
    SYSTEMCALL_PWRITEV,
    ssize_t,
    pwritev,
    (int aFd, const struct iovec *aVec, int aCount, off_t aOffset),
    (aFd, aVec, aCount, aOffset));

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
