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

#include "pidserver.h"

#include "error_.h"
#include "uid_.h"
#include "timekeeping_.h"

#include <unistd.h>
#include <stdlib.h>

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
static int
createPidServerClient_(struct PidServerClient_ *self,
                       struct UnixSocket       *aServer)
{
    int rc = -1;

    ERROR_IF(
        acceptUnixSocket(&self->mSocket_, aServer),
        {
            warn(errno, "Unable to accept connection");
        });
    self->mSocket = &self->mSocket_;

    ERROR_IF(
        ownUnixSocketPeerCred(self->mSocket, &self->mCred));

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            closeUnixSocket(self->mSocket);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
closePidServerClient_(struct PidServerClient_ *self)
{
    if (self)
        closeUnixSocket(self->mSocket);
}

/* -------------------------------------------------------------------------- */
int
createPidServer(struct PidServer *self)
{
    int rc = -1;

    self->mSocket           = 0;
    self->mSentinel.mSocket = 0;

    TAILQ_INIT(&self->mClients);
    TAILQ_INSERT_TAIL(&self->mClients, &self->mSentinel, mList);

    ensure(
        &self->mSentinel == TAILQ_FIRST(&self->mClients));
    ensure(
        &self->mSentinel == TAILQ_LAST(&self->mClients, PidServerClientList_));

    ERROR_IF(
        createUnixSocket(&self->mSocket_, 0, 0, 0));
    self->mSocket = &self->mSocket_;

    ERROR_IF(
        ownUnixSocketName(self->mSocket, &self->mSocketAddr));

    ERROR_IF(
        self->mSocketAddr.sun_path[0]);

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            closeUnixSocket(self->mSocket);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
void
closePidServer(struct PidServer *self)
{
    if (self)
    {
        closeUnixSocket(self->mSocket);

        while ( ! TAILQ_EMPTY(&self->mClients))
        {
            struct PidServerClient_ *client =
                TAILQ_FIRST(&self->mClients);

            TAILQ_REMOVE(&self->mClients, client, mList);

            closePidServerClient_(client);
        }
   }
}

/* -------------------------------------------------------------------------- */
int
acceptPidServerConnection(struct PidServer *self)
{
    int rc = -1;

    /* Accept a new connection from a client to hold an additional
     * reference to the child process group. If this is the first
     * reference, activate the janitor to periodically remove expired
     * references.
     *
     * Do not accept the new connection if there is no more memory
     * to store the connection record, but pause rather than allowing
     * the event loop to spin wildly. */

    struct PidServerClient_ *client_ = 0;
    struct PidServerClient_ *client  = 0;
    ERROR_UNLESS(
        (client_ = malloc(sizeof(*client_))),
        {
            monotonicSleep(Duration(NSECS(Seconds(1))));
            errno = ENOMEM;
        });

    ERROR_IF(
        createPidServerClient_(client_, self->mSocket));
    client  = client_;
    client_ = 0;

    ERROR_UNLESS(
        geteuid() == client->mCred.uid || ! client->mCred.uid,
        {
            warn(0,
                 "Discarding connection from %" PRIs_ucred,
                 FMTs_ucred(client->mCred));
        });

    char buf[1];

    int err;
    ERROR_IF(
        (err = writeFile(client->mSocket->mFile, buf, 1),
         -1 == err || (errno = EIO, 1 != err)));

    debug(0,
          "add reference from %" PRIs_ucred,
          FMTs_ucred(client->mCred));

    TAILQ_INSERT_TAIL(
        &self->mClients, client, mList);
    client = 0;

    rc = 0;

Finally:

    FINALLY
    ({
        closePidServerClient_(client);
        free(client_);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
bool
cleanPidServer(struct PidServer *self)
{
    /* This function is called to eriodically make a sweep
     * of the references to the child process group, and to remove
     * those references which have expired. */

    unsigned passSentinel = 2;
    unsigned clientLimit  = testAction(TestLevelRace) ? 1 : 100;

    struct Duration zeroDuration = Duration(NanoSeconds(0));

    struct PidServerClient_ *client = 0;

    while (1)
    {
        if (client)
        {
            TAILQ_INSERT_TAIL(&self->mClients, client, mList);
            client = 0;
        }

        if ( ! clientLimit)
            break;

        client = TAILQ_FIRST(&self->mClients);

        TAILQ_REMOVE(&self->mClients, client, mList);

        if ( ! client->mSocket)
        {
            if ( ! --passSentinel)
                clientLimit = 0;
            continue;
        }

        switch (waitFileReadReady(client->mSocket->mFile, &zeroDuration))
        {
        default:
            break;

        case -1:
            warn(errno,
                 "Unable to check connection from %" PRIs_ucred,
                 FMTs_ucred(client->mCred));
            break;

        case 1:

            /* Any activity on the connection that holds the reference
             * between the client and the keeper is sufficient to trigger
             * the keeper to drop the reference. */

            debug(0,
                  "drop reference from %" PRIs_ucred,
                  FMTs_ucred(client->mCred));

            closePidServerClient_(client);
            free(client);
            client = 0;
            break;
        }

        --clientLimit;
    }

    ensure( ! client);

    /* There is no further need to schedule the next run of the janitor
     * if there are no more outstanding connections. The last remaining
     * connection must be the sentinel. */

    bool cleaned = (
        TAILQ_FIRST(&self->mClients) ==
        TAILQ_LAST(&self->mClients, PidServerClientList_));

    ensure( ! cleaned ||
            &self->mSentinel == TAILQ_FIRST(&self->mClients));

    return cleaned;
}

/* -------------------------------------------------------------------------- */
