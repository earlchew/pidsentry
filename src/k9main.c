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

#include "k9main.h"

#include "options_.h"
#include "env_.h"
#include "macros_.h"
#include "pipe_.h"
#include "unixsocket_.h"
#include "timekeeping_.h"
#include "stdfdfiller_.h"
#include "pidfile_.h"
#include "process_.h"
#include "error_.h"
#include "test_.h"
#include "fd_.h"
#include "dl_.h"

#include "libk9.h"

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
#include <sys/un.h>

/* TODO
 *
 * cmdRunCommand() is too big, break it up
 * Add test case for SIGKILL of watchdog and child not watching tether
 * Check for useless #include in *.c
 * Provide option to use PR_SET_PDEATHSIG instead of umbilical
 * Correct monitoring child scheduling of competing deadlines
 * Fix logging in parasite printing null for program name
 * Fix vgcore being dropped everywhere
 */

#define DEVNULLPATH "/dev/null"

static const char sDevNullPath[] = DEVNULLPATH;

static const char *sK9soPath;

/* -------------------------------------------------------------------------- */
enum PollFdKind
{
    POLL_FD_STDIN,
    POLL_FD_STDOUT,
    POLL_FD_CHILD,
    POLL_FD_SIGNAL,
    POLL_FD_CLOCK,
    POLL_FD_UMBILICAL,
    POLL_FD_KINDS
};

static const char *sPollFdNames[POLL_FD_KINDS] =
{
    [POLL_FD_CHILD]     = "child",
    [POLL_FD_SIGNAL]    = "signal",
    [POLL_FD_STDOUT]    = "stdout",
    [POLL_FD_STDIN]     = "stdin",
    [POLL_FD_CLOCK]     = "clock",
    [POLL_FD_UMBILICAL] = "umbilical",
};

struct PollFdAction
{
    void (*mAction)(void                        *self,
                    struct pollfd               *aPollFds,
                    const struct EventClockTime *aPollTime);
    void  *mSelf;
};

static const unsigned sPollInputEvents     = POLLHUP|POLLERR|POLLPRI|POLLIN;
static const unsigned sPollOutputEvents    = POLLHUP|POLLERR|POLLOUT;
static const unsigned sPollDisconnectEvent = POLLHUP|POLLERR;

/* -------------------------------------------------------------------------- */
enum PollFdTimerKind
{
    POLL_FD_TIMER_TETHER,
    POLL_FD_TIMER_ORPHAN,
    POLL_FD_TIMER_KINDS
};

static const char *sPollFdTimerNames[POLL_FD_TIMER_KINDS] =
{
    [POLL_FD_TIMER_TETHER] = "tether",
    [POLL_FD_TIMER_ORPHAN] = "orphan",
};

struct PollFdTimerAction
{
    void                (*mAction)(void                        *self,
                                   struct PollFdTimerAction    *aPollFdTimer,
                                   const struct EventClockTime *aPollTime,
                                   const struct Duration       *aLapTime);
    void                 *mSelf;
    struct Duration       mPeriod;
    struct EventClockTime mSince;
};

/* -------------------------------------------------------------------------- */
static pid_t
runChild(
    char              **aCmd,
    struct StdFdFiller *aStdFdFiller,
    struct Pipe        *aTetherPipe,
    struct UnixSocket  *aUmbilicalSocket,
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
        childPid = getpid();

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

        do
        {
            /* Close the reading end of the tether pipe separately
             * because it might turn out that the writing end
             * will not need to be duplicated. */

            if (closePipeReader(aTetherPipe))
                terminate(
                    errno,
                    "Unable to close tether pipe reader");

            /* Configure the environment variables of the child so that
             * it can find and monitor the tether to the watchdog. */

            if (gOptions.mTether && ! gOptions.mCordless)
            {
                const char *pidEnv = setEnvPid("K9_PID", childPid);
                if ( ! pidEnv)
                    terminate(
                        errno,
                        "Unable to set K9_PID=%jd", (intmax_t) childPid);
                debug(0, "env - K9_PID=%s", pidEnv);

                const char *lockFileName = ownProcessLockPath();

                if ( ! lockFileName)
                    terminate(
                        0,
                        "Process lock file unavailable");

                const char *lockEnv = setEnvString("K9_LOCK", lockFileName);
                if ( ! lockEnv)
                    terminate(
                        errno,
                        "Unable to set K9_LOCK=%s", lockFileName);
                debug(0, "env - K9_LOCK=%s", lockEnv);

                struct MonotonicTime baseTime = ownProcessBaseTime();

                const char *basetimeEnv = setEnvUInt64("K9_TIME",
                                                       baseTime.monotonic.ns);
                if ( ! basetimeEnv)
                    terminate(
                        errno,
                        "Unable to set K9_TIME=%" PRIu_NanoSeconds,
                        baseTime.monotonic.ns);
                debug(0, "env - K9_TIME=%s", basetimeEnv);

                struct sockaddr_un umbilicalSockAddr;
                if (ownUnixSocketName(aUmbilicalSocket, &umbilicalSockAddr))
                    terminate(
                        errno,
                        "Unable to find address of umbilical socket");

                char umbilicalAddr[sizeof(umbilicalSockAddr.sun_path)];
                memcpy(umbilicalAddr,
                       &umbilicalSockAddr.sun_path[1],
                       sizeof(umbilicalAddr) - 1);
                umbilicalAddr[sizeof(umbilicalAddr)-1] = 0;

                const char *umbilicalEnv =
                    setEnvString("K9_ADDR", umbilicalAddr);
                if ( ! umbilicalEnv)
                    terminate(
                        errno,
                        "Unable to set K9_ADDR=%s", umbilicalAddr);
                debug(0, "env - K9_ADDR=%s", umbilicalEnv);

                const char *sopathEnv = setEnvString("K9_SO", sK9soPath);
                if ( ! sopathEnv)
                    terminate(
                        errno,
                        "Unable to set K9_SO=%s", sK9soPath);
                debug(0, "env - K9_SO=%s", sopathEnv);

                if ( ! gOptions.mDebug)
                {
                    if (deleteEnv("K9_DEBUG") && ENOENT != errno)
                        terminate(
                            errno,
                            "Unable to remove K9_DEBUG");
                }
                else
                {
                    const char *debugEnv =
                        setEnvUInt("K9_DEBUG", gOptions.mDebug);
                    if ( ! debugEnv)
                        terminate(
                            errno,
                            "Unable to set K9_DEBUG=%u", gOptions.mDebug);
                    debug(0, "env - K9_DEBUG=%s", debugEnv);
                }

                const char *preload    = getenv("LD_PRELOAD");
                size_t      preloadlen = preload ? strlen(preload) : 0;

                static const char k9preloadfmt[] = "%s %s";

                char k9preload[
                    preloadlen + strlen(sK9soPath) + sizeof(k9preloadfmt)];

                if (preload)
                    sprintf(k9preload, k9preloadfmt, sK9soPath, preload);
                else
                    strcpy(k9preload, sK9soPath);

                const char *ldpreloadEnv = setEnvString(
                    "LD_PRELOAD", k9preload);
                if ( ! ldpreloadEnv)
                    terminate(
                        errno,
                        "Unable to set LD_PRELOAD=%s", k9preload);
                debug(0, "env - LD_PRELOAD=%s", ldpreloadEnv);
            }

            if (closeUnixSocket(aUmbilicalSocket))
                terminate(
                    errno,
                    "Unable to close umbilical socket");

            if (gOptions.mTether)
            {
                int tetherFd = *gOptions.mTether;

                if (0 > tetherFd)
                    tetherFd = aTetherPipe->mWrFile->mFd;

                char tetherArg[sizeof(int) * CHAR_BIT + 1];

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

    if (reapProcess(aChildPid, &status))
        terminate(
            errno,
            "Unable to reap child pid '%jd'",
            (intmax_t) aChildPid);

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
struct StatusCodeText
{
    char mText[sizeof(((siginfo_t *) 0)->si_code) * CHAR_BIT + sizeof("-")];
};

static const char *
createStatusCodeText(
    struct StatusCodeText *aStatusCodeText, const siginfo_t *aSigInfo)
{
    switch (aSigInfo->si_code)
    {
    default:
        sprintf(aStatusCodeText->mText, "%d", (int) aSigInfo->si_code);
        return aStatusCodeText->mText;

    case CLD_STOPPED:   return "stopped";
    case CLD_EXITED:    return "exited";
    case CLD_KILLED:    return "killed";
    case CLD_DUMPED:    return "dumped";
    case CLD_TRAPPED:   return "trapped";
    case CLD_CONTINUED: return "continued";
    }
}

/* -------------------------------------------------------------------------- */
struct PollFdClock
{
    enum PollFdKind mKind;
    struct Pipe     mClockPipe;
};

static void
pollFdClock(void                        *self_,
            struct pollfd               *aPollFds,
            const struct EventClockTime *aPollTime)
{
    struct PollFdClock *self = self_;

    ensure(POLL_FD_CLOCK == self->mKind);

    /* The clock is used to deliver SIGALRM to the process
     * periodically to ensure that blocking operations will
     * return with EINTR so that the event loop remains
     * responsive. */

    struct PollEventText pollEventText;
    debug(
        1,
        "clock tick %s",
        createPollEventText(
            &pollEventText,
            aPollFds[POLL_FD_CLOCK].revents));

    unsigned char clockTick;

    ssize_t len = read(self->mClockPipe.mRdFile->mFd, &clockTick, 1);

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

/* -------------------------------------------------------------------------- */
struct PollFdChild
{
    enum PollFdKind mKind;
    bool            mDead;
};

static void
pollFdChild(void                        *self_,
            struct pollfd               *aPollFds,
            const struct EventClockTime *aPollTime)
{
    struct PollFdChild *self = self_;

    ensure(POLL_FD_CHILD == self->mKind);

        /* Record when the child has terminated, but do not exit
         * the event loop until all the IO has been flushed. */

    struct PollEventText pollEventText;
    debug(
        1,
        "poll child %s",
        createPollEventText(
            &pollEventText,
            aPollFds[POLL_FD_CHILD].revents));

    ensure(aPollFds[POLL_FD_CHILD].events);

    aPollFds[POLL_FD_CHILD].events = 0;

    self->mDead = true;
}

/* -------------------------------------------------------------------------- */
struct PollFdSignal
{
    enum PollFdKind mKind;
    pid_t           mChildPid;
    struct Pipe    *mSigPipe;
};

static void
pollFdSignal(void                        *self_,
             struct pollfd               *aPollFds,
             const struct EventClockTime *aPollTime)
{
    struct PollFdSignal *self = self_;

    ensure(POLL_FD_SIGNAL == self->mKind);

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

    struct PollEventText pollEventText;
    debug(
        1,
        "poll signal %s",
        createPollEventText(
            &pollEventText,
            aPollFds[POLL_FD_SIGNAL].revents));

    unsigned char sigNum;

    ssize_t len = read(self->mSigPipe->mRdFile->mFd, &sigNum, 1);

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
              (intmax_t) self->mChildPid);

        if (kill(self->mChildPid, sigNum))
        {
            if (ESRCH != errno)
                warn(
                    errno,
                    "Unable to deliver signal %d to child pid %jd",
                    sigNum,
                    (intmax_t) self->mChildPid);
        }
    }
}

/* -------------------------------------------------------------------------- */
struct PollFdUmbilical
{
    enum PollFdKind    mKind;
    pid_t              mChildPid;
    struct UnixSocket *mUmbilicalSocket;
    struct UnixSocket  mUmbilicalPeer_;
    struct UnixSocket *mUmbilicalPeer;
};

static int
pollFdUmbilicalAccept_(struct UnixSocket       *aPeer,
                       const struct UnixSocket *aServer,
                       pid_t                    aChildPid)
{
    int rc = -1;

    struct UnixSocket *peersocket = 0;

    if (acceptUnixSocket(aPeer, aServer))
        goto Finally;

    peersocket = aPeer;

    /* Require that the remote peer be the process being monitored.
     * The connection will be dropped if the process uses execv() to
     * run another program, and then re-established when the new
     * program creates its own umbilical connection. */

    struct ucred cred;

    if (ownUnixSocketPeerCred(aPeer, &cred))
        goto Finally;

    debug(1, "umbilical connection from pid %jd", (intmax_t) cred.pid);

    if (cred.pid != aChildPid)
    {
        errno = EPERM;
        goto Finally;
    }

    /* There is nothing read from the umbilical connection, so shut down
     * the reading side here. Do not shut down the writing side leaving
     * the umbilical half-open to allow it to be used to signal to
     * the child process if the watchdog terminates. */

    if (shutdownUnixSocketReader(peersocket))
        goto Finally;

    rc = 0;

Finally:

    FINALLY(
    {
        if (rc)
            closeUnixSocket(peersocket);
    });

    return rc;
}

static void
pollFdUmbilical(void                        *self_,
                struct pollfd               *aPollFds,
                const struct EventClockTime *aPollTime)
{
    struct PollFdUmbilical *self = self_;

    ensure(POLL_FD_UMBILICAL == self->mKind);

    /* Process an inbound connection from the child process on its
     * umbilical socket. The parasite watchdog library attached to the
     * child will use this to detect if the watchdog has terminated. */

    struct PollEventText pollEventText;
    debug(
        1,
        "poll umbilical %s",
        createPollEventText(
            &pollEventText,
            aPollFds[POLL_FD_UMBILICAL].revents));

    if (aPollFds[POLL_FD_UMBILICAL].revents & POLLIN)
    {
        if (closeUnixSocket(self->mUmbilicalPeer))
            terminate(
                errno,
                "Unable to close umbilical peer");
        self->mUmbilicalPeer = 0;

        if ( ! pollFdUmbilicalAccept_(&self->mUmbilicalPeer_,
                                      self->mUmbilicalSocket,
                                      self->mChildPid))
            self->mUmbilicalPeer = &self->mUmbilicalPeer_;
    }
}

/* -------------------------------------------------------------------------- */
struct TerminationAgent
{
    struct EventClockTime mSince;     /* Termination timeline */
    int                   mTriggered; /* Termination in progress */
};

struct PollFdTether
{
    enum PollFdKind mKind;

    struct
    {
        int                   mPeriod_ms;  /* Timeout period if not -1 */
        struct EventClockTime mSince;      /* Last activity on tether */
        unsigned              mCycleCount; /* Current number of cycles */
        unsigned              mCycleLimit; /* Cycles before triggering */
        bool                  mTriggered;  /* Timeout triggered */
    } mTimeoutx;

    pid_t                     mChildPid;
    struct TerminationAgent  *mTerminationAgent;
    struct PollFdTimerAction *mTimer;

    struct
    {
        int mStdout;
        int mStdin;
    } mClosed;

    struct Pipe *mNullPipe;
};

static void
resetPollFdTetherTimeout(struct PollFdTether         *self,
                         const struct EventClockTime *aPollTime)
{
    lapTimeRestart(&self->mTimer->mSince, aPollTime);
    self->mTimeoutx.mCycleCount = 0;
}

static void
pollFdStdin(void                        *self_,
            struct pollfd               *aPollFds,
            const struct EventClockTime *aPollTime)
{
    struct PollFdTether *self = self_;

    ensure(POLL_FD_STDIN == self->mKind);

    ensure(STDIN_FILENO == aPollFds[POLL_FD_STDIN].fd);
    ensure( ! self->mClosed.mStdin);

    resetPollFdTetherTimeout(self, aPollTime);

    struct PollEventText pollEventText;
    debug(
        1,
        "poll stdin %s",
        createPollEventText(
            &pollEventText,
            aPollFds[POLL_FD_STDIN].revents));

    ensure(aPollFds[POLL_FD_STDIN].events);

    if ( ! (aPollFds[POLL_FD_STDIN].revents & POLLIN))
        self->mClosed.mStdin = -1;
    else
    {
        aPollFds[POLL_FD_STDIN].fd = self->mNullPipe->mRdFile->mFd;

        aPollFds[POLL_FD_STDOUT].events = sPollOutputEvents;
        aPollFds[POLL_FD_STDIN].events  = sPollDisconnectEvent;
    }
}

static void
pollFdStdout(void                        *self_,
             struct pollfd               *aPollFds,
             const struct EventClockTime *aPollTime)
{
    struct PollFdTether *self = self_;

    ensure(POLL_FD_STDIN == self->mKind);

    ensure(STDOUT_FILENO == aPollFds[POLL_FD_STDOUT].fd);
    ensure( ! self->mClosed.mStdout);

    resetPollFdTetherTimeout(self, aPollTime);

    struct PollEventText pollEventText;
    debug(
        1,
        "poll stdout %s",
        createPollEventText(
            &pollEventText,
            aPollFds[POLL_FD_STDOUT].revents));

    ensure(aPollFds[POLL_FD_STDOUT].events);

    do
    {
        if (aPollFds[POLL_FD_STDOUT].revents & POLLOUT)
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

                        /* Although SPLICE_F_NONBLOCK is used, the underlying
                         * file descriptor for stdout might be blocking and
                         * must not be made non-blocking since that file
                         * descriptor was likely inherited and might be
                         * shared with other processes.
                         *
                         * Rely on the periodic SIGALRM to force splice
                         * to return with EINTR if the call ends up blocking.
                         * This means that it is important that EINTR
                         * not simply retry the splice, but re-run the
                         * entire event loop. */

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
                    aPollFds[POLL_FD_STDIN].fd = STDIN_FILENO;

                    aPollFds[POLL_FD_STDOUT].events= sPollDisconnectEvent;
                    aPollFds[POLL_FD_STDIN].events = sPollInputEvents;
                }
                break;
            }
        }

        self->mClosed.mStdout = -1;
        break;

    } while (0);
}

static void
pollFdTimerTether(void                        *self_,
                  struct PollFdTimerAction    *aPollFdTimerAction,
                  const struct EventClockTime *aPollTime,
                  const struct Duration       *aLapTime)
{
    struct PollFdTether *self = self_;

    ensure(POLL_FD_STDIN == self->mKind);

    /* The tether timer is only active if there is a tether and it was
     * configured with a timeout. The timeout expires if there was
     * no activity on the tether with the consequence that the monitored
     * child will be terminated. */

    do
    {
        siginfo_t siginfo;

        siginfo.si_pid = 0;
        if ( ! waitid(P_PID,
                      self->mChildPid,
                      &siginfo,
                      WSTOPPED | WNOHANG | WNOWAIT))
        {
            if (siginfo.si_pid == self->mChildPid &&
                (siginfo.si_code == CLD_TRAPPED ||
                 siginfo.si_code == CLD_STOPPED))
            {
                struct StatusCodeText statusCodeText;

                debug(
                    0,
                    "deferred timeout after %" PRIs_MilliSeconds
                    " with child status %s",
                    FMTs_MilliSeconds(MSECS(aLapTime->duration)),
                    createStatusCodeText(&statusCodeText, &siginfo));

                self->mTimeoutx.mCycleCount = 0;
                break;
            }

            if (++self->mTimeoutx.mCycleCount < self->mTimeoutx.mCycleLimit)
                break;

            self->mTimeoutx.mCycleCount = self->mTimeoutx.mCycleLimit;
        }
        else
        {
            if (ECHILD != errno)
                terminate(
                    errno,
                    "Unable to check for status of child pid %jd",
                    (intmax_t) self->mChildPid);

            /* The child process is no longer active, so it makes
             * sense to proceed as if the child process should
             * be terminated. */
        }

        /* Once the timeout has expired, the timer can be cancelled because
         * there is no further need to run this state machine. */

        debug(0, "timeout after %ds", gOptions.mTimeout_s);

        aPollFdTimerAction->mPeriod = duration(NanoSeconds(0));

        if ( ! self->mTerminationAgent->mTriggered)
        {
            self->mTerminationAgent->mTriggered = -1;
            self->mTerminationAgent->mSince     = *aPollTime;
        }

    } while (0);
}

/* -------------------------------------------------------------------------- */
struct PollFdTimerOrphan
{
    enum PollFdTimerKind mKind;

    struct TerminationAgent *mTerminationAgent;
};

void
pollFdTimerOrphan(void                        *self_,
                  struct PollFdTimerAction    *aPollFdTimerAction,
                  const struct EventClockTime *aPollTime,
                  const struct Duration       *aLapTime)
{
    struct PollFdTimerOrphan *self = self_;

    ensure(POLL_FD_TIMER_ORPHAN == self->mKind);

    /* Using PR_SET_PDEATHSIG is very attractive however the detailed
     * discussion at the end of this thread is important:
     *
     * https://bugzilla.kernel.org/show_bug.cgi?id=43300
     *
     * In the most general case, PR_SET_PDEATHSIG is useless because
     * it tracks the termination of the parent thread, not the parent
     * process. */

    if (1 == getppid())
    {
        debug(0, "orphaned");

        aPollFdTimerAction->mPeriod = duration(NanoSeconds(0));

        if ( ! self->mTerminationAgent->mTriggered)
        {
            self->mTerminationAgent->mTriggered = -1;
            self->mTerminationAgent->mSince     = *aPollTime;
        }
    }
}

/* -------------------------------------------------------------------------- */
static void
monitorChild(pid_t              aChildPid,
             struct UnixSocket *aUmbilicalSocket,
             struct Pipe       *aTermPipe,
             struct Pipe       *aSigPipe)
{
    debug(0, "start monitoring child");

    struct PollFdTimerAction pollfdtimeractions[POLL_FD_TIMER_KINDS] =
    {
        [POLL_FD_TIMER_TETHER] =
        {
            0, 0, duration(NSECS(Seconds(0)))
        },

        [POLL_FD_TIMER_ORPHAN] =
        {
            0, 0, duration(NSECS(Seconds(0)))
        },
    };

    struct PollEventText pollEventText;
    struct PollEventText pollRcvdEventText;

    struct Pipe nullPipe;
    if (createPipe(&nullPipe))
        terminate(
            errno,
            "Unable to create null pipe");

    struct PollFdClock pollfdclock = { .mKind = POLL_FD_CLOCK };

    if (createPipe(&pollfdclock.mClockPipe))
        terminate(
            errno,
            "Unable to create clock pipe");
    if (closePipeOnExec(&pollfdclock.mClockPipe, O_CLOEXEC))
        terminate(
            errno,
            "Unable to set close on exec for clock pipe");

    struct timeval clockPeriod = { .tv_sec = 3 };

    if (watchProcessClock(&pollfdclock.mClockPipe, &clockPeriod))
        terminate(
            errno,
            "Unable to install process clock watch");

    struct PollFdChild pollfdchild =
    {
        .mKind  = POLL_FD_CHILD,
        .mDead = false,
    };

    struct PollFdSignal pollfdsignal =
    {
        .mKind     = POLL_FD_SIGNAL,
        .mChildPid = aChildPid,
        .mSigPipe  = aSigPipe,
    };

    struct PollFdUmbilical pollfdumbilical =
    {
        .mKind            = POLL_FD_UMBILICAL,
        .mChildPid        = aChildPid,
        .mUmbilicalSocket = aUmbilicalSocket,
        .mUmbilicalPeer   = 0,
    };

    struct TerminationAgent termination =
    {
        .mTriggered = false,
    };

    int timeout_ms = gOptions.mTimeout_s * 1000;

    if (timeout_ms / 1000 != gOptions.mTimeout_s || 0 > timeout_ms)
        terminate(
            0,
            "Invalid timeout value %d", gOptions.mTimeout_s);

    if ( ! gOptions.mTether)
        timeout_ms = 0;

    if ( ! timeout_ms)
        timeout_ms = -1;

    /* Divide the timeout into two cycles so that if the child process is
     * stopped, the first cycle will have a chance to detect it and
     * defer the timeout. */

    const unsigned timeoutCycles = 2;

    struct PollFdTether pollfdtether =
    {
        .mKind = POLL_FD_STDIN,

        .mTimeoutx =
        {
            .mCycleCount = 0,
            .mCycleLimit = timeoutCycles,
            .mPeriod_ms  = 0 > timeout_ms ? -1 : timeout_ms / timeoutCycles,
            .mSince      = eventclockTime(),
            .mTriggered  = false,
        },

        .mChildPid         = aChildPid,
        .mTerminationAgent = &termination,
        .mTimer            = &pollfdtimeractions[POLL_FD_TIMER_TETHER],

        .mClosed =
        {
            .mStdout = gOptions.mTether ? 0 : -1,
            .mStdin  = gOptions.mTether ? 0 : -1,
        },

        .mNullPipe = &nullPipe,
    };

    pollfdtether.mTimer->mAction = pollFdTimerTether;
    pollfdtether.mTimer->mSelf   = &pollfdtether;
    pollfdtether.mTimer->mSince  = EVENTCLOCKTIME_INIT;
    pollfdtether.mTimer->mPeriod =
        duration(NanoSeconds(
            NSECS(Seconds(
                gOptions.mTether
                ? gOptions.mTimeout_s : 0)).ns / timeoutCycles));

    /* If requested to be aware when the watchdog becomes an orphan,
     * check if init(8) is the parent of this process. If this is
     * detected, start sending signals to the child to encourage it
     * to exit. */

    struct PollFdTimerOrphan pollfdtimerorphan =
    {
        .mKind = POLL_FD_TIMER_ORPHAN,

        .mTerminationAgent = &termination,
    };

    if (gOptions.mOrphaned)
    {
        pollfdtimeractions[POLL_FD_TIMER_ORPHAN].mAction = pollFdTimerOrphan;
        pollfdtimeractions[POLL_FD_TIMER_ORPHAN].mSelf   = &pollfdtimerorphan;
        pollfdtimeractions[POLL_FD_TIMER_ORPHAN].mPeriod =
            duration(NSECS(Seconds(3)));
    }

    /* Experiments at http://www.greenend.org.uk/rjk/tech/poll.html show
     * that it is best not to put too much trust in POLLHUP vs POLLIN,
     * and to treat the presence of either as a trigger to attempt to
     * read from the file descriptor.
     *
     * For the writing end of the pipe, Linux returns POLLERR if the
     * far end reader is no longer available (to match EPIPE), but
     * the documentation suggests that POLLHUP might also be reasonable
     * in this context. */

    struct pollfd pollfds[POLL_FD_KINDS] =
    {
        [POLL_FD_CLOCK] = {
            .fd     = pollfdclock.mClockPipe.mRdFile->mFd,
            .events = sPollInputEvents },
        [POLL_FD_CHILD] = {
            .fd     = aTermPipe->mRdFile->mFd,
            .events = sPollInputEvents },
        [POLL_FD_SIGNAL] = {
            .fd     = aSigPipe->mRdFile->mFd,
            .events = sPollInputEvents },
        [POLL_FD_UMBILICAL] = {
            .fd     = aUmbilicalSocket->mFile->mFd,
            .events = sPollInputEvents },
        [POLL_FD_STDIN] = {
            .fd     = STDIN_FILENO,
            .events = sPollInputEvents, },
        [POLL_FD_STDOUT] = {
            .fd     = STDOUT_FILENO,
            .events = sPollDisconnectEvent },
    };

    struct PollFdAction pollfdactions[POLL_FD_KINDS] =
    {
        [POLL_FD_CLOCK]     = { pollFdClock,     &pollfdclock },
        [POLL_FD_CHILD]     = { pollFdChild,     &pollfdchild },
        [POLL_FD_SIGNAL]    = { pollFdSignal,    &pollfdsignal },
        [POLL_FD_UMBILICAL] = { pollFdUmbilical, &pollfdumbilical },
        [POLL_FD_STDIN]     = { pollFdStdin,     &pollfdtether },
        [POLL_FD_STDOUT]    = { pollFdStdout,    &pollfdtether },
    };

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

    /* It would be so much easier to use non-blocking IO, but O_NONBLOCK
     * is an attribute of the underlying open file, not of each
     * file descriptor. Since stdin and stdout are typically inherited
     * from the parent, setting O_NONBLOCK would affect all file descriptors
     * referring to the same open file, so this approach cannot be
     * dinner. */

    struct EventClockTime polltm;

    do
    {
        if (0 > pollfdtether.mClosed.mStdout ||
            0 > pollfdtether.mClosed.mStdin)
        {
            pollfdtether.mClosed.mStdout = 1;
            pollfdtether.mClosed.mStdin  = 1;

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

            pollfds[POLL_FD_STDOUT].events = sPollDisconnectEvent;
            pollfds[POLL_FD_STDIN].events  = sPollDisconnectEvent;
        }

        ensure(pollfdtether.mClosed.mStdin == pollfdtether.mClosed.mStdout);

        /* Poll the file descriptors and process the file descriptor
         * events before attempting to check for timeouts. This
         * order of operations is important to deal robustly with
         * slow clocks and stoppages. */

        polltm = eventclockTime();

        struct Duration timeout   = duration(NanoSeconds(0));
        size_t          numActive = 0;

        for (size_t ix = 0; NUMBEROF(pollfdtimeractions) > ix; ++ix)
        {
            if (pollfdtimeractions[ix].mPeriod.duration.ns)
            {
                ++numActive;

                struct Duration remaining;

                if (deadlineTimeExpired(
                        &pollfdtimeractions[ix].mSince,
                        pollfdtimeractions[ix].mPeriod,
                        &remaining,
                        &polltm))
                {
                    timeout = duration(NanoSeconds(0));
                    break;
                }

                if (timeout.duration.ns >
                    remaining.duration.ns || ! timeout.duration.ns)
                    timeout = remaining;
            }
        }

        int timeout_ms;

        if ( ! numActive)
            timeout_ms = -1;
        else
        {
            struct MilliSeconds timeoutDuration = MSECS(timeout.duration);

            timeout_ms = timeoutDuration.ms;

            if (0 > timeout_ms || timeoutDuration.ms != timeout_ms)
                timeout_ms = INT_MAX;
        }

        debug(1, "poll wait %dms", timeout_ms);

        int rc = poll(pollfds, NUMBEROF(pollfds), timeout_ms);

        if (-1 == rc)
        {
            if (EINTR == errno)
                continue;

            terminate(
                errno,
                "Unable to poll for activity");
        }

        /* Latch the event clock time here before quickly polling the
         * file descriptors again. Deadlines will be compared against
         * this latched time */

        polltm = eventclockTime();

        RACE
        ({
            while (1)
            {
                rc = poll(pollfds, NUMBEROF(pollfds), 0);

                if (-1 == rc)
                {
                    if (EINTR == errno)
                        continue;

                    terminate(
                        errno,
                        "Unable to poll for activity");
                }

                break;
            }
        });

        {
            /* When processing file descriptor events, do not loop in EINTR
             * but instead allow the polling cycle to be re-run so that
             * the event loop will not remain stuck processing a single
             * file descriptor. */

            unsigned eventCount = 0;

            if ( ! rc)
                ++eventCount;

            /* The poll(2) call will mark POLLNVAL, POLLERR or POLLHUP
             * no matter what the caller has subscribed for. Only pay
             * attention to what was subscribed. */

            debug(1, "poll scan of %d fds", rc);

            for (size_t ix = 0; NUMBEROF(pollfds) > ix; ++ix)
            {
                debug(
                    1,
                    "poll %s %d (%s) (%s)",
                    sPollFdNames[ix],
                    pollfds[ix].fd,
                    createPollEventText(
                        &pollEventText, pollfds[ix].events),
                    createPollEventText(
                        &pollRcvdEventText, pollfds[ix].revents));

                pollfds[ix].revents &= pollfds[ix].events;

                if (pollfds[ix].revents)
                {
                    ensure(rc);

                    ++eventCount;

                    if (pollfdactions[ix].mAction)
                        pollfdactions[ix].mAction(
                            pollfdactions[ix].mSelf,
                            pollfds,
                            &polltm);
                }
            }

            /* Ensure that the interpretation of the poll events is being
             * correctly handled, to avoid a busy-wait poll loop. */

            ensure(eventCount);
        }

        /* With the file descriptors processed, any timeouts have had
         * a chance to be recalibrated, and now the timers can be
         * processed. */

        for (size_t ix = 0; NUMBEROF(pollfdtimeractions) > ix; ++ix)
        {
            if (pollfdtimeractions[ix].mPeriod.duration.ns)
            {
                if (deadlineTimeExpired(
                        &pollfdtimeractions[ix].mSince,
                        pollfdtimeractions[ix].mPeriod,
                        0,
                        &polltm))
                {
                    /* Compute the lap time, and as a side-effect set
                     * the deadline for the next timer cycle. This means
                     * that the timer action need not do anything to
                     * prepare for the next timer cycle, unless it needs
                     * to cancel the timer. */

                    struct Duration lapTime = lapTimeSince(
                        &pollfdtimeractions[ix].mSince,
                        pollfdtimeractions[ix].mPeriod,
                        &polltm);

                    debug(1, "expire %s timer after %" PRIs_MilliSeconds,
                          sPollFdTimerNames[ix],
                          FMTs_MilliSeconds(MSECS(lapTime.duration)));

                    pollfdtimeractions[ix].mAction(
                        pollfdtimeractions[ix].mSelf,
                        &pollfdtimeractions[ix],
                        &polltm,
                        &lapTime);
                }
            }
        }

        if (termination.mTriggered)
        {
            unsigned elapsedTime_s =
                termination.mTriggered < 0
                ? gOptions.mPacing_s
                : SECS(
                    lapTimeSince(
                        &termination.mSince,
                        duration(NSECS(Seconds(gOptions.mPacing_s))),
                        &polltm).duration).s;

            debug(1, "post mortem clock %us", elapsedTime_s);

            if (gOptions.mPacing_s <= elapsedTime_s)
            {
                termination.mTriggered = 1;

                pid_t pidNum = childSignalPlan->mPid;
                int   sigNum = childSignalPlan->mSig;

                if (childSignalPlan[1].mPid)
                    ++childSignalPlan;

                warn(
                    0,
                    "Killing child pid %jd with signal %d",
                    (intmax_t) pidNum,
                    sigNum);

                if (kill(pidNum, sigNum) && ESRCH != errno)
                    terminate(
                        errno,
                        "Unable to kill child pid %jd with signal %d",
                        (intmax_t) pidNum,
                        sigNum);
            }
        }

    } while ( ! pollfdchild.mDead ||
                sPollOutputEvents == pollfds[POLL_FD_STDOUT].events ||
                sPollInputEvents  == pollfds[POLL_FD_STDIN].events);

    if (closeUnixSocket(pollfdumbilical.mUmbilicalPeer))
        terminate(
            errno,
            "Unable to close umbilical peer");

    if (unwatchProcessClock())
        terminate(
            errno,
            "Unable to remove process clock watch");

    if (closePipe(&pollfdclock.mClockPipe))
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
                pidFileTime.tv_nsec = NSECS(MilliSeconds(900)).ns;

            for (uint64_t resolution = 1000; ; resolution *= 10)
            {
                if (pidFileTime.tv_nsec % resolution)
                {
                    ensure(resolution);

                    debug(0, "delay for %" PRIu64 "ns", resolution);

                    monotonicSleep(duration(NanoSeconds(resolution)));

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

    struct UnixSocket umbilicalSocket;
    if (createUnixSocket(&umbilicalSocket, 0, 0, 0))
        terminate(
            errno,
            "Unable to create umbilical socket");

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
                              &tetherPipe, &umbilicalSocket,
                              &syncPipe, &termPipe, &sigPipe);
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

    /* Discard the origin stdin file descriptor, and instead attach
     * the reading end of the tether as stdin. This means that the
     * watchdog does not contribute any more references to the
     * original stdin file table entry. */

    if (STDIN_FILENO != dup2(tetherPipe.mRdFile->mFd, STDIN_FILENO))
        terminate(
            errno,
            "Unable to dup tether pipe to stdin");

    /* Avoid closing the original stdout file descriptor only if
     * there is a need to copy the contents of the tether to it.
     * Otherwise, close the original stdout and open it as a sink so
     * that the watchdog does not contribute any more references to the
     * original stdout file table entry. */

    bool discardStdout = gOptions.mQuiet;

    if ( ! gOptions.mTether)
        discardStdout = true;
    else
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

    if (closePipe(&tetherPipe))
        terminate(
            errno,
            "Unable to close tether pipe");

    if (purgeProcessOrphanedFds())
        terminate(
            errno,
            "Unable to purge orphaned files");

    /* Monitor the running child until it has either completed of
     * its own accord, or terminated. Once the child has stopped
     * running, release the pid file if one was allocated. */

    monitorChild(childPid, &umbilicalSocket, &termPipe, &sigPipe);

    /* With the running child terminated, it is ok to close the
     * umbilical pipe because the child has no more use for it. */

    if (closeUnixSocket(&umbilicalSocket))
        terminate(
            errno,
            "Unable to close umbilical socket");

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
static void
initK9soPath(void)
{
    const char *k9soMain = STRINGIFY(K9SO_MAIN);

    const char *err;

    sK9soPath = findDlSymbol(k9soMain, 0, &err);

    if ( ! sK9soPath)
    {
        if (err)
            terminate(
                0,
                "Unable to resolve shared library of symbol %s", k9soMain);
        else
            terminate(
                0,
                "Unable to resolve address of symbol %s - %s",
                k9soMain,
                err);
    }
}

/* -------------------------------------------------------------------------- */
int
K9SO_MAIN(int argc, char **argv)
{
    if (Timekeeping_init())
        terminate(
            0,
            "Unable to initialise timekeeping module");

    if (Process_init(argv[0]))
        terminate(
            errno,
            "Unable to initialise process state");

    initK9soPath();

    struct ExitCode exitCode;

    {
        char **cmd = processOptions(argc, argv);

        if ( ! cmd && gOptions.mPidFile)
            exitCode = cmdPrintPidFile(gOptions.mPidFile);
        else
            exitCode = cmdRunCommand(cmd);
    }

    if (Process_exit())
        terminate(
            errno,
            "Unable to finalise process state");

    if (Timekeeping_exit())
        terminate(
            0,
            "Unable to finalise timekeeping module");

    return exitCode.mStatus;
}

/* -------------------------------------------------------------------------- */
