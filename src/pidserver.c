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
static struct PidServerClient_ *
closePidServerClient_(struct PidServerClient_ *self)
{
    if (self)
    {
        self->mEvent  = closeEventQueueFile(self->mEvent);
        self->mSocket = closeUnixSocket(self->mSocket);

        free(self);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
static struct PidServerClient_ *
createPidServerClient_(struct PidServer *aServer)
{
    int rc = -1;

    struct PidServerClient_ *self = 0;

    ERROR_UNLESS(
        (self = malloc(sizeof(*self))));

    self->mServer = aServer;
    self->mEvent  = 0;

    ERROR_IF(
        acceptUnixSocket(&self->mSocket_, self->mServer->mSocket),
        {
            warn(errno, "Unable to accept connection");
        });
    self->mSocket = &self->mSocket_;

    ERROR_IF(
        ownUnixSocketPeerCred(self->mSocket, &self->mCred));

    ERROR_IF(
        createEventQueueFile(
            &self->mEvent_,
            self->mServer->mEventQueue,
            self->mSocket->mFile,
            EventQueuePollRead,
            EventQueueHandle(self)));
    self->mEvent = &self->mEvent_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            self = closePidServerClient_(self);
    });

    return self;
}

/* -------------------------------------------------------------------------- */
int
createPidServer(struct PidServer *self)
{
    int rc = -1;

    self->mSocket     = 0;
    self->mEventQueue = 0;

    TAILQ_INIT(&self->mClients);

    ERROR_IF(
        createUnixSocket(&self->mSocket_, 0, 0, 0));
    self->mSocket = &self->mSocket_;

    ERROR_IF(
        ownUnixSocketName(self->mSocket, &self->mSocketAddr));

    ERROR_IF(
        self->mSocketAddr.sun_path[0]);

    ERROR_IF(
        createEventQueue(&self->mEventQueue_));
    self->mEventQueue = &self->mEventQueue_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
        {
            self->mEventQueue = closeEventQueue(self->mEventQueue);
            self->mSocket     = closeUnixSocket(self->mSocket);
        }
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
struct PidServer *
closePidServer(struct PidServer *self)
{
    if (self)
    {
        while ( ! TAILQ_EMPTY(&self->mClients))
        {
            struct PidServerClient_ *client =
                TAILQ_FIRST(&self->mClients);

            TAILQ_REMOVE(&self->mClients, client, mList);

            client = closePidServerClient_(client);
        }

        self->mEventQueue = closeEventQueue(self->mEventQueue);
        self->mSocket     = closeUnixSocket(self->mSocket);
   }

    return 0;
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

    struct PidServerClient_ *client  = 0;

    ERROR_UNLESS(
        (client = createPidServerClient_(self)));

    ERROR_UNLESS(
        geteuid() == client->mCred.uid || ! client->mCred.uid,
        {
            warn(0,
                 "Discarding connection from %" PRIs_ucred,
                 FMTs_ucred(client->mCred));
        });

    ERROR_IF(
        pushEventQueue(self->mEventQueue, client->mEvent));

    char buf[1] = { 0 };

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
        client = closePidServerClient_(client);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
int
cleanPidServer(struct PidServer *self)
{
    int rc = -1;

    /* This function is called to process activity on the event
     * queue, and remove those references to the child process group
     * that have expired. */

    struct EventQueueFile *events[16];

    struct Duration zeroTimeout = Duration(NanoSeconds(0));

    int poppedEvents;
    ERROR_IF(
        (poppedEvents = popEventQueue(
            self->mEventQueue,
            events, NUMBEROF(events), &zeroTimeout),
         -1 == poppedEvents));

    for (int px = 0; px < poppedEvents; ++px)
    {
        struct PidServerClient_ *client = events[px]->mSubject.mHandle;

        debug(0,
              "drop reference from %" PRIs_ucred,
              FMTs_ucred(client->mCred));

        TAILQ_REMOVE(&self->mClients, client, mList);

        client = closePidServerClient_(client);
    }

    /* There is no further need to continue cleaning if there are no
     * more outstanding connections. The last remaining connection
     * must be the sentinel. */

    rc = TAILQ_EMPTY(&self->mClients);

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
