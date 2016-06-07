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

#include "compiler_.h"
#include "unixsocket_.h"
#include "fileeventqueue_.h"
#include "pidsignature_.h"

#include <sys/un.h>
#include <sys/queue.h>

/* -------------------------------------------------------------------------- */
BEGIN_C_SCOPE;
struct PidServerClientActivity_;
END_C_SCOPE;

#define METHOD_DEFINITION
#define METHOD_RETURN_PidServerClientActivityMethod    int
#define METHOD_CONST_PidServerClientActivityMethod
#define METHOD_ARG_LIST_PidServerClientActivityMethod  \
    (struct PidServerClientActivity_ *aActivity_)
#define METHOD_CALL_LIST_PidServerClientActivityMethod \
    (aActivity_)

#define METHOD_NAME      PidServerClientActivityMethod_
#define METHOD_RETURN    METHOD_RETURN_PidServerClientActivityMethod
#define METHOD_CONST     METHOD_CONST_PidServerClientActivityMethod
#define METHOD_ARG_LIST  METHOD_ARG_LIST_PidServerClientActivityMethod
#define METHOD_CALL_LIST METHOD_CALL_LIST_PidServerClientActivityMethod
#include "method_.h"

#define PidServerClientActivityMethod_(Method_, Object_)        \
    METHOD_TRAMPOLINE(                                          \
        Method_, Object_,                                       \
        PidServerClientActivityMethod__,                        \
        METHOD_RETURN_PidServerClientActivityMethod,            \
        METHOD_CONST_PidServerClientActivityMethod,             \
        METHOD_ARG_LIST_PidServerClientActivityMethod,          \
        METHOD_CALL_LIST_PidServerClientActivityMethod)

/* -------------------------------------------------------------------------- */
BEGIN_C_SCOPE;

struct PidServer;

/* -------------------------------------------------------------------------- */
struct PidServerClient_
{
    struct ucred mCred;

    struct UnixSocket  mSocket_;
    struct UnixSocket *mSocket;
};

/* -------------------------------------------------------------------------- */
struct PidServerClientActivity_;
typedef TAILQ_ENTRY(PidServerClientActivity_) PidServerClientActivityListEntryT;

struct PidServerClientActivity_
{
    PidServerClientActivityListEntryT  mList_;
    PidServerClientActivityListEntryT *mList;

    struct FileEventQueueActivity  mEvent_;
    struct FileEventQueueActivity *mEvent;

    struct PidServerClient_              *mClient;
    struct PidServerClientActivityMethod_ mMethod;
};

typedef TAILQ_HEAD(PidServerClientActivityList_,
                   PidServerClientActivity_) PidServerClientActivityListT_;

/* -------------------------------------------------------------------------- */
struct PidServer
{
    struct UnixSocket  mSocket_;
    struct UnixSocket *mSocket;
    struct sockaddr_un mSocketAddr;

    struct FileEventQueue  mEventQueue_;
    struct FileEventQueue *mEventQueue;

    struct PidSignature *mPidSignature;

    struct PidServerClientActivityList_ mClients;
};

/* -------------------------------------------------------------------------- */
CHECKED int
createPidServer(struct PidServer *self, struct Pid aPid);

struct PidServer *
closePidServer(struct PidServer *self);

CHECKED int
acceptPidServerConnection(struct PidServer *self);

CHECKED int
cleanPidServer(struct PidServer *self);

/* -------------------------------------------------------------------------- */

END_C_SCOPE;

#endif /* PIDSERVER_H */
