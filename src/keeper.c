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

#include "keeper.h"
#include "error_.h"
#include "socketpair_.h"
#include "unixsocket_.h"
#include "thread_.h"
#include "macros_.h"
#include "pipe_.h"
#include "type_.h"
#include "fd_.h"
#include "pollfd_.h"
#include "uid_.h"

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <poll.h>

#include <sys/queue.h>

/* -------------------------------------------------------------------------- */
enum PollFdKeeperKind
{
    POLL_FD_KEEPER_TETHER,
    POLL_FD_KEEPER_SERVER,
    POLL_FD_KEEPER_KINDS
};

static const char *pollFdNames_[POLL_FD_KEEPER_KINDS] =
{
    [POLL_FD_KEEPER_TETHER] = "keeper tether",
    [POLL_FD_KEEPER_SERVER] = "keeper server",
};

/* -------------------------------------------------------------------------- */
enum PollFdKeeperTimerKind
{
    POLL_FD_KEEPER_TIMER_JANITOR,
    POLL_FD_KEEPER_TIMER_KINDS
};

static const char *pollFdTimerNames_[POLL_FD_KEEPER_TIMER_KINDS] =
{
    [POLL_FD_KEEPER_TIMER_JANITOR] = "keeper janitor",
};

static const struct Type * const keeperMonitorType_ = TYPE("KeeperMonitor");

/* -------------------------------------------------------------------------- */
#define PRIs_ucred "s"                  \
                   "uid %" PRId_Uid " " \
                   "gid %" PRId_Gid " " \
                   "pid %" PRId_Pid

#define FMTs_ucred(Ucred)        \
    "",                          \
    FMTd_Uid(Uid((Ucred).uid)),  \
    FMTd_Gid(Gid((Ucred).gid)),  \
    FMTd_Pid(Pid((Ucred).pid))

/* -------------------------------------------------------------------------- */
struct KeeperClient
{
    TAILQ_ENTRY(KeeperClient) mList;

    struct ucred mCred;

    struct UnixSocket  mSocket_;
    struct UnixSocket *mSocket;
};

typedef TAILQ_HEAD(KeeperClientList, KeeperClient) KeeperClientListT;

struct KeeperMonitor
{
    const struct Type *mType;

    struct Pipe *mNullPipe;

    struct
    {
        struct SocketPair *mSocket;
    } mTether;

    struct
    {
        struct UnixSocket      *mSocket;
        struct KeeperClientList mClientList;
    } mServer;

    struct
    {
        void *mNone;
    } mJanitor;

    struct pollfd            mPollFds[POLL_FD_KEEPER_KINDS];
    struct PollFdAction      mPollFdActions[POLL_FD_KEEPER_KINDS];
    struct PollFdTimerAction mPollFdTimerActions[POLL_FD_KEEPER_TIMER_KINDS];
};

/* -------------------------------------------------------------------------- */
int
createKeeperProcess(
    struct KeeperProcess *self,
    struct Pgid           aPgid)
{
    int rc = -1;

    self->mPid  = Pid(0);
    self->mPgid = aPgid;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closeKeeperProcess(
    struct KeeperProcess *self)
{
}

/* -------------------------------------------------------------------------- */
static void
pollFdTether_(void                        *self_,
              const struct EventClockTime *aPollTime)
{
    struct KeeperMonitor *self = self_;

    ensure(keeperMonitorType_ == self->mType);

    /* When the watchdog terminates, it shuts down its end of the keeper tether,
     * which is detected by the keeper here. Respond by removing the server
     * from the poll loop so that it will no longer respond to any
     * attempts to make new connections. */

    struct pollfd *serverPollFd =
        &self->mPollFds[POLL_FD_KEEPER_SERVER];

    serverPollFd->fd     = self->mNullPipe->mRdFile->mFd;
    serverPollFd->events = 0;

    struct pollfd *tetherPollFd =
        &self->mPollFds[POLL_FD_KEEPER_TETHER];

    tetherPollFd->fd     = self->mNullPipe->mRdFile->mFd;
    tetherPollFd->events = 0;
}

/* -------------------------------------------------------------------------- */
static void
pollFdServer_(void                        *self_,
              const struct EventClockTime *aPollTime)
{
    struct KeeperMonitor *self = self_;

    ensure(keeperMonitorType_ == self->mType);

    /* Accept a new connection from a client to hold an additional
     * reference to the child process group. If this is the first
     * reference, activate the janitor to periodically remove expired
     * references.
     *
     * Do not accept the new connection if there is no more memory
     * to store the connection record, but pause rather than allowing
     * the event loop to spin wildly. */

    struct KeeperClient *keeperClient = 0;
    ERROR_UNLESS(
        (keeperClient = malloc(sizeof(*keeperClient))),
        {
            monotonicSleep(Duration(NSECS(Seconds(1))));
            errno = ENOMEM;
        });

    ERROR_IF(
        acceptUnixSocket(
            &keeperClient->mSocket_, self->mServer.mSocket),
        {
            warn(errno, "Unable to accept connection");
        });
    keeperClient->mSocket = &keeperClient->mSocket_;

    ERROR_IF(
        ownUnixSocketPeerCred(keeperClient->mSocket, &keeperClient->mCred));

    ERROR_UNLESS(
        geteuid() == keeperClient->mCred.uid || ! keeperClient->mCred.uid,
        {
            debug(0,
                  "discarding connection from %" PRIs_ucred,
                  FMTs_ucred(keeperClient->mCred));
        });

    char buf[1];

    int err;
    ERROR_IF(
        (err = writeFile(keeperClient->mSocket->mFile, buf, 1),
         -1 == err || (errno = EIO, 1 != err)));

    debug(0,
          "add reference from %" PRIs_ucred,
          FMTs_ucred(keeperClient->mCred));

    TAILQ_INSERT_TAIL(
        &self->mServer.mClientList, keeperClient, mList);
    keeperClient = 0;

    struct PollFdTimerAction *janitorTimerAction =
        &self->mPollFdTimerActions[POLL_FD_KEEPER_TIMER_JANITOR];

    if ( ! janitorTimerAction->mPeriod.duration.ns)
        janitorTimerAction->mPeriod = Duration(NSECS(Seconds(5)));

Finally:

    FINALLY
    ({
        if (keeperClient)
        {
            closeUnixSocket(keeperClient->mSocket);
            free(keeperClient);
        }
    });
}

/* -------------------------------------------------------------------------- */
static void
pollFdTimerJanitor_(void                        *self_,
                    const struct EventClockTime *aPollTime)
{
    struct KeeperMonitor *self = self_;

    ensure(keeperMonitorType_ == self->mType);

    /* The role of the janitor is to periodically make a sweep
     * of the references to the child process group, and to remove
     * those references which have expired. */

    unsigned passSentinel = 2;
    unsigned clientLimit  = testAction(TestLevelRace) ? 1 : 100;

    struct Duration zeroDuration = Duration(NanoSeconds(0));

    struct KeeperClient *keeperClient = 0;

    while (1)
    {
        if (keeperClient)
        {
            TAILQ_INSERT_TAIL(
                &self->mServer.mClientList,
                keeperClient,
                mList);
            keeperClient = 0;
        }

        if ( ! clientLimit)
            break;

        keeperClient =
            TAILQ_FIRST(&self->mServer.mClientList);

        TAILQ_REMOVE(&self->mServer.mClientList, keeperClient, mList);

        if ( ! keeperClient->mSocket)
        {
            if ( ! --passSentinel)
                clientLimit = 0;
            continue;
        }

        switch (waitFileReadReady(keeperClient->mSocket->mFile,
                                  &zeroDuration))
        {
        default:
            break;

        case -1:
            warn(errno,
                 "Unable to check connection from %" PRIs_ucred,
                 FMTs_ucred(keeperClient->mCred));
            break;

        case 1:

            /* Any activity on the connection that holds the reference
             * between the client and the keeper is sufficient to trigger
             * the keeper to drop the reference. */

            debug(0,
                  "drop reference from %" PRIs_ucred,
                  FMTs_ucred(keeperClient->mCred));

            closeUnixSocket(keeperClient->mSocket);
            free(keeperClient);
            keeperClient = 0;
            break;
        }

        --clientLimit;
    }

    ensure( ! keeperClient);

    /* There is no further need to schedule the next run of the janitor
     * if there are no more outstanding connections. */

    if (TAILQ_FIRST(&self->mServer.mClientList) ==
        TAILQ_LAST(&self->mServer.mClientList, KeeperClientList))
    {
        struct PollFdTimerAction *janitorTimerAction =
            &self->mPollFdTimerActions[POLL_FD_KEEPER_TIMER_JANITOR];

        janitorTimerAction->mPeriod = Duration(NanoSeconds(0));
    }
}

/* -------------------------------------------------------------------------- */
static bool
pollFdCompletion_(void *self_)
{
    struct KeeperMonitor *self = self_;

    ensure(keeperMonitorType_ == self->mType);

    struct pollfd *serverPollFd =
        &self->mPollFds[POLL_FD_KEEPER_SERVER];

    struct PollFdTimerAction *janitorTimerAction =
        &self->mPollFdTimerActions[POLL_FD_KEEPER_TIMER_JANITOR];

    return
        ! serverPollFd->events &&
        ! janitorTimerAction->mPeriod.duration.ns;
}

/* -------------------------------------------------------------------------- */
static void
runKeeperProcess_(
    struct KeeperProcess *self,
    struct SocketPair    *aKeeperTether,
    struct UnixSocket    *aServerSocket)
{
    debug(0,
          "running keeper pid %" PRId_Pid " in pgid %" PRId_Pgid,
          FMTd_Pid(ownProcessId()),
          FMTd_Pgid(ownProcessGroupId()));

    closeSocketPairParent(aKeeperTether);

    struct Pid keptPid;
    ABORT_IF(
        (keptPid = forkProcess(ForkProcessSetProcessGroup, self->mPgid),
         -1 == keptPid.mPid));

    if ( ! keptPid.mPid)
    {
        debug(0,
              "holding kept pid %" PRId_Pid " in pgid %" PRId_Pgid,
              FMTd_Pid(ownProcessId()),
              FMTd_Pgid(ownProcessGroupId()));

        quitProcess(EXIT_SUCCESS);
    }

    int whiteList[] =
    {
        STDIN_FILENO,
        STDOUT_FILENO,
        STDERR_FILENO,
        ownProcessLockFile()->mFd,
        aKeeperTether->mChildFile->mFd,
        aServerSocket->mFile->mFd,
    };

    closeFdDescriptors(whiteList, NUMBEROF(whiteList));

    struct Pipe nullPipe;
    ABORT_IF(
        createPipe(&nullPipe, O_CLOEXEC | O_NONBLOCK));

    struct KeeperMonitor keeperMonitor =
    {
        .mType = keeperMonitorType_,

        .mNullPipe = &nullPipe,

        .mTether =
        {
            .mSocket = aKeeperTether,
        },

        .mServer =
        {
            .mSocket = aServerSocket,
            .mClientList = TAILQ_HEAD_INITIALIZER(
                keeperMonitor.mServer.mClientList),
        },

        .mJanitor =
        {
            .mNone = 0,
        },

        .mPollFds =
        {
            [POLL_FD_KEEPER_TETHER] =
            {
                .fd     = aKeeperTether->mChildFile->mFd,
                .events = POLL_INPUTEVENTS,
            },

            [POLL_FD_KEEPER_SERVER] =
            {
                .fd     = aServerSocket->mFile->mFd,
                .events = POLL_INPUTEVENTS,
            },
        },

        .mPollFdActions =
        {
            [POLL_FD_KEEPER_TETHER] = { pollFdTether_ },
            [POLL_FD_KEEPER_SERVER] = { pollFdServer_ },
        },

        .mPollFdTimerActions =
        {
            [POLL_FD_KEEPER_TIMER_JANITOR] =
            {
                .mAction = pollFdTimerJanitor_,
                .mSince  = EVENTCLOCKTIME_INIT,
                .mPeriod = Duration(NanoSeconds(0)),
            },
        },

    };

    struct KeeperClient sentinelClient = { .mSocket = 0 };

    TAILQ_INSERT_TAIL(
        &keeperMonitor.mServer.mClientList, &sentinelClient, mList);

    ensure(
        &sentinelClient == TAILQ_FIRST(&keeperMonitor.mServer.mClientList));
    ensure(
        &sentinelClient == TAILQ_LAST(&keeperMonitor.mServer.mClientList,
                                      KeeperClientList));

    struct PollFd pollfd;
    ABORT_IF(
        createPollFd(
            &pollfd,

            keeperMonitor.mPollFds,
            keeperMonitor.mPollFdActions,
            pollFdNames_, POLL_FD_KEEPER_KINDS,

            keeperMonitor.mPollFdTimerActions,
            pollFdTimerNames_, POLL_FD_KEEPER_TIMER_KINDS,

            pollFdCompletion_, &keeperMonitor));

    /* Now that the keeper process has initialised, allow the watchdog
     * to continue execution. */

    char buf[1] = { 0 };

    int err;
    ABORT_IF(
        (err = writeFile(aKeeperTether->mChildFile, buf, 1),
         -1 == err || (errno = EIO, 1 != err)));

    ABORT_IF(
        runPollFdLoop(&pollfd));

    ensure(
        &sentinelClient == TAILQ_FIRST(&keeperMonitor.mServer.mClientList));
    ensure(
        &sentinelClient == TAILQ_LAST(&keeperMonitor.mServer.mClientList,
                                      KeeperClientList));

    closePollFd(&pollfd);

    int status;
    ABORT_IF(
        reapProcess(keptPid, &status));

    ABORT_UNLESS(
        WIFEXITED(status) && ! WEXITSTATUS(status));

    debug(0, "exit keeper");

    quitProcess(EXIT_SUCCESS);
}

/* -------------------------------------------------------------------------- */
int
forkKeeperProcess(
    struct KeeperProcess *self,
    struct SocketPair    *aKeeperTether,
    struct UnixSocket    *aServerSocket)
{
    int rc = -1;

    struct ThreadSigMask threadSigMask;
    pushThreadSigMask(&threadSigMask, ThreadSigMaskUnblock, 0);

    struct Pid daemonPid = Pid(-1);
    ERROR_IF(
        (daemonPid = forkProcessDaemon(),
         -1 == daemonPid.mPid));

    if ( ! daemonPid.mPid)
    {
        runKeeperProcess_(self, aKeeperTether, aServerSocket);
        quitProcess(EXIT_SUCCESS);
    }

    ERROR_UNLESS(
        1 == waitFileReadReady(aKeeperTether->mParentFile, 0));

    char buf[1];

    int err;
    ERROR_IF(
        (err = readFile(aKeeperTether->mParentFile, buf, 1),
         -1 == err || (errno = EIO, 1 != err)));

    self->mPid = daemonPid;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            if (-1 != daemonPid.mPid)
                ABORT_IF(
                    kill(daemonPid.mPid, SIGKILL),
                    {
                        terminate(
                            errno,
                            "Unable to kill keeper pid %" PRId_Pid,
                            FMTd_Pid(daemonPid));
                    });
        }

        popThreadSigMask(&threadSigMask);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
