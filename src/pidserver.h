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
#ifndef PIDSERVER_H
#define PIDSERVER_H

#include "unixsocket_.h"
#include "eventqueue_.h"

#include <sys/un.h>
#include <sys/queue.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PidServer;

/* -------------------------------------------------------------------------- */
struct PidServerClient_
{
    TAILQ_ENTRY(PidServerClient_) mList;

    struct PidServer *mServer;

    struct ucred mCred;

    struct UnixSocket  mSocket_;
    struct UnixSocket *mSocket;

    struct EventQueueFile  mEvent_;
    struct EventQueueFile *mEvent;
};

typedef TAILQ_HEAD(PidServerClientList_,
                   PidServerClient_) PidServerClientListT_;

/* -------------------------------------------------------------------------- */
struct PidServer
{
    struct UnixSocket  mSocket_;
    struct UnixSocket *mSocket;
    struct sockaddr_un mSocketAddr;

    struct EventQueue  mEventQueue_;
    struct EventQueue *mEventQueue;

    struct PidServerClientList_ mClients;
};

/* -------------------------------------------------------------------------- */
int
createPidServer(struct PidServer *self);

void
closePidServer(struct PidServer *self);

int
acceptPidServerConnection(struct PidServer *self);

int
cleanPidServer(struct PidServer *self);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* PIDSERVER_H */
