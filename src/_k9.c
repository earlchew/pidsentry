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

#include "options.h"
#include "macros.h"
#include "pipe.h"
#include "stdfdfiller.h"
#include "pidfile.h"
#include "process.h"
#include "error.h"
#include "test.h"
#include "fd.h"

#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/ioctl.h>

/* TODO
 *
 * cmdRunCommand() is too big, break it up
 * EINTR everywhere, especially flock
 * SIGALARM for occasional EINTR
 * Correctly close socket/pipe on read and write EOF
 * Check correct operation if child closes tether first, vs stdout close first
 * Bracket splice() with SIGALARM (setitimer() and getitimer())
 * Partition monitorChild() -- it's too big
 * Add test for flock timeout (Use -T to shorten timeout value)
 * Add unit test for timekeeping
 */

#define DEVNULLPATH "/dev/null"

static const char sDevNullPath[] = DEVNULLPATH;

/* -------------------------------------------------------------------------- */
static pid_t
runChild(
    char              **aCmd,
    struct StdFdFiller *aStdFdFiller,
    struct Pipe        *aTetherPipe,
    struct Pipe        *aSyncPipe,
    struct Pipe        *aTermPipe,
    struct Pipe        *aSigPipe)
{
    pid_t rc = -1;

    /* Both the parent and child share the same signal handler configuration.
     * In particular, no custom signal handlers are configured, so
     * signals delivered to either will likely caused them to terminate.
     *
     * This is safe because that would cause one of end the termPipe
     * to close, and the other end will eventually notice. */

    pid_t childPid = forkProcess(
        gOptions.mSetPgid
        ? ForkProcessSetProcessGroup
        : ForkProcessShareProcessGroup);

    if (-1 == childPid)
        goto Finally;

    if (childPid)
    {
        debug(0, "running child process %jd", (intmax_t) childPid);
    }
    else
    {
        debug(0, "starting child process");

        /* Unwatch the signals so that the child process will be
         * responsive to signals from the parent. Note that the parent
         * will wait for the child to synchronise before sending it
         * signals, so that there is no race here. */

        if (unwatchProcessSignals())
            terminate(
                errno,
                "Unable to remove watch from signals");

        /* Close the StdFdFiller in case this will free up stdin, stdout or
         * stderr. The remaining operations will close the remaining
         * unwanted file descriptors. */

        if (closeStdFdFiller(aStdFdFiller))
            terminate(
                errno,
                "Unable to close stdin, stdout and stderr fillers");

        if (closePipe(aTermPipe))
            terminate(
                errno,
                "Unable to close termination pipe");

        if (closePipe(aSigPipe))
            terminate(
                errno,
                "Unable to close signal pipe");

        /* Wait until the parent has created the pidfile. This
         * invariant can be used to determine if the pidfile
         * is really associated with the process possessing
         * the specified pid. */

        debug(0, "synchronising child process");

        RACE
        ({
            while (true)
            {
                char buf[1];

                switch (read(aSyncPipe->mRdFile->mFd, buf, 1))
                {
                default:
                        break;
                case -1:
                    if (EINTR == errno)
                        continue;
                    terminate(
                        errno,
                        "Unable to synchronise child");
                    break;

                case 0:
                    _exit(1);
                    break;
                }

                break;
            }
        });

        if (closePipe(aSyncPipe))
            terminate(
                errno,
                "Unable to close sync pipe");

        char tetherArg[sizeof(int) * CHAR_BIT + 1];

        do
        {
            /* Close the reading end of the tether pipe separately
             * because it might turn out that the writing end
             * will not need to be duplicated. */

            if (closePipeReader(aTetherPipe))
                terminate(
                    errno,
                    "Unable to close tether pipe reader");

            if (gOptions.mTether)
            {
                int tetherFd = *gOptions.mTether;

                if (0 > tetherFd)
                    tetherFd = aTetherPipe->mWrFile->mFd;

                sprintf(tetherArg, "%d", tetherFd);

                if (gOptions.mName)
                {
                    bool useEnv = isupper(gOptions.mName[0]);

                    for (unsigned ix = 1; useEnv && gOptions.mName[ix]; ++ix)
                    {
                        unsigned char ch = gOptions.mName[ix];

                        if ( ! isupper(ch) && ! isdigit(ch) && ch != '_')
                            useEnv = false;
                    }

                    if (useEnv)
                    {
                        if (setenv(gOptions.mName, tetherArg, 1))
                            terminate(
                                errno,
                                "Unable to set environment variable '%s'",
                                gOptions.mName);
                    }
                    else
                    {
                        /* Start scanning from the first argument, leaving
                         * the command name intact. */

                        char *matchArg = 0;

                        for (unsigned ix = 1; aCmd[ix]; ++ix)
                        {
                            matchArg = strstr(aCmd[ix], gOptions.mName);

                            if (matchArg)
                            {
                                char replacedArg[
                                    strlen(aCmd[ix])       -
                                    strlen(gOptions.mName) +
                                    strlen(tetherArg)      + 1];

                                sprintf(replacedArg,
                                        "%.*s%s%s",
                                        matchArg - aCmd[ix],
                                        aCmd[ix],
                                        tetherArg,
                                        matchArg + strlen(gOptions.mName));

                                aCmd[ix] = strdup(replacedArg);

                                if ( ! aCmd[ix])
                                    terminate(
                                        errno,
                                        "Unable to duplicate '%s'",
                                        replacedArg);
                                break;
                            }
                        }

                        if ( ! matchArg)
                            terminate(
                                0,
                                "Unable to find matching argument '%s'",
                                gOptions.mName);
                    }
                }

                if (tetherFd == aTetherPipe->mWrFile->mFd)
                    break;

                if (dup2(aTetherPipe->mWrFile->mFd, tetherFd) != tetherFd)
                    terminate(
                        errno,
                        "Unable to dup tether pipe fd %d to fd %d",
                        aTetherPipe->mWrFile->mFd,
                        tetherFd);
            }

            if (closePipe(aTetherPipe))
                terminate(
                    errno,
                    "Unable to close tether pipe");

        } while (0);

        debug(0, "child process synchronised");

        /* The child process does not close the process lock because it
         * might need to emit a diagnostic if execvp() fails. Rely on
         * O_CLOEXEC to close the underlying file descriptors. */

        execvp(aCmd[0], aCmd);
        terminate(
            errno,
            "Unable to execute '%s'", aCmd[0]);
    }

    rc = childPid;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static int
reapChild(pid_t aChildPid)
{
    int status;
    pid_t pid;

    ensure(-1 != aChildPid && aChildPid);

    do
    {
        pid = waitpid(aChildPid, &status, 0);

        if (-1 == pid && EINTR != errno)
            terminate(
                errno,
                "Unable to reap child pid '%jd'",
                (intmax_t) aChildPid);

    } while (pid != aChildPid);

    return status;
}

/* -------------------------------------------------------------------------- */
struct PollEventText
{
    char mText[
        sizeof(unsigned) * CHAR_BIT +
        sizeof(
            " "
            "0x "
            "IN "
            "PRI "
            "OUT "
            "ERR "
            "HUP "
            "NVAL ")];
};

static char *
pollEventTextBit_(char *aBuf, unsigned *aMask, unsigned aBit, const char *aText)
{
    char *buf = aBuf;

    if (*aMask & aBit)
    {
        *aMask ^= aBit;
        *buf++ = ' ';
        buf = stpcpy(buf, aText + sizeof("POLL") - 1);
    }

    return buf;
}

#define pollEventTextBit_(aBuf, aMask, aBit) \
    pollEventTextBit_((aBuf), (aMask), (aBit), # aBit)

static const char *
createPollEventText(
    struct PollEventText *aPollEventText, unsigned aPollEventMask)
{
    unsigned mask = aPollEventMask;

    char *buf = aPollEventText->mText;

    buf[0] = 0;
    buf[1] = 0;

    buf = pollEventTextBit_(buf, &mask, POLLIN);
    buf = pollEventTextBit_(buf, &mask, POLLPRI);
    buf = pollEventTextBit_(buf, &mask, POLLOUT);
    buf = pollEventTextBit_(buf, &mask, POLLERR);
    buf = pollEventTextBit_(buf, &mask, POLLHUP);
    buf = pollEventTextBit_(buf, &mask, POLLNVAL);

    if (mask)
        sprintf(buf, " 0x%x", mask);

    return aPollEventText->mText + 1;
}

/* -------------------------------------------------------------------------- */
static void
monitorChild(pid_t aChildPid, struct Pipe *aTermPipe, struct Pipe *aSigPipe)
{
    debug(0, "start monitoring child");

    struct PollEventText pollEventText;
    struct PollEventText pollRcvdEventText;

    enum PollFdKind
    {
        POLL_FD_STDIN,
        POLL_FD_STDOUT,
        POLL_FD_CHILD,
        POLL_FD_SIGNAL,
        POLL_FD_CLOCK,
        POLL_FD_COUNT
    };

    struct Pipe nullPipe;
    if (createPipe(&nullPipe))
        terminate(
            errno,
            "Unable to create null pipe");

    struct Pipe clockPipe;
    if (createPipe(&clockPipe))
        terminate(
            errno,
            "Unable to create clock pipe");
    if (closePipeOnExec(&clockPipe, O_CLOEXEC))
        terminate(
            errno,
            "Unable to set close on exec for clock pipe");

    struct timeval clockPeriod = { .tv_sec = 3 };

    if (watchProcessClock(&clockPipe, &clockPeriod))
        terminate(
            errno,
            "Unable to install process clock watch");

    /* Experiments at http://www.greenend.org.uk/rjk/tech/poll.html show
     * that it is best not to put too much trust in POLLHUP vs POLLIN,
     * and to treat the presence of either as a trigger to attempt to
     * read from the file descriptor.
     *
     * For the writing end of the pipe, Linux returns POLLERR if the
     * far end reader is no longer available (to match EPIPE), but
     * the documentation suggests that POLLHUP might also be reasonable
     * in this context. */

    const unsigned pollInputEvents     = POLLHUP | POLLERR | POLLPRI | POLLIN;
    const unsigned pollOutputEvents    = POLLHUP | POLLERR | POLLOUT;
    const unsigned pollDisconnectEvent = POLLHUP | POLLERR;

    static const char *pollfdNames[POLL_FD_COUNT] =
    {
        [POLL_FD_CHILD]  = "child",
        [POLL_FD_SIGNAL] = "signal",
        [POLL_FD_STDOUT] = "stdout",
        [POLL_FD_STDIN]  = "stdin",
        [POLL_FD_CLOCK]  = "clock",
    };

    struct pollfd pollfds[POLL_FD_COUNT] =
    {
        [POLL_FD_CLOCK] = {
            .fd = clockPipe.mRdFile->mFd,  .events = pollInputEvents },
        [POLL_FD_CHILD] = {
            .fd = aTermPipe->mRdFile->mFd, .events = pollInputEvents },
        [POLL_FD_SIGNAL] = {
            .fd = aSigPipe->mRdFile->mFd,  .events = pollInputEvents },
        [POLL_FD_STDOUT] = {
            .fd = STDOUT_FILENO,  .events = pollDisconnectEvent },
        [POLL_FD_STDIN] = {
            .fd = STDIN_FILENO,   .events = ! gOptions.mTether
                                            ? 0 : pollInputEvents },
    };

    bool deadChild = false;

    struct ChildSignalPlan
    {
        pid_t mPid;
        int   mSig;
    };

    struct ChildSignalPlan sharedPgrpPlan[] =
    {
        { aChildPid, SIGTERM },
        { aChildPid, SIGKILL },
        { 0 }
    };

    struct ChildSignalPlan ownPgrpPlan[] =
    {
        {  aChildPid, SIGTERM },
        { -aChildPid, SIGTERM },
        { -aChildPid, SIGKILL },
        { 0 }
    };

    const struct ChildSignalPlan *childSignalPlan =
        gOptions.mSetPgid ? ownPgrpPlan : sharedPgrpPlan;

    int timeout = gOptions.mTimeout;

    timeout = timeout ? timeout * 1000 : -1;

    int closedStdout = 0;
    int closedStdin  = 0;

    /* It would be so much easier to use non-blocking IO, but O_NONBLOCK
     * is an attribute of the underlying open file, not of each
     * file descriptor. Since stdin and stdout are typically inherited
     * from the parent, setting O_NONBLOCK affects all file descriptors
     * referring to the same open file. */

    do
    {
        ensure(closedStdin == closedStdout);

        debug(1, "poll wait");

        int rc = poll(pollfds, NUMBEROF(pollfds), timeout);

        if (-1 == rc)
        {
            if (EINTR == errno)
                continue;

            terminate(
                errno,
                "Unable to poll for activity");
        }

        /* The poll(2) call will mark POLLNVAL, POLLERR or POLLHUP
         * no matter what the caller has subscribed for. Only pay
         * attention to what was subscribed. */

        debug(1, "poll scan of %d fds", rc);

        for (unsigned ix = 0; NUMBEROF(pollfds) > ix; ++ix)
        {
            debug(
                1,
                "poll %s %d (%s) (%s)",
                pollfdNames[ix],
                pollfds[ix].fd,
                createPollEventText(&pollEventText, pollfds[ix].events),
                createPollEventText(&pollRcvdEventText, pollfds[ix].revents));
            pollfds[ix].revents &= pollfds[ix].events;
        }

        /* When processing file descriptor events, do not loop in EINTR
         * but instead allow the polling cycle to be re-run so that
         * the event loop will not remain stuck processing a single
         * file descriptor. */

        unsigned eventCount = 0;

        if (pollfds[POLL_FD_CLOCK].revents)
        {
            ensure(rc);

            ++eventCount;

            /* The clock is used to deliver SIGALRM to the process
             * periodically to ensure that blocking operations will
             * return with EINTR so that the event loop remains
             * responsive. */

            debug(
                1,
                "clock tick %s",
                createPollEventText(
                    &pollEventText,
                    pollfds[POLL_FD_CLOCK].revents));

            unsigned char clockTick;

            ssize_t len = read(clockPipe.mRdFile->mFd, &clockTick, 1);

            if (-1 == len)
            {
                if (EINTR != errno)
                    terminate(
                        errno,
                        "Unable to read clock tick from queue");
            }
            else if ( ! len)
            {
                    terminate(
                        0,
                        "Clock tick queue closed unexpectedly");
            }
        }

        if (pollfds[POLL_FD_STDIN].revents)
        {
            ensure(rc);
            ensure(STDIN_FILENO == pollfds[POLL_FD_STDIN].fd);
            ensure( ! closedStdin);

            ++eventCount;

            debug(
                1,
                "poll stdin %s",
                createPollEventText(
                    &pollEventText,
                    pollfds[POLL_FD_STDIN].revents));

            ensure(pollfds[POLL_FD_STDIN].events);

            if ( ! (pollfds[POLL_FD_STDIN].revents & POLLIN))
                closedStdin = -1;
            else
            {
                pollfds[POLL_FD_STDIN].fd = nullPipe.mRdFile->mFd;

                pollfds[POLL_FD_STDOUT].events = pollOutputEvents;
                pollfds[POLL_FD_STDIN].events  = pollDisconnectEvent;
            }
        }

        /* If a timeout is expected and a timeout occurred, and the
         * event loop was waiting for data from the child process,
         * then declare the child terminated. */

        if ( ! rc)
        {
            ensure(-1 != timeout);

            ++eventCount;

            debug(0, "timeout after %d", timeout);

            pid_t pidNum = childSignalPlan->mPid;
            int   sigNum = childSignalPlan->mSig;

            if (childSignalPlan[1].mPid)
                ++childSignalPlan;

            warn(
                0,
                "Killing unresponsive child pid %jd with signal %d",
                (intmax_t) pidNum,
                sigNum);

            if (kill(pidNum, sigNum) && ESRCH != errno)
                terminate(
                    errno,
                    "Unable to kill child pid %jd with signal %d",
                    (intmax_t) pidNum,
                    sigNum);
        }

        if (pollfds[POLL_FD_STDOUT].revents)
        {
            ensure(rc);
            ensure(STDOUT_FILENO == pollfds[POLL_FD_STDOUT].fd);
            ensure( ! closedStdout);

            ++eventCount;

            debug(
                1,
                "poll stdout %s",
                createPollEventText(
                    &pollEventText,
                    pollfds[POLL_FD_STDOUT].revents));

            ensure(pollfds[POLL_FD_STDOUT].events);

            do
            {
                if (pollfds[POLL_FD_STDOUT].revents & POLLOUT)
                {
                    int available;

                    /* Use FIONREAD to dynamically determine the amount
                     * of data in stdin, remembering that the child
                     * process could change the capacity of the pipe
                     * at runtime. */

                    if (ioctl(STDIN_FILENO, FIONREAD, &available))
                        terminate(
                            errno,
                            "Unable to find amount of readable data in stdin");

                    ensure(available);

                    if (testAction() && available)
                        available = 1 + random() % available;

                    ssize_t bytes = spliceFd(
                        STDIN_FILENO,
                        STDOUT_FILENO,
                        available,
                        SPLICE_F_MOVE | SPLICE_F_MORE | SPLICE_F_NONBLOCK);

                    debug(1,
                          "spliced stdin to stdout %zd out of %d",
                          bytes,
                          available);

                    /* If the child has closed its end of the tether, the
                     * watchdog will read EOF on the tether. Continue running
                     * the event loop until the child terminates. */

                    if (-1 == bytes)
                    {
                        if (EPIPE != errno)
                        {
                            switch (errno)
                            {
                            default:
                                terminate(
                                    errno,
                                    "Unable to write to stdout");

                            case EWOULDBLOCK:
                            case EINTR:
                                break;
                            }

                            break;
                        }
                    }
                    else if (bytes)
                    {
                        /* Continue polling stdout unless all the available
                         * data on stdin was transferred because this might
                         * be the last chunk of data on stdin before it was
                         * closed so there will be no more available. */

                        if (bytes >= available)
                        {
                            pollfds[POLL_FD_STDIN].fd = STDIN_FILENO;

                            pollfds[POLL_FD_STDOUT].events= pollDisconnectEvent;
                            pollfds[POLL_FD_STDIN].events = pollInputEvents;
                        }
                        break;
                    }
                }

                closedStdout = -1;
                break;

            } while (0);
        }

        if (0 > closedStdout || 0 > closedStdin)
        {
            closedStdout = 1;
            closedStdin  = 1;

            debug(0, "closing stdin and stdout");

            /* If the far end of stdout has been closed, close stdin
             * using the side-effect of dup2. Use of dup2 ensures
             * that the watchdog continues to have a valid stdin.
             *
             * Also duplicating the file descriptors allows nullPipe
             * to be cleaned up while leaving a valid stdin and stdout. */

            if (STDIN_FILENO != dup2(nullPipe.mRdFile->mFd, STDIN_FILENO))
                terminate(
                    errno,
                    "Unable to dup null pipe to stdin");

            if (STDOUT_FILENO != dup2(nullPipe.mWrFile->mFd, STDOUT_FILENO))
                terminate(
                    errno,
                    "Unable to dup null pipe to stdout");

            pollfds[POLL_FD_STDIN].fd  = STDIN_FILENO;
            pollfds[POLL_FD_STDOUT].fd = STDOUT_FILENO;

            pollfds[POLL_FD_STDOUT].events = pollDisconnectEvent;
            pollfds[POLL_FD_STDIN].events  = pollDisconnectEvent;
        }

        /* Propagate signals to the child process. Signals are queued
         * by the local signal handler to the inherent race in the
         * fork() idiom:
         *
         *     pid_t childPid = fork();
         *
         * The fork() completes before childPid can be assigned. This
         * event loop only runs after the fork() is complete and
         * any signals received before the fork() will be queued for
         * delivery. */

        if (pollfds[POLL_FD_SIGNAL].revents)
        {
            ensure(rc);

            ++eventCount;

            debug(
                1,
                "poll signal %s",
                createPollEventText(
                    &pollEventText,
                    pollfds[POLL_FD_SIGNAL].revents));

            unsigned char sigNum;

            ssize_t len = read(aSigPipe->mRdFile->mFd, &sigNum, 1);

            if (-1 == len)
            {
                if (EINTR != errno)
                    terminate(
                        errno,
                        "Unable to read signal from queue");
            }
            else if ( ! len)
            {
                    terminate(
                        0,
                        "Signal queue closed unexpectedly");
            }
            else
            {
                debug(1,
                      "deliver signal %d to child pid %jd",
                      sigNum,
                      (intmax_t) aChildPid);

                if (kill(aChildPid, sigNum))
                {
                    if (ESRCH != errno)
                        warn(
                            errno,
                            "Unable to deliver signal %d to child pid %jd",
                            sigNum,
                            (intmax_t) aChildPid);
                }
            }
        }

        /* Record when the child has terminated, but do not exit
         * the event loop until all the IO has been flushed. */

        if (pollfds[POLL_FD_CHILD].revents)
        {
            ensure(rc);

            ++eventCount;

            debug(
                1,
                "poll child %s",
                createPollEventText(
                    &pollEventText,
                    pollfds[POLL_FD_CHILD].revents));

            ensure(pollfds[POLL_FD_CHILD].events);

            pollfds[POLL_FD_CHILD].events = 0;
            deadChild = true;
        }

        /* Ensure that the interpretation of the poll events is being
         * correctly handled, to avoid a busy-wait poll loop. */

        ensure(eventCount);

    } while ( ! deadChild ||
                pollOutputEvents == pollfds[POLL_FD_STDOUT].events ||
                pollInputEvents  == pollfds[POLL_FD_STDIN].events);

    if (unwatchProcessClock())
        terminate(
            errno,
            "Unable to remove process clock watch");

    if (closePipe(&clockPipe))
        terminate(
            errno,
            "Unable to close clock pipe");

    if (closePipe(&nullPipe))
        terminate(
            errno,
            "Unable to close null pipe");

    debug(0, "stop monitoring child");
}

/* -------------------------------------------------------------------------- */
static void
announceChild(pid_t aPid, struct PidFile *aPidFile, const char *aPidFileName)
{
    for (int zombie = -1; zombie; )
    {
        if (0 < zombie)
        {
            debug(0, "discarding zombie pid file '%s'", aPidFileName);

            if (closePidFile(aPidFile))
                terminate(
                    errno,
                    "Cannot close pid file '%s'", aPidFileName);
        }

        if (createPidFile(aPidFile, aPidFileName))
            terminate(
                errno,
                "Cannot create pid file '%s'", aPidFileName);

        /* It is not possible to create the pidfile and acquire a flock
         * as an atomic operation. The flock can only be acquired after
         * the pidfile exists. Since this newly created pidfile is empty,
         * it resembles an closed pidfile, and in the intervening time,
         * another process might have removed it and replaced it with
         * another. */

        if (acquireWriteLockPidFile(aPidFile))
            terminate(
                errno,
                "Cannot acquire write lock on pid file '%s'", aPidFileName);

        zombie = detectPidFileZombie(aPidFile);

        if (0 > zombie)
            terminate(
                errno,
                "Unable to obtain status of pid file '%s'", aPidFileName);
    }

    debug(0, "created pid file '%s'", aPidFileName);

    /* Ensure that the mtime of the pidfile is later than the
     * start time of the child process, if that process exists. */

    struct timespec childStartTime = findProcessStartTime(aPid);

    if (UTIME_OMIT == childStartTime.tv_nsec)
    {
        terminate(
            errno,
            "Unable to obtain status of pid %jd", (intmax_t) aPid);
    }
    else if (UTIME_NOW != childStartTime.tv_nsec)
    {
        debug(0,
              "child process mtime %jd.%09ld",
              (intmax_t) childStartTime.tv_sec, childStartTime.tv_nsec);

        struct stat pidFileStat;

        while (true)
        {
            if (fstat(aPidFile->mFile->mFd, &pidFileStat))
                terminate(
                    errno,
                    "Cannot obtain status of pid file '%s'", aPidFileName);

            struct timespec pidFileTime = pidFileStat.st_mtim;

            debug(0,
                  "pid file mtime %jd.%09ld",
                  (intmax_t) pidFileTime.tv_sec, pidFileTime.tv_nsec);

            if (pidFileTime.tv_sec > childStartTime.tv_sec)
                break;

            if (pidFileTime.tv_sec  == childStartTime.tv_sec &&
                pidFileTime.tv_nsec >  childStartTime.tv_nsec)
                break;

            if ( ! pidFileTime.tv_nsec)
                pidFileTime.tv_nsec = 900 * 1000 * 1000;

            for (long usResolution = 1; ; usResolution *= 10)
            {
                if (pidFileTime.tv_nsec % (1000 * usResolution))
                {
                    /* OpenGroup says that the argument to usleep(3)
                     * must be less than 1e6. */

                    ensure(usResolution);
                    ensure(1000 * 1000 >= usResolution);

                    --usResolution;

                    debug(0, "delay for %ldus", usResolution);

                    if (usleep(usResolution) && EINTR != errno)
                        terminate(
                            errno,
                            "Unable to sleep for %ldus", usResolution);

                    break;
                }
            }

            /* Mutate the data in the pidfile so that the mtime
             * and ctime will be updated. */

            if (1 != write(aPidFile->mFile->mFd, "\n", 1))
                terminate(
                    errno,
                    "Unable to write to pid file '%s'", aPidFileName);

            if (ftruncate(aPidFile->mFile->mFd, 0))
                terminate(
                    errno,
                    "Unable to truncate pid file '%s'", aPidFileName);
        }
    }

    if (writePidFile(aPidFile, aPid))
        terminate(
            errno,
            "Cannot write to pid file '%s'", aPidFileName);

    /* The pidfile was locked on creation, and now that it
     * is completely initialised, it is ok to release
     * the flock. */

    if (releaseLockPidFile(aPidFile))
        terminate(
            errno,
            "Cannot unlock pid file '%s'", aPidFileName);
}

/* -------------------------------------------------------------------------- */
static struct ExitCode
cmdPrintPidFile(const char *aFileName)
{
    struct ExitCode exitCode = { 1 };

    struct PidFile pidFile;

    if (openPidFile(&pidFile, aFileName))
    {
        if (ENOENT != errno)
            terminate(
                errno,
                "Unable to open pid file '%s'", aFileName);
        return exitCode;
    }

    if (acquireReadLockPidFile(&pidFile))
        terminate(
            errno,
            "Unable to acquire read lock on pid file '%s'", aFileName);

    pid_t pid = readPidFile(&pidFile);

    switch (pid)
    {
    default:
        if (-1 != dprintf(STDOUT_FILENO, "%jd\n", (intmax_t) pid))
            exitCode.mStatus = 0;
        break;
    case 0:
        break;
    case -1:
        terminate(
            errno,
            "Unable to read pid file '%s'", aFileName);
    }

    FINALLY
    ({
        if (closePidFile(&pidFile))
            terminate(
                errno,
                "Unable to close pid file '%s'", aFileName);
    });

    return exitCode;
}

/* -------------------------------------------------------------------------- */
static struct ExitCode
cmdRunCommand(char **aCmd)
{
    ensure(aCmd);

    /* The instance of the StdFdFiller guarantees that any further file
     * descriptors that are opened will not be mistaken for stdin,
     * stdout or stderr. */

    struct StdFdFiller stdFdFiller;

    if (createStdFdFiller(&stdFdFiller))
        terminate(
            errno,
            "Unable to create stdin, stdout, stderr filler");

    struct Pipe tetherPipe;
    if (createPipe(&tetherPipe))
        terminate(
            errno,
            "Unable to create tether pipe");

    struct Pipe termPipe;
    if (createPipe(&termPipe))
        terminate(
            errno,
            "Unable to create termination pipe");
    if (closePipeOnExec(&termPipe, O_CLOEXEC))
        terminate(
            errno,
            "Unable to set close on exec for termination pipe");

    struct Pipe sigPipe;
    if (createPipe(&sigPipe))
        terminate(
            errno,
            "Unable to create signal pipe");
    if (closePipeOnExec(&sigPipe, O_CLOEXEC))
        terminate(
            errno,
            "Unable to set close on exec for signal pipe");

    if (watchProcessSignals(&sigPipe))
        terminate(
            errno,
            "Unable to add watch on signals");

    if (watchProcessChildren(&termPipe))
        terminate(
            errno,
            "Unable to add watch on child process termination");

    if (ignoreProcessSigPipe())
        terminate(
            errno,
            "Unable to ignore pipe signal");

    /* Only identify the watchdog process after all the signal
     * handlers have been installed. The functional tests can
     * use this as an indicator that the watchdog is ready to
     * run the child process. */

    if (gOptions.mIdentify)
        RACE
        ({
            if (-1 == dprintf(STDOUT_FILENO, "%jd\n", (intmax_t) getpid()))
                terminate(
                    errno,
                    "Unable to print parent pid");
        });

    struct Pipe syncPipe;
    if (createPipe(&syncPipe))
        terminate(
            errno,
            "Unable to create sync pipe");

    pid_t childPid = runChild(aCmd,
                              &stdFdFiller,
                              &tetherPipe, &syncPipe, &termPipe, &sigPipe);
    if (-1 == childPid)
        terminate(
            errno,
            "Unable to fork child");

    struct PidFile  pidFile_;
    struct PidFile *pidFile = 0;

    if (gOptions.mPidFile)
    {
        const char *pidFileName = gOptions.mPidFile;

        pid_t pid = gOptions.mPid;

        switch (pid)
        {
        default:
            break;
        case -1:
            pid = getpid(); break;
        case 0:
            pid = childPid; break;
        }

        pidFile = &pidFile_;

        announceChild(pid, pidFile, pidFileName);
    }

    /* The creation time of the child process is earlier than
     * the creation time of the pidfile. With the pidfile created,
     * release the waiting child process. */

    if (gOptions.mIdentify)
        RACE
        ({
            if (-1 == dprintf(STDOUT_FILENO, "%jd\n", (intmax_t) childPid))
                terminate(
                    errno,
                    "Unable to print child pid");
        });

    RACE
    ({
        if (1 != write(syncPipe.mWrFile->mFd, "", 1))
            terminate(
                errno,
                "Unable to synchronise child process");
    });

    if (closePipe(&syncPipe))
        terminate(
            errno,
            "Unable to close sync pipe");

    /* With the child process launched, close the instance of StdFdFiller
     * so that stdin, stdout and stderr become available for manipulation
     * and will not be closed multiple times. */

    if (closeStdFdFiller(&stdFdFiller))
        terminate(
            errno,
            "Unable to close stdin, stdout and stderr fillers");

    if (STDIN_FILENO != dup2(tetherPipe.mRdFile->mFd, STDIN_FILENO))
        terminate(
            errno,
            "Unable to dup tether pipe to stdin");

    if (closePipe(&tetherPipe))
        warn(
            errno,
            "Unable to close tether pipe");

    if (purgeProcessOrphanedFds())
        terminate(
            errno,
            "Unable to cleanse files");

    /* If stdout is not open, or data sent to stdout is to be discarded,
     * then provide a default sink for the data transmitted through
     * the tether. */

    bool discardStdout = gOptions.mQuiet;

    if (gOptions.mTether)
    {
        switch (ownFdValid(STDOUT_FILENO))
        {
        default:
            break;

        case -1:
            terminate(
                errno,
                "Unable to check validity of stdout");

        case 0:
            discardStdout = true;
            break;
        }
    }

    if (discardStdout)
    {
        int nullfd = open(sDevNullPath, O_WRONLY);

        if (-1 == nullfd)
            terminate(
                errno,
                "Unable to open %s", sDevNullPath);

        if (STDOUT_FILENO != nullfd)
        {
            if (STDOUT_FILENO != dup2(nullfd, STDOUT_FILENO))
                terminate(
                    errno,
                    "Unable to dup %s to stdout", sDevNullPath);
            if (closeFd(&nullfd))
                terminate(
                    errno,
                    "Unable to close %s", sDevNullPath);
        }
    }

    /* Monitor the running child until it has either completed of
     * its own accord, or terminated. Once the child has stopped
     * running, release the pid file if one was allocated. */

    monitorChild(childPid, &termPipe, &sigPipe);

    if (resetProcessSigPipe())
        terminate(
            errno,
            "Unable to reset pipe signal");

    if (unwatchProcessSignals())
        terminate(
            errno,
            "Unable to remove watch from signals");

    if (unwatchProcessChildren())
        terminate(
            errno,
            "Unable to remove watch on child process termination");

    if (closePipe(&sigPipe))
        terminate(
            errno,
            "Unable to close signal pipe");

    if (closePipe(&termPipe))
        terminate(
            errno,
            "Unable to close termination pipe");

    if (pidFile)
    {
        if (acquireWriteLockPidFile(pidFile))
            terminate(
                errno,
                "Cannot lock pid file '%s'", pidFile->mPathName.mFileName);

        if (closePidFile(pidFile))
            terminate(
                errno,
                "Cannot close pid file '%s'", pidFile->mPathName.mFileName);

        pidFile = 0;
    }

    /* Reap the child only after the pid file is released. This ensures
     * that any competing reader that manages to sucessfully lock and
     * read the pid file will see that the process exists. */

    debug(0, "reaping child pid %jd", (intmax_t) childPid);

    int status = reapChild(childPid);

    debug(0, "reaped child pid %jd status %d", (intmax_t) childPid, status);

    return extractProcessExitStatus(status);
}

/* -------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    if (Process_init(argv[0]))
        terminate(
            errno,
            "Unable to initialise process state");

    struct ExitCode exitCode;

    {
        char **cmd = parseOptions(argc, argv);

        if ( ! cmd && gOptions.mPidFile)
            exitCode = cmdPrintPidFile(gOptions.mPidFile);
        else
            exitCode = cmdRunCommand(cmd);
    }

    if (Process_exit())
        terminate(
            errno,
            "Unable to finalise process state");

    return exitCode.mStatus;
}

/* -------------------------------------------------------------------------- */
