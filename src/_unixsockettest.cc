/* -*- c-basic-offset:4; indent-tabs-mode:nil -*- vi: set sw=4 et: */
/*
// Copyright (c) 2015, Earl Chew
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

#include "unixsocket_.h"
#include "fd_.h"
#include "timekeeping_.h"

#include <sys/un.h>

#include "gtest/gtest.h"

TEST(UnixSocketTest, AbstractServerCollision)
{
    const char abstractName[1] = { 0 };

    struct UnixSocket serversock1;

    srandom(getpid());
    EXPECT_EQ(0, createUnixSocket(&serversock1,
                                  abstractName, sizeof(abstractName), 0));

    struct UnixSocket serversock2;

    srandom(getpid());
    EXPECT_EQ(-1, createUnixSocket(&serversock2,
                                  abstractName, sizeof(abstractName), 0));
    EXPECT_EQ(EADDRINUSE, errno);

    EXPECT_EQ(0, createUnixSocket(&serversock2,
                                  abstractName, sizeof(abstractName), 0));
}

TEST(UnixSocketTest, AbstractServer)
{
    struct UnixSocket serversock;

    EXPECT_EQ(0, createUnixSocket(&serversock, 0, 0, 0));

    struct sockaddr_un name;
    EXPECT_EQ(0, ownUnixSocketName(&serversock, &name));
    EXPECT_EQ(0, name.sun_path[0]);
    for (unsigned ix = 1; sizeof(name.sun_path) > ix; ++ix)
        EXPECT_TRUE(strchr("0123456789abcdef", name.sun_path[ix]));

    struct UnixSocket clientsock;

    int rc = connectUnixSocket(
        &clientsock, name.sun_path, sizeof(name.sun_path));
    EXPECT_TRUE(0 == rc || EINPROGRESS == rc);

    struct UnixSocket peersock;

    const struct Duration zeroTimeout = duration(NanoSeconds(0));

    EXPECT_EQ(0, acceptUnixSocket(&peersock, &serversock));
    EXPECT_EQ(1, waitUnixSocketWriteReady(&clientsock, &zeroTimeout));

    struct ucred cred;

    memset(&cred, -1, sizeof(cred));
    EXPECT_EQ(0, ownUnixSocketPeerCred(&peersock, &cred));
    EXPECT_EQ(getpid(), cred.pid);
    EXPECT_EQ(getuid(), cred.uid);
    EXPECT_EQ(getgid(), cred.gid);

    memset(&cred, -1, sizeof(cred));
    EXPECT_EQ(0, ownUnixSocketPeerCred(&clientsock, &cred));
    EXPECT_EQ(getpid(), cred.pid);
    EXPECT_EQ(getuid(), cred.uid);
    EXPECT_EQ(getgid(), cred.gid);

    int err;
    EXPECT_EQ(0, ownUnixSocketError(&clientsock, &err));
    EXPECT_EQ(0, err);

    char buf[1];

    buf[0] = 'X';
    EXPECT_EQ(1, sendUnixSocket(&clientsock, buf, sizeof(buf)));
    EXPECT_EQ(1, recvUnixSocket(&peersock, buf, sizeof(buf)));
    EXPECT_EQ('X', buf[0]);

    buf[0] = 'Z';
    EXPECT_EQ(1, sendUnixSocket(&peersock, buf, sizeof(buf)));
    EXPECT_EQ(1, recvUnixSocket(&clientsock, buf, sizeof(buf)));
    EXPECT_EQ('Z', buf[0]);

    EXPECT_EQ(0, closeUnixSocket(&clientsock));
    EXPECT_EQ(0, closeUnixSocket(&peersock));
    EXPECT_EQ(0, closeUnixSocket(&serversock));
}

#include "../googletest/src/gtest_main.cc"
