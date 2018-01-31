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

#include "pidsignature_.h"

#include "ert/compiler.h"
#include "ert/unixsocket.h"
#include "ert/fileeventqueue.h"
#include "ert/queue.h"

#include <sys/un.h>
#include <sys/socket.h>

/* -------------------------------------------------------------------------- */
ERT_BEGIN_C_SCOPE;
struct PidServerClientActivity_;
ERT_END_C_SCOPE;

#define ERT_METHOD_DEFINITION
#define METHOD_RETURN_PidServerClientActivityMethod    int
#define METHOD_CONST_PidServerClientActivityMethod
#define METHOD_ARG_LIST_PidServerClientActivityMethod  \
    (struct PidServerClientActivity_ *aActivity_)
#define METHOD_CALL_LIST_PidServerClientActivityMethod \
    (aActivity_)

#define ERT_METHOD_NAME      PidServerClientActivityMethod_
#define ERT_METHOD_RETURN    METHOD_RETURN_PidServerClientActivityMethod
#define ERT_METHOD_CONST     METHOD_CONST_PidServerClientActivityMethod
#define ERT_METHOD_ARG_LIST  METHOD_ARG_LIST_PidServerClientActivityMethod
#define ERT_METHOD_CALL_LIST METHOD_CALL_LIST_PidServerClientActivityMethod
#include "ert/method.h"

#define PidServerClientActivityMethod_(Object_, Method_)        \
    ERT_METHOD_TRAMPOLINE(                                      \
        Object_, Method_,                                       \
        PidServerClientActivityMethod__,                        \
        METHOD_RETURN_PidServerClientActivityMethod,            \
        METHOD_CONST_PidServerClientActivityMethod,             \
        METHOD_ARG_LIST_PidServerClientActivityMethod,          \
        METHOD_CALL_LIST_PidServerClientActivityMethod)

/* -------------------------------------------------------------------------- */
ERT_BEGIN_C_SCOPE;

struct PidServer;

/* -------------------------------------------------------------------------- */
struct PidServerClient_
{
    struct ucred mCred;

    struct Ert_UnixSocket  mUnixSocket_;
    struct Ert_UnixSocket *mUnixSocket;
};

/* -------------------------------------------------------------------------- */
struct PidServerClientActivity_;
typedef TAILQ_ENTRY(PidServerClientActivity_) PidServerClientActivityListEntryT;

struct PidServerClientActivity_
{
    PidServerClientActivityListEntryT  mList_;
    PidServerClientActivityListEntryT *mList;

    struct Ert_FileEventQueueActivity  mEvent_;
    struct Ert_FileEventQueueActivity *mEvent;

    struct PidServerClient_              *mClient;
    struct PidServerClientActivityMethod_ mMethod;
};

typedef TAILQ_HEAD(PidServerClientActivityList_,
                   PidServerClientActivity_) PidServerClientActivityListT_;

/* -------------------------------------------------------------------------- */
struct PidServer
{
    struct Ert_UnixSocket  mUnixSocket_;
    struct Ert_UnixSocket *mUnixSocket;
    struct sockaddr_un mSocketAddr;

    struct Ert_FileEventQueue  mEventQueue_;
    struct Ert_FileEventQueue *mEventQueue;

    struct PidSignature *mPidSignature;

    struct PidServerClientActivityList_ mClients;
};

/* -------------------------------------------------------------------------- */
ERT_CHECKED int
createPidServer(struct PidServer *self, struct Ert_Pid aPid);

struct PidServer *
closePidServer(struct PidServer *self);

ERT_CHECKED int
acceptPidServerConnection(struct PidServer *self);

ERT_CHECKED int
cleanPidServer(struct PidServer *self);

/* -------------------------------------------------------------------------- */

ERT_END_C_SCOPE;

#endif /* PIDSERVER_H */
