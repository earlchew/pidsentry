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
#include "deadline_.h"
#include "method_.h"

#include <unistd.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------- */
static CHECKED struct PidServerClientActivity_ *
closePidServerClientActivity_(struct PidServerClientActivity_ *self)
{
    if (self)
    {
        ensure( ! self->mList);

        self->mEvent = closeFileEventQueueActivity(self->mEvent);

        free(self);
    }

    return 0;
}

static CHECKED struct PidServerClientActivity_*
createPidServerClientActivity_(
    struct PidServerClient_               *aClient,
    struct FileEventQueue                 *aQueue,
    struct PidServerClientActivityMethod_  aMethod)
{
    int rc = -1;

    struct PidServerClientActivity_ *self = 0;

    ERROR_UNLESS(
        (self = malloc(sizeof(*self))));

    self->mList   = 0;
    self->mEvent  = 0;
    self->mClient = aClient;
    self->mMethod = aMethod;

    ERROR_IF(
        createFileEventQueueActivity(
            &self->mEvent_,
            aQueue,
            aClient->mUnixSocket->mSocket->mFile));
    self->mEvent = &self->mEvent_;

    ERROR_IF(
        armFileEventQueueActivity(
            self->mEvent,
            EventQueuePollRead,
            FileEventQueueActivityMethod(
                LAMBDA(
                    int, (struct PidServerClientActivity_ *self_),
                    {
                        return
                            callPidServerClientActivityMethod_(
                                self_->mMethod, self_);
                    }),
                self)));

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            self = closePidServerClientActivity_(self);
    });

    return self;
}

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
static CHECKED struct PidServerClient_ *
closePidServerClient_(struct PidServerClient_ *self)
{
    if (self)
    {
        self->mUnixSocket = closeUnixSocket(self->mUnixSocket);

        free(self);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
static CHECKED struct PidServerClient_ *
createPidServerClient_(struct UnixSocket *aSocket)
{
    int rc = -1;

    struct PidServerClient_ *self = 0;

    ERROR_UNLESS(
        (self = malloc(sizeof(*self))));

    ERROR_IF(
        acceptUnixSocket(&self->mUnixSocket_, aSocket),
        {
            warn(errno, "Unable to accept connection");
        });
    self->mUnixSocket = &self->mUnixSocket_;

    ERROR_IF(
        ownUnixSocketPeerCred(self->mUnixSocket, &self->mCred));

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
createPidServer(struct PidServer *self, struct Pid aPid)
{
    int rc = -1;

    self->mUnixSocket   = 0;
    self->mEventQueue   = 0;
    self->mPidSignature = 0;

    TAILQ_INIT(&self->mClients);

    ERROR_UNLESS(
        self->mPidSignature = createPidSignature(aPid, 0));

    debug(0,
          "create pid server for %" PRIs_Method,
          FMTs_Method(printPidSignature, self->mPidSignature));

    ERROR_IF(
        createUnixSocket(&self->mUnixSocket_, 0, 0, 0));
    self->mUnixSocket = &self->mUnixSocket_;

    ERROR_IF(
        ownUnixSocketName(self->mUnixSocket, &self->mSocketAddr));

    ERROR_IF(
        self->mSocketAddr.sun_path[0]);

    int numEvents = 16;

    ERROR_IF(
        createFileEventQueue(&self->mEventQueue_, numEvents));
    self->mEventQueue = &self->mEventQueue_;

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            self = closePidServer(self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
static CHECKED int
enqueuePidServerConnection_(struct PidServer                *self,
                            struct PidServerClientActivity_ *aActivity)
{
    int rc = -1;

    ensure( ! aActivity->mList);

    debug(0,
          "add reference from %" PRIs_ucred,
          FMTs_ucred(aActivity->mClient->mCred));

    TAILQ_INSERT_TAIL(
        &self->mClients, aActivity, mList_);

    aActivity->mList = &aActivity->mList_;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static CHECKED struct PidServerClientActivity_ *
discardPidServerConnection_(struct PidServer                *self,
                            struct PidServerClientActivity_ *aActivity)
{
    struct PidServerClientActivity_ *activity = aActivity;

    if (activity)
    {
        if (activity->mList)
        {
            debug(0,
                  "drop reference from %" PRIs_ucred,
                  FMTs_ucred(activity->mClient->mCred));

            TAILQ_REMOVE(&self->mClients, activity, mList_);
            activity->mList = 0;
        }

        struct PidServerClient_ *client = activity->mClient;

        activity = closePidServerClientActivity_(activity);
        client   = closePidServerClient_(client);
    }

    return activity;
}

/* -------------------------------------------------------------------------- */
struct PidServer *
closePidServer(struct PidServer *self)
{
    if (self)
    {
        while ( ! TAILQ_EMPTY(&self->mClients))
        {
            struct PidServerClientActivity_ *activity =
                TAILQ_FIRST(&self->mClients);

            activity = discardPidServerConnection_(self, activity);
        }

        self->mEventQueue   = closeFileEventQueue(self->mEventQueue);
        self->mUnixSocket   = closeUnixSocket(self->mUnixSocket);
        self->mPidSignature = destroyPidSignature(self->mPidSignature);
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

    struct PidServerClient_         *client    = 0;
    struct PidServerClientActivity_ *activity  = 0;
    struct PidSignature             *signature = 0;

    struct Deadline  deadline_;
    struct Deadline *deadline = 0;

    ERROR_UNLESS(
        (client = createPidServerClient_(self->mUnixSocket)));

    ERROR_UNLESS(
        geteuid() == client->mCred.uid || ! client->mCred.uid,
        {
            warn(0,
                 "Discarding connection from %" PRIs_ucred,
                 FMTs_ucred(client->mCred));
        });

    struct Duration pidSignatureTimeout = Duration(NSECS(Seconds(30)));

    ERROR_IF(
        createDeadline(&deadline_, &pidSignatureTimeout));
    deadline = &deadline_;

    ERROR_UNLESS(
        signature = recvPidSignature(
            client->mUnixSocket->mSocket->mFile, deadline));

    ERROR_IF(
        rankPidSignature(self->mPidSignature, signature),
        {
            warn(0,
                 "Discarding connection for %" PRIs_Method,
                 FMTs_Method(printPidSignature, signature));
        });

    ERROR_UNLESS(
        (activity = createPidServerClientActivity_(
            client,
            self->mEventQueue,
            PidServerClientActivityMethod_(
                LAMBDA(
                    int, (struct PidServer                *self_,
                          struct PidServerClientActivity_ *aActivity),
                    {
                        struct PidServerClientActivity_ *activity_ = aActivity;

                        activity_ =
                            discardPidServerConnection_(self_, activity_);

                        return 0;
                    }),
                self))));
    client = 0;

    char buf[1] = { 0 };

    int wrBytes;
    ERROR_IF(
        (wrBytes = writeSocket(
            activity->mClient->mUnixSocket->mSocket, buf, sizeof(buf), 0),
         -1 == wrBytes || (errno = EIO, sizeof(buf) != wrBytes)));

    ERROR_IF(
        enqueuePidServerConnection_(self, activity));
    activity = 0;

    rc = 0;

Finally:

    FINALLY
    ({
        signature = destroyPidSignature(signature);
        activity  = discardPidServerConnection_(self, activity);
        deadline  = closeDeadline(deadline);
        client    = closePidServerClient_(client);
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

    ERROR_IF(
        pollFileEventQueueActivity(self->mEventQueue, &ZeroDuration));

    /* There is no further need to continue cleaning if there are no
     * more outstanding connections. The last remaining connection
     * must be the sentinel. */

    rc = TAILQ_EMPTY(&self->mClients);

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
