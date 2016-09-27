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

#include "fd_.h"
#include "pipe_.h"
#include "fdset_.h"

#include "gtest/gtest.h"

#include <limits.h>

#include <sys/resource.h>

TEST(FdTest, ReadFully)
{
    {
        char *buf = 0;

        EXPECT_EQ(-1, readFdFully(-1, &buf, 0));
        EXPECT_EQ(0,  buf);
    }

    {
        char  buf_;
        char *buf = &buf_;

        struct Pipe  pipe_;
        struct Pipe *pipe = 0;

        EXPECT_EQ(0, createPipe(&pipe_, 0));
        pipe = &pipe_;

        closePipeWriter(pipe);

        EXPECT_EQ(0, readFdFully(pipe->mRdFile->mFd, &buf, 0));
        EXPECT_EQ(0, buf);

        pipe = closePipe(pipe);
    }

    {
        char  buf_;
        char *buf = &buf_;

        struct Pipe  pipe_;
        struct Pipe *pipe = 0;

        EXPECT_EQ(0, createPipe(&pipe_, 0));
        pipe = &pipe_;

        EXPECT_EQ(1, writeFd(pipe->mWrFile->mFd, "1", 1, 0));
        closePipeWriter(pipe);

        EXPECT_EQ(1, readFdFully(pipe->mRdFile->mFd, &buf, 0));
        EXPECT_EQ(0, strncmp("1", buf, 1));

        free(buf);

        pipe = closePipe(pipe);
    }

    {
        char  buf_;
        char *buf = &buf_;

        struct Pipe  pipe_;
        struct Pipe *pipe = 0;

        EXPECT_EQ(0, createPipe(&pipe_, 0));
        pipe = &pipe_;

        EXPECT_EQ(4, writeFd(pipe->mWrFile->mFd, "1234", 4, 0));
        closePipeWriter(pipe);

        EXPECT_EQ(4, readFdFully(pipe->mRdFile->mFd, &buf, 0));
        EXPECT_EQ(0, strncmp("1234", buf, 4));

        free(buf);

        pipe = closePipe(pipe);
    }

    {
        char  buf_;
        char *buf = &buf_;

        struct Pipe  pipe_;
        struct Pipe *pipe = 0;

        EXPECT_EQ(0, createPipe(&pipe_, 0));
        pipe = &pipe_;

        EXPECT_EQ(5, writeFd(pipe->mWrFile->mFd, "12345", 5, 0));
        closePipeWriter(pipe);

        EXPECT_EQ(5, readFdFully(pipe->mRdFile->mFd, &buf, 0));
        EXPECT_EQ(0, strncmp("12345", buf, 5));

        free(buf);

        pipe = closePipe(pipe);
    }
}

TEST(FdTest, CloseExceptWhiteList)
{
    int pipefd[4];

    EXPECT_EQ(0, pipe(pipefd + 0));
    EXPECT_EQ(0, pipe(pipefd + 2));

    struct FdSet  fdset_;
    struct FdSet *fdset = 0;

    struct rlimit fdLimit;
    EXPECT_EQ(0, getrlimit(RLIMIT_NOFILE, &fdLimit));

    EXPECT_EQ(0, createFdSet(&fdset_));
    fdset = &fdset_;

    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(STDERR_FILENO,STDERR_FILENO)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(pipefd[1], pipefd[1])));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(pipefd[2], pipefd[2])));

    /* Half the time, include a range that exceeds the number of
     * available file descriptors. */

    if ((getpid() / 2) & 1)
        EXPECT_EQ(0, insertFdSetRange(fdset,
                                      FdRange(fdLimit.rlim_cur, INT_MAX)));

    pid_t childpid = fork();

    EXPECT_NE(-1, childpid);

    if ( ! childpid)
    {
        int rc = -1;

        do
        {
            if (closeFdExceptWhiteList(fdset))
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            if (ownFdValid(pipefd[0]))
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            if ( ! ownFdValid(pipefd[1]))
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            if ( ! ownFdValid(pipefd[2]))
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            if (ownFdValid(pipefd[3]))
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            unsigned numFds = 0;
            for (unsigned fd = 0; fd < fdLimit.rlim_cur; ++fd)
            {
                if (ownFdValid(fd))
                    ++numFds;
            }

            if (3 != numFds)
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            rc = 0;

        } while (0);

        if (rc)
        {
            execl("/bin/false", "false", (char *) 0);
            _exit(EXIT_FAILURE);
        }

        execl("/bin/true", "true", (char *) 0);
        _exit(EXIT_FAILURE);
    }

    int status;
    EXPECT_EQ(childpid, waitpid(childpid, &status, 0));
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(EXIT_SUCCESS, WEXITSTATUS(status));

    fdset = closeFdSet(fdset);

    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(FdTest, CloseOnlyBlackList)
{
    int pipefd[2];

    EXPECT_EQ(0, pipe(pipefd));

    struct FdSet  fdset_;
    struct FdSet *fdset = 0;

    struct rlimit fdLimit;
    EXPECT_EQ(0, getrlimit(RLIMIT_NOFILE, &fdLimit));

    EXPECT_EQ(0, createFdSet(&fdset_));
    fdset = &fdset_;

    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(STDIN_FILENO,STDIN_FILENO)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(STDOUT_FILENO,STDOUT_FILENO)));
    EXPECT_EQ(0, insertFdSetRange(fdset, FdRange(pipefd[0], pipefd[0])));

    /* Half the time, include a range that exceeds the number of
     * available file descriptors. */

    if ((getpid() / 2) & 1)
        EXPECT_EQ(0, insertFdSetRange(fdset,
                                      FdRange(fdLimit.rlim_cur, INT_MAX)));

    pid_t childpid = fork();

    EXPECT_NE(-1, childpid);

    if ( ! childpid)
    {
        int rc = -1;

        do
        {
            unsigned openFds = 0;
            for (unsigned fd = 0; fd < fdLimit.rlim_cur; ++fd)
            {
                if (ownFdValid(fd))
                    ++openFds;
            }

            if (closeFdOnlyBlackList(fdset))
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            if ( ! ownFdValid(pipefd[1]))
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            if (ownFdValid(pipefd[0]))
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            unsigned numFds = 0;
            for (unsigned fd = 0; fd < fdLimit.rlim_cur; ++fd)
            {
                if (ownFdValid(fd))
                    ++numFds;
            }

            if (numFds + 3 != openFds)
            {
                fprintf(stderr, "%u %u %u\n", __LINE__, numFds, openFds);
                break;
            }

            rc = 0;

        } while (0);

        if (rc)
        {
            execl("/bin/false", "false", (char *) 0);
            _exit(EXIT_FAILURE);
        }

        execl("/bin/true", "true", (char *) 0);
        _exit(EXIT_FAILURE);
    }

    int status;
    EXPECT_EQ(childpid, waitpid(childpid, &status, 0));
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(EXIT_SUCCESS, WEXITSTATUS(status));

    fdset = closeFdSet(fdset);

    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(FdTest, OpenStdFds)
{
    pid_t childpid = fork();

    EXPECT_NE(-1, childpid);

    if ( ! childpid)
    {
        int rc = -1;

        do
        {
            char buf[1];

            int errfd = dup(STDERR_FILENO);
            if (-1 == errfd)
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            FILE *errfp = fdopen(errfd, "w");
            if ( ! errfp)
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            if (SIG_ERR == signal(SIGPIPE, SIG_IGN))
            {
                fprintf(stderr, "%u\n", __LINE__);
                break;
            }

            close(STDIN_FILENO);
            close(STDOUT_FILENO);
            close(STDERR_FILENO);

            if (openStdFds())
            {
                fprintf(errfp, "%u\n", __LINE__);
                break;
            }

            if ( ! ownFdValid(STDIN_FILENO) || read(STDIN_FILENO, buf, 1))
            {
                fprintf(errfp, "%u\n", __LINE__);
                break;
            }

            if ( ! ownFdValid(STDOUT_FILENO) ||
                 -1 != write(STDOUT_FILENO, buf, 1) || EPIPE != errno)
            {
                fprintf(errfp, "%u\n", __LINE__);
                break;
            }

            if ( ! ownFdValid(STDERR_FILENO) ||
                 -1 != write(STDERR_FILENO, buf, 1) || EPIPE != errno)
            {
                fprintf(errfp, "%u\n", __LINE__);
                break;
            }

            close(STDIN_FILENO);

            if (openStdFds())
            {
                fprintf(errfp, "%u\n", __LINE__);
                break;
            }

            if ( ! ownFdValid(STDIN_FILENO) || read(STDIN_FILENO, buf, 1))
            {
                fprintf(errfp, "%u\n", __LINE__);
                break;
            }

            close(STDOUT_FILENO);

            if (openStdFds())
            {
                fprintf(errfp, "%u\n", __LINE__);
                break;
            }

            if ( ! ownFdValid(STDOUT_FILENO) ||
                 -1 != write(STDOUT_FILENO, buf, 1) || EPIPE != errno)
            {
                fprintf(errfp, "%u\n", __LINE__);
                break;
            }

            close(STDERR_FILENO);

            if (openStdFds())
            {
                fprintf(errfp, "%u\n", __LINE__);
                break;
            }

            if ( ! ownFdValid(STDERR_FILENO) ||
                 -1 != write(STDERR_FILENO, buf, 1) || EPIPE != errno)
            {
                fprintf(errfp, "%u\n", __LINE__);
                break;
            }

            rc = 0;

        } while (0);

        if (rc)
        {
            execl("/bin/false", "false", (char *) 0);
            _exit(EXIT_FAILURE);
        }

        execl("/bin/true", "true", (char *) 0);
        _exit(EXIT_FAILURE);
    }

    int status;
    EXPECT_EQ(childpid, waitpid(childpid, &status, 0));
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(EXIT_SUCCESS, WEXITSTATUS(status));
}

#include "../googletest/src/gtest_main.cc"
