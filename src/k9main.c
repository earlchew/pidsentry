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
#include "thread_.h"
#include "error_.h"
#include "pollfd_.h"
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
 * monitorChild() is too big, break it up
 * Periodically poll umbilical
 * Correct monitoring child scheduling of competing deadlines
 * Add test case for SIGKILL of watchdog and child not watching tether
 * Check for useless #include in *.c
 * Fix logging in parasite printing null for program name
 * Fix vgcore being dropped everywhere
 */

#define DEVNULLPATH "/dev/null"

static const char sDevNullPath[] = DEVNULLPATH;

static const char *sK9soPath;

/* -------------------------------------------------------------------------- */
enum PollFdKind
{
    POLL_FD_TETHER,
    POLL_FD_CHILD,
    POLL_FD_UMBILICAL,
    POLL_FD_KINDS
};

static const char *sPollFdNames[POLL_FD_KINDS] =
{
    [POLL_FD_TETHER]    = "tether",
    [POLL_FD_CHILD]     = "child",
    [POLL_FD_UMBILICAL] = "umbilical",
};

/* -------------------------------------------------------------------------- */
enum PollFdTimerKind
{
    POLL_FD_TIMER_TETHER,
    POLL_FD_TIMER_UMBILICAL,
    POLL_FD_TIMER_ORPHAN,
    POLL_FD_TIMER_TERMINATION,
    POLL_FD_TIMER_DISCONNECTION,
    POLL_FD_TIMER_KINDS
};

static const char *sPollFdTimerNames[POLL_FD_TIMER_KINDS] =
{
    [POLL_FD_TIMER_TETHER]        = "tether",
    [POLL_FD_TIMER_UMBILICAL]     = "umbilical",
    [POLL_FD_TIMER_ORPHAN]        = "orphan",
    [POLL_FD_TIMER_TERMINATION]   = "termination",
    [POLL_FD_TIMER_DISCONNECTION] = "disconnection",
};

/* -------------------------------------------------------------------------- */
struct ChildProcess
{
    pid_t mPid;

    struct UnixSocket  mUmbilicalSocket;
    struct Pipe        mTermPipe;
    struct Pipe        mTetherPipe_;
    struct Pipe       *mTetherPipe;
};

/* -------------------------------------------------------------------------- */
static void
createChild(struct ChildProcess *self)
{
    self->mPid = 0;

    /* Only the reading end of the tether is marked non-blocking. The
     * writing end must be used by the child process (and perhaps inherited
     * by any subsequent process that it forks), so only the reading
     * end is marked non-blocking. */

    if (createPipe(&self->mTetherPipe_, 0))
        terminate(
            errno,
            "Unable to create tether pipe");
    self->mTetherPipe = &self->mTetherPipe_;

    if (closeFileOnExec(self->mTetherPipe->mRdFile, O_CLOEXEC))
        terminate(
            errno,
            "Unable to set close on exec for tether");

    if (nonblockingFile(self->mTetherPipe->mRdFile))
        terminate(
            errno,
            "Unable to mark tether non-blocking");

    if (createUnixSocket(&self->mUmbilicalSocket, 0, 0, 0))
        terminate(
            errno,
            "Unable to create umbilical socket");

    if (createPipe(&self->mTermPipe, O_CLOEXEC | O_NONBLOCK))
        terminate(
            errno,
            "Unable to create termination pipe");
}

/* -------------------------------------------------------------------------- */
static void
reapChild(void *self_)
{
    struct ChildProcess *self = self_;

    /* Check that the child process being monitored is the one
     * is the subject of the signal. Here is a way for a parent
     * to be surprised by the presence of an adopted child:
     *
     *  sleep 5 & exec sh -c 'sleep 1 & wait'
     *
     * The new shell inherits the earlier sleep as a child even
     * though it did not create it. */

    enum ProcessStatus childstatus = monitorProcess(self->mPid);

    if (ProcessStatusError == childstatus)
        terminate(
            errno,
            "Unable to determine status of pid %jd",
            (intmax_t) self->mPid);

    if (ProcessStatusExited != childstatus &&
        ProcessStatusKilled != childstatus &&
        ProcessStatusDumped != childstatus)
    {
        debug(1,
              "child not yet terminated pid %jd status %c",
              (intmax_t) self->mPid, childstatus);
    }
    else
    {
        if (closePipeWriter(&self->mTermPipe))
            terminate(
                errno,
                "Unable to close termination pipe writer");
    }
}

/* -------------------------------------------------------------------------- */
static void
killChild(void *self_, int aSigNum)
{
    struct ChildProcess *self = self_;

    if (self->mPid)
    {
        debug(0,
              "sending signal %d to child pid %jd",
              aSigNum,
              (intmax_t) self->mPid);

        if (kill(self->mPid, aSigNum))
        {
            if (ESRCH != errno)
                terminate(
                    errno,
                    "Unable to deliver signal %d to child pid %jd",
                    aSigNum,
                    (intmax_t) self->mPid);
        }
    }
}

/* -------------------------------------------------------------------------- */
static int
forkChild(
    struct ChildProcess          *self,
    char                        **aCmd,
    struct StdFdFiller           *aStdFdFiller,
    struct Pipe                  *aSyncPipe,
    struct PushedProcessSigMask  *aSigMask)
{
    int rc = -1;

    /* Both the parent and child share the same signal handler configuration.
     * In particular, no custom signal handlers are configured, so
     * signals delivered to either will likely caused them to terminate.
     *
     * This is safe because that would cause one of end the termPipe
     * to close, and the other end will eventually notice. */

    pid_t watchdogPid = getpid();

    pid_t childPid = forkProcess(
        gOptions.mSetPgid
        ? ForkProcessSetProcessGroup
        : ForkProcessShareProcessGroup);

    if (-1 == childPid)
        goto Finally;

    if ( ! childPid)
    {
        childPid = getpid();

        debug(0, "starting child process");

        /* The forked child has all its signal handlers reset, but
         * note that the parent will wait for the child to synchronise
         * before sending it signals, so that there is no race here.
         *
         * Close the StdFdFiller in case this will free up stdin, stdout or
         * stderr. The remaining operations will close the remaining
         * unwanted file descriptors. */

        if (closeStdFdFiller(aStdFdFiller))
            terminate(
                errno,
                "Unable to close stdin, stdout and stderr fillers");

        if (closePipe(&self->mTermPipe))
            terminate(
                errno,
                "Unable to close termination pipe");

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

            if (closePipeReader(self->mTetherPipe))
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

                const char *ppidEnv = setEnvPid("K9_PPID", watchdogPid);
                if ( ! ppidEnv)
                    terminate(
                        errno,
                        "Unable to set K9_PPID=%jd", (intmax_t) watchdogPid);
                debug(0, "env - K9_PPID=%s", ppidEnv);

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
                if (ownUnixSocketName(
                        &self->mUmbilicalSocket, &umbilicalSockAddr))
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

                const char *umbilicalTimeoutEnv =
                    setEnvUInt("K9_TIMEOUT", gOptions.mTimeout.mUmbilical_s);
                if ( ! umbilicalTimeoutEnv)
                    terminate(
                        errno,
                        "Unable to set K9_TIMEOUT=%u",
                        gOptions.mTimeout.mUmbilical_s);
                debug(0, "env - K9_TIMEOUT=%s", umbilicalTimeoutEnv);

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

            if (closeUnixSocket(&self->mUmbilicalSocket))
                terminate(
                    errno,
                    "Unable to close umbilical socket");

            if (gOptions.mTether)
            {
                int tetherFd = *gOptions.mTether;

                if (0 > tetherFd)
                    tetherFd = self->mTetherPipe->mWrFile->mFd;

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

                if (tetherFd == self->mTetherPipe->mWrFile->mFd)
                    break;

                if (dup2(self->mTetherPipe->mWrFile->mFd, tetherFd) != tetherFd)
                    terminate(
                        errno,
                        "Unable to dup tether pipe fd %d to fd %d",
                        self->mTetherPipe->mWrFile->mFd,
                        tetherFd);
            }

            if (closePipe(self->mTetherPipe))
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

    debug(0, "running child process %jd", (intmax_t) childPid);

    self->mPid = childPid;

    rc = 0;

Finally:

    FINALLY({});

    return rc;
}

/* -------------------------------------------------------------------------- */
static void
closeChildTether(struct ChildProcess *self)
{
    ensure(self->mTetherPipe);

    if (closePipe(self->mTetherPipe))
        terminate(
            errno,
            "Unable to close tether pipe");
    self->mTetherPipe = 0;
}

/* -------------------------------------------------------------------------- */
static int
closeChild(struct ChildProcess *self)
{
    int status;

    if (closeUnixSocket(&self->mUmbilicalSocket))
        terminate(
            errno,
            "Unable to close umbilical socket");

    if (closePipe(self->mTetherPipe))
        terminate(
            errno,
            "Unable to close tether pipe");
    self->mTetherPipe = 0;

    if (closePipe(&self->mTermPipe))
        terminate(
            errno,
            "Unable to close termination pipe");

    if (reapProcess(self->mPid, &status))
        terminate(
            errno,
            "Unable to reap child pid '%jd'",
            (intmax_t) self->mPid);

    return status;
}

/* -------------------------------------------------------------------------- */
/* Tether Thread
 *
 * The purpose of the tether thread is to isolate the event loop
 * in the main thread from blocking that might arise when writing to
 * the destination file descriptor. The destination file descriptor
 * cannot be guaranteed to be non-blocking because it is inherited
 * when the watchdog process is started. */

enum TetherThreadState
{
    TETHER_THREAD_STOPPED,
    TETHER_THREAD_RUNNING,
    TETHER_THREAD_STOPPING,
};

struct TetherThread
{
    pthread_t    mThread;
    struct Pipe  mControlPipe;
    struct Pipe *mNullPipe;
    bool         mFlushed;

    struct {
        pthread_mutex_t       mMutex;
        struct EventClockTime mSince;
    } mActivity;

    struct {
        pthread_mutex_t        mMutex;
        pthread_cond_t         mCond;
        enum TetherThreadState mValue;
    } mState;
};

enum TetherFdKind
{
    TETHER_FD_CONTROL,
    TETHER_FD_INPUT,
    TETHER_FD_OUTPUT,
    TETHER_FD_KINDS
};

static const char *sTetherFdNames[] =
{
    [TETHER_FD_CONTROL] = "control",
    [TETHER_FD_INPUT]   = "input",
    [TETHER_FD_OUTPUT]  = "output",
};

enum TetherFdTimerKind
{
    TETHER_FD_TIMER_DISCONNECT,
    TETHER_FD_TIMER_KINDS
};

static const char *sTetherFdTimerNames[] =
{
    [TETHER_FD_TIMER_DISCONNECT] = "disconnection",
};

struct TetherPoll
{
    struct TetherThread     *mThread;
    int                      mSrcFd;
    int                      mDstFd;

    struct pollfd            mPollFds[TETHER_FD_KINDS];
    struct PollFdAction      mPollFdActions[TETHER_FD_KINDS];
    struct PollFdTimerAction mPollFdTimerActions[TETHER_FD_TIMER_KINDS];
};

static void
polltethercontrol(void                        *self_,
                  struct pollfd               *aPollFds_unused,
                  const struct EventClockTime *aPollTime)
{
    struct TetherPoll *self = self_;

    char buf[1];

    if (0 > readFd(self->mPollFds[TETHER_FD_CONTROL].fd, buf, sizeof(buf)))
        terminate(
            errno,
            "Unable to read tether control");

    debug(0, "tether disconnection request received");

    /* Note that gOptions.mTimeout.mDrain_s might be zero to indicate
     * that the no drain timeout is to be enforced. */

    self->mPollFdTimerActions[TETHER_FD_TIMER_DISCONNECT].mPeriod =
        Duration(NSECS(Seconds(gOptions.mTimeout.mDrain_s)));
}

static void
polltetherdrain(void                        *self_,
                struct pollfd               *aPollFds_unused,
                const struct EventClockTime *aPollTime)
{
    struct TetherPoll *self = self_;

    if (self->mPollFds[TETHER_FD_CONTROL].events)
    {
        {
            lockMutex(&self->mThread->mActivity.mMutex);
            self->mThread->mActivity.mSince = eventclockTime();
            unlockMutex(&self->mThread->mActivity.mMutex);
        }

        bool drained = true;

        do
        {
            /* The output file descriptor must have been closed if:
             *
             *  o There is no input available, so the poll must have
             *    returned because an output disconnection event was detected
             *  o Input was available, but none could be written to the output
             */

            int available;

            if (ioctl(self->mSrcFd, FIONREAD, &available))
                terminate(
                    errno,
                    "Unable to find amount of readable data in fd %d",
                    self->mSrcFd);

            if ( ! available)
            {
                debug(0, "tether drain input empty");
                break;
            }

            /* This splice(2) call will likely block if it is unable to
             * write all the data to the output file descriptor immediately.
             * Note that it cannot block on reading the input file descriptor
             * because that file descriptor is private to this process, the
             * amount of input available is known and is only read by this
             * thread. */

            ssize_t bytes = spliceFd(
                self->mSrcFd, self->mDstFd, available, SPLICE_F_MOVE);

            if ( ! bytes)
            {
                debug(0, "tether drain output closed");
                break;
            }

            if (-1 == bytes)
            {
                if (EPIPE == errno)
                {
                    debug(0, "tether drain output broken");
                    break;
                }

                if (EWOULDBLOCK != errno && EINTR != errno)
                    terminate(
                        errno,
                        "Unable to splice %d bytes from fd %d to fd %d",
                        available,
                        self->mSrcFd,
                        self->mDstFd);
            }
            else
            {
                debug(1,
                      "drained %zd bytes from fd %d to fd %d",
                      bytes, self->mSrcFd, self->mDstFd);
            }

            drained = false;

        } while (0);

        if (drained)
            self->mPollFds[TETHER_FD_CONTROL].events = 0;
    }
}

static void
polltetherdisconnected(void                        *self_,
                       struct PollFdTimerAction    *aPollFdTimer,
                       const struct EventClockTime *aPollTime)
{
    struct TetherPoll *self = self_;

    /* Once the tether drain timeout expires, disable the timer, and
     * force completion of the tether thread. */

    self->mPollFdTimerActions[TETHER_FD_TIMER_DISCONNECT].mPeriod =
        Duration(NanoSeconds(0));

    self->mPollFds[TETHER_FD_CONTROL].events = 0;
}

static bool
polltethercompletion(void                     *self_,
                     struct pollfd            *aPollFds_unused,
                     struct PollFdTimerAction *aPollFdTimer)
{
    struct TetherPoll *self = self_;

    return ! self->mPollFds[TETHER_FD_CONTROL].events;
}

static void *
tetherThreadMain_(void *self_)
{
    struct TetherThread *self = self_;

    {
        lockMutex(&self->mState.mMutex);
        self->mState.mValue = TETHER_THREAD_RUNNING;
        unlockMutexSignal(&self->mState.mMutex, &self->mState.mCond);
    }

    /* Do not open, or close files in this thread because it will race
     * the main thread forking the child process. When forking the
     * child process, it is important to control the file descriptors
     * inherited by the chlid. */

    int srcFd     = STDIN_FILENO;
    int dstFd     = STDOUT_FILENO;
    int controlFd = self->mControlPipe.mRdFile->mFd;

    /* The file descriptor for stdin is a pipe created by the watchdog
     * so it is known to be nonblocking. The file descriptor for stdout
     * is inherited, so it is likely blocking. */

    ensure(nonblockingFd(srcFd));

    /* The tether thread is configured to receive SIGALRM, but
     * these signals are not delivered until the thread is
     * flushed after the child process has terminated. */

    struct PushedProcessSigMask pushedSigMask;

    const int sigList[] = { SIGALRM, 0 };

    if (pushProcessSigMask(&pushedSigMask, ProcessSigMaskUnblock, sigList))
        terminate(
            errno,
            "Unable to push process signal mask");

    struct TetherPoll tetherpoll =
    {
        .mThread = self,
        .mSrcFd  = srcFd,
        .mDstFd  = dstFd,

        .mPollFds =
        {
            [TETHER_FD_CONTROL]= {.fd= controlFd,.events= POLL_INPUTEVENTS },
            [TETHER_FD_INPUT]  = {.fd= srcFd,    .events= POLL_INPUTEVENTS },
            [TETHER_FD_OUTPUT] = {.fd= dstFd,    .events= POLL_DISCONNECTEVENT},
        },

        .mPollFdActions =
        {
            [TETHER_FD_CONTROL] = { polltethercontrol, &tetherpoll },
            [TETHER_FD_INPUT]   = { polltetherdrain,   &tetherpoll },
            [TETHER_FD_OUTPUT]  = { polltetherdrain,   &tetherpoll },
        },

        .mPollFdTimerActions =
        {
            [TETHER_FD_TIMER_DISCONNECT] =
            {
                polltetherdisconnected, &tetherpoll
            },
        },
    };

    struct PollFd pollfd;
    if (createPollFd(
            &pollfd,
            tetherpoll.mPollFds,
            tetherpoll.mPollFdActions,
            sTetherFdNames, TETHER_FD_KINDS,
            tetherpoll.mPollFdTimerActions,
            sTetherFdTimerNames, TETHER_FD_TIMER_KINDS,
            polltethercompletion, &tetherpoll))
        terminate(
            errno,
            "Unable to initialise polling loop");

    if (runPollFdLoop(&pollfd))
        terminate(
            errno,
            "Unable to run polling loop");

    if (closePollFd(&pollfd))
        terminate(
            errno,
            "Unable to close polling loop");

    if (popProcessSigMask(&pushedSigMask))
        terminate(
            errno,
            "Unable to push process signal mask");

    /* Close the input file descriptor so that there is a chance
     * to propagte SIGPIPE to the child process. */

    if (dup2(self->mNullPipe->mRdFile->mFd, srcFd) != srcFd)
        terminate(
            errno,
            "Unable to dup fd %d to fd %d",
            self->mNullPipe->mRdFile->mFd,
            srcFd);

    /* Shut down the end of the control pipe controlled by this thread,
     * without closing the control pipe file descriptor itself. */

    if (dup2(self->mNullPipe->mRdFile->mFd, controlFd) != controlFd)
        terminate(errno, "Unable to shut down tether thread control");

    debug(0, "tether emptied");

    {
        lockMutex(&self->mState.mMutex);

        while (TETHER_THREAD_RUNNING == self->mState.mValue)
            waitCond(&self->mState.mCond, &self->mState.mMutex);

        unlockMutex(&self->mState.mMutex);
    }

    return 0;
}

static void
createTetherThread(struct TetherThread *self, struct Pipe *aNullPipe)
{
    if (createPipe(&self->mControlPipe, O_CLOEXEC | O_NONBLOCK))
        terminate(errno, "Unable to create tether control pipe");

    if (errno = pthread_mutex_init(&self->mActivity.mMutex, 0))
        terminate(errno, "Unable to create activity mutex");

    if (errno = pthread_mutex_init(&self->mState.mMutex, 0))
        terminate(errno, "Unable to create state mutex");

    if (errno = pthread_cond_init(&self->mState.mCond, 0))
        terminate(errno, "Unable to create state condition");

    self->mNullPipe        = aNullPipe;
    self->mActivity.mSince = eventclockTime();
    self->mState.mValue    = TETHER_THREAD_STOPPED;
    self->mFlushed         = false;

    {
        struct PushedProcessSigMask pushedSigMask;

        if (pushProcessSigMask(&pushedSigMask, ProcessSigMaskBlock, 0))
            terminate(errno, "Unable to push process signal mask");

        createThread(&self->mThread, 0, tetherThreadMain_, self);

        if (popProcessSigMask(&pushedSigMask))
            terminate(errno, "Unable to restore signal mask");
    }

    {
        lockMutex(&self->mState.mMutex);

        while (TETHER_THREAD_STOPPED == self->mState.mValue)
            waitCond(&self->mState.mCond, &self->mState.mMutex);

        unlockMutex(&self->mState.mMutex);
    }
}

static void
pingTetherThread(struct TetherThread *self)
{
    debug(0, "ping tether thread");

    if (errno = pthread_kill(self->mThread, SIGALRM))
        terminate(
            errno,
            "Unable to signal tether thread");
}

static void
flushTetherThread(struct TetherThread *self)
{
    debug(0, "flushing tether thread");

    if (watchProcessClock(0, Duration(NanoSeconds(0))))
        terminate(
            errno,
            "Unable to configure synchronisation clock");

    char buf[1] = { 0 };

    if (sizeof(buf) != writeFile(self->mControlPipe.mWrFile, buf, sizeof(buf)))
    {
        /* This code will race the tether thread which might finished
         * because it already has detected that the child process has
         * terminated and closed its file descriptors. */

        if (EPIPE != errno)
            terminate(
                errno,
                "Unable to flush tether thread");
    }

    self->mFlushed = true;
}

static void
closeTetherThread(struct TetherThread *self)
{
    ensure(self->mFlushed);

    /* Note that the tether thread might be blocked on splice(2). The
     * concurrent process clock ticks are enough to cause splice(2)
     * to periodically return EINTR, allowing the tether thread to
     * check its deadline. */

    debug(0, "synchronising tether thread");

    {
        lockMutex(&self->mState.mMutex);

        ensure(TETHER_THREAD_RUNNING == self->mState.mValue);
        self->mState.mValue = TETHER_THREAD_STOPPING;

        unlockMutexSignal(&self->mState.mMutex, &self->mState.mCond);
    }

    (void) joinThread(&self->mThread);

    if (unwatchProcessClock())
        terminate(
            errno,
            "Unable to reset synchronisation clock");

    if (errno = pthread_cond_destroy(&self->mState.mCond))
        terminate(errno, "Unable to destroy state condition");

    if (errno = pthread_mutex_destroy(&self->mState.mMutex))
        terminate(errno, "Unable to destroy state mutex");

    if (errno = pthread_mutex_destroy(&self->mActivity.mMutex))
        terminate(errno, "Unable to destroy activity mutex");

    if (closePipe(&self->mControlPipe))
        terminate(errno, "Unable to close tether control pipe");
}

/* -------------------------------------------------------------------------- */
/* Child Process Monitoring
 *
 * The child process must be monitored for activity, and also for
 * termination.
 */

struct ChildMonitor
{
    pid_t mChildPid;

    struct Pipe         mNullPipe;
    struct TetherThread mTetherThread;

    struct pollfd            mPollFds[POLL_FD_KINDS];
    struct PollFdAction      mPollFdActions[POLL_FD_KINDS];
    struct PollFdTimerAction mPollFdTimerActions[POLL_FD_TIMER_KINDS];
};

/* -------------------------------------------------------------------------- */
/* Child Termination
 *
 * The watchdog will receive SIGCHLD when the child process terminates,
 * though no direct indication will be received if the child process
 * performs an execv(2). The SIGCHLD signal will be delivered to the
 * event loop on a pipe, at which point the child process is known
 * to be dead. */

struct PollFdChild
{
    enum PollFdKind mKind;

    struct ChildMonitor *mMonitor;
    struct pollfd       *mPollFds;
};

static void
pollFdChild(void                        *self_,
            struct pollfd               *aPollFds_ununsed,
            const struct EventClockTime *aPollTime)
{
    struct PollFdChild *self = self_;

    ensure(POLL_FD_CHILD == self->mKind);

    /* There is a race here between receiving the indication that the
     * child process has terminated, the other watchdog actions
     * that might be taking place to actively monitor or terminate
     * the child process. In other words, those actions might be
     * attempting to manage a child process that is already dead,
     * or declare the child process errant when it has already exited.
     *
     * Actively test the race by occasionally delaying this activity
     * when in test mode. */

    if ( ! testSleep())
    {
        struct PollEventText pollEventText;
        debug(
            1,
            "detected child %s",
            createPollEventText(
                &pollEventText,
                self->mPollFds[POLL_FD_CHILD].revents));

        ensure(self->mPollFds[POLL_FD_CHILD].events);

        /* The child process has terminated, so there is no longer
         * any need to monitor for SIGCHLD. */

        self->mPollFds[POLL_FD_CHILD].fd     = self->mMonitor->mNullPipe.mRdFile->mFd;
        self->mPollFds[POLL_FD_CHILD].events = 0;

        /* Record when the child has terminated, but do not exit
         * the event loop until all the IO has been flushed. With the
         * child terminated, no further input can be produced so indicate
         * to the tether thread that it should start flushing data now. */

        flushTetherThread(&self->mMonitor->mTetherThread);

        struct PollFdTimerAction *terminationTimer =
            &self->mMonitor->mPollFdTimerActions[POLL_FD_TIMER_TERMINATION];

        struct PollFdTimerAction *umbilicalTimer =
            &self->mMonitor->mPollFdTimerActions[POLL_FD_TIMER_UMBILICAL];

        struct PollFdTimerAction *disconnectionTimer =
            &self->mMonitor->mPollFdTimerActions[POLL_FD_TIMER_DISCONNECTION];

        terminationTimer->mPeriod   = Duration(NanoSeconds(0));
        umbilicalTimer->mPeriod     = Duration(NanoSeconds(0));
        disconnectionTimer->mPeriod = Duration(NSECS(Seconds(1)));
    }
}

static void
pollFdTimerChild(void                        *self_,
                 struct PollFdTimerAction    *aPollFdTimerAction,
                 const struct EventClockTime *aPollTime)
{
    struct PollFdChild *self = self_;

    ensure(POLL_FD_CHILD == self->mKind);

    debug(0, "disconnecting tether thread");

    pingTetherThread(&self->mMonitor->mTetherThread);
}

/* -------------------------------------------------------------------------- */
/* Maintain Umbilical Connection to Child Process
 *
 * The umbilical connection to the child process allows the child to
 * monitor the watchdog so that the child can terminate if it detects
 * that the watchdog is no longer present. This is important in
 * scenarios where the supervisor init(8) kills the watchdog without
 * giving the watchdog a chance to clean up, or if the watchdog
 * fails catatrophically. */

struct PollFdUmbilical
{
    enum PollFdKind                 mKind;
    pid_t                           mChildPid;
    struct pollfd                  *mPollFds;
    struct PollFdTimerAction       *mUmbilicalTimer;
    const struct PollFdTimerAction *mDisconnectionTimer;
    struct UnixSocket              *mUmbilicalSocket;
    struct UnixSocket               mUmbilicalPeer_;
    struct UnixSocket              *mUmbilicalPeer;
};

static int
pollFdUmbilicalAccept_(struct UnixSocket       *aPeer,
                       const struct UnixSocket *aServer,
                       pid_t                    aChildPid)
{
    int rc = -1;

    struct UnixSocket *peersocket = 0;

    if (acceptUnixSocket(aPeer, aServer, O_NONBLOCK | O_CLOEXEC))
        goto Finally;

    peersocket = aPeer;

    /* Require that the remote peer be the process being monitored.
     * The connection will be dropped if the process uses execv() to
     * run another program, and requiring that it then be re-established
     * when the new program creates its own umbilical connection. */

    struct ucred cred;

    if (ownUnixSocketPeerCred(aPeer, &cred))
        goto Finally;

    debug(0, "umbilical connection from pid %jd", (intmax_t) cred.pid);

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
                struct pollfd               *aPollFds_unused,
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
        "detected umbilical %s",
        createPollEventText(
            &pollEventText,
            self->mPollFds[POLL_FD_UMBILICAL].revents));

    if (self->mPollFds[POLL_FD_UMBILICAL].revents & POLLIN)
    {
        if (closeUnixSocket(self->mUmbilicalPeer))
            terminate(
                errno,
                "Unable to close umbilical peer");
        self->mUmbilicalPeer = 0;

        self->mUmbilicalTimer->mPeriod = Duration(NanoSeconds(0));

        if (pollFdUmbilicalAccept_(&self->mUmbilicalPeer_,
                                   self->mUmbilicalSocket,
                                   self->mChildPid))
            terminate(
                errno,
                "Unable to accept connection from umbilical peer");

        self->mUmbilicalPeer = &self->mUmbilicalPeer_;

        if (self->mDisconnectionTimer->mPeriod.duration.ns)
            debug(1, "child already exited");
        else
        {
            debug(1, "activating umbilical timer");

            self->mUmbilicalTimer->mPeriod =
                Duration(NanoSeconds(NSECS(
                    Seconds(gOptions.mTimeout.mUmbilical_s)).ns / 2));

            lapTimeSkip(
                &self->mUmbilicalTimer->mSince,
                self->mUmbilicalTimer->mPeriod,
                aPollTime);
        }
    }
}

static int
closeFdUmbilical(struct PollFdUmbilical *self)
{
    return closeUnixSocket(self->mUmbilicalPeer);
}

static void
pollFdTimerUmbilical(void                        *self_,
                     struct PollFdTimerAction    *aPollFdTimerAction,
                     const struct EventClockTime *aPollTime)
{
    struct PollFdUmbilical *self = self_;

    ensure(POLL_FD_UMBILICAL == self->mKind);

    char buf = 0;

    while (1)
    {
        ssize_t wrlen = sendUnixSocket(self->mUmbilicalPeer, &buf, sizeof(buf));

        if (-1 != wrlen)
            debug(1, "wrote to umbilical result %zd", wrlen);
        else
        {
            switch (errno)
            {
            default:
                terminate(errno, "Unable to write to umbilical");

            case EINTR:
                continue;

            case EPIPE:
                debug(0,
                      "umbilical connection closed by pid %zd",
                      self->mChildPid);

                self->mUmbilicalTimer->mPeriod = Duration(NanoSeconds(0));
                break;

            case EWOULDBLOCK:
                break;
            }
        }

        break;
    }
}

/* -------------------------------------------------------------------------- */
/* Child Termination State Machine
 *
 * When it is necessary to terminate the child process, first request
 * that the child terminate by sending it SIGTERM, and if the child
 * does not terminate, resort to sending SIGKILL. */

struct ChildSignalPlan
{
    pid_t mPid;
    int   mSig;
};

struct PollFdTimerTermination
{
    enum PollFdTimerKind mKind;

    struct PollFdTimerAction       *mTerminationTimer;
    const struct PollFdTimerAction *mDisconnectionTimer;

    struct Duration               mPeriod;
    const struct ChildSignalPlan *mPlan;
};

static void
activateFdTimerTermination(struct PollFdTimerTermination *self,
                           const struct EventClockTime   *aPollTime)
{
    if (self->mDisconnectionTimer->mPeriod.duration.ns)
        debug(1, "child already exited");
    else if ( ! self->mTerminationTimer->mSince.eventclock.ns)
    {
        debug(1, "activating termination timer");

        self->mTerminationTimer->mPeriod = self->mPeriod;

        lapTimeSkip(&self->mTerminationTimer->mSince,
                    self->mTerminationTimer->mPeriod,
                    aPollTime);
    }
}

static void
pollFdTimerTermination(void                        *self_,
                       struct PollFdTimerAction    *aPollFdTimerAction,
                       const struct EventClockTime *aPollTime)
{
    struct PollFdTimerTermination *self = self_;

    pid_t pidNum = self->mPlan->mPid;
    int   sigNum = self->mPlan->mSig;

    if (self->mPlan[1].mPid)
        ++self->mPlan;

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

/* -------------------------------------------------------------------------- */
/* Watchdog Tether
 *
 * The main tether used by the watchdog to monitor the child process requires
 * the child process to maintain some activity on the tether to demonstrate
 * that the child is functioning correctly. Data transfer on the tether
 * occurs in a separate thread since it might block. The main thread
 * is non-blocking and waits for the tether to be closed. */

struct PollFdTether
{
    enum PollFdKind mKind;

    struct PollFdTimerAction       *mTetherTimer;
    struct pollfd                  *mPollFds;
    struct TetherThread            *mThread;
    struct PollFdTimerTermination  *mTermination;
    struct Pipe                    *mNullPipe;

    pid_t    mChildPid;
    unsigned mCycleCount;       /* Current number of cycles */
    unsigned mCycleLimit;       /* Cycles before triggering */
};

static void
disconnectPollFdTether(struct PollFdTether *self,
                       struct pollfd       *aPollFds)
{
    debug(0, "disconnect tether control");

    aPollFds[POLL_FD_TETHER].fd     = self->mNullPipe->mRdFile->mFd;
    aPollFds[POLL_FD_TETHER].events = 0;
}

static void
pollFdTether(void                        *self_,
             struct pollfd               *aPollFds_unused,
             const struct EventClockTime *aPollTime)
{
    struct PollFdTether *self = self_;

    ensure(POLL_FD_TETHER == self->mKind);

    /* The tether thread control pipe will be closed when the tether
     * is shut down between the child process and watchdog, */

    disconnectPollFdTether(self, self->mPollFds);
}

static void
pollFdTimerTether(void                        *self_,
                  struct PollFdTimerAction    *aPollFdTimerAction,
                  const struct EventClockTime *aPollTime)
{
    struct PollFdTether *self = self_;

    ensure(POLL_FD_TETHER == self->mKind);

    /* The tether timer is only active if there is a tether and it was
     * configured with a timeout. The timeout expires if there was
     * no activity on the tether with the consequence that the monitored
     * child will be terminated. */

    do
    {
        enum ProcessStatus childstatus = monitorProcess(self->mChildPid);

        if (ProcessStatusError == childstatus)
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
        else if (ProcessStatusTrapped == childstatus ||
                 ProcessStatusStopped == childstatus)
        {
            debug(0, "deferred timeout due to child status %c", childstatus);

            self->mCycleCount = 0;
            break;
        }
        else
        {
            /* Find when the tether was last active and use it to
             * determine if a timeout has actually occurred. If
             * there was recent activity, use the time of that
             * activity to reschedule the timer in order to align
             * the timeout with the activity. */

            struct EventClockTime since;
            {
                lockMutex(&self->mThread->mActivity.mMutex);
                since = self->mThread->mActivity.mSince;
                unlockMutex(&self->mThread->mActivity.mMutex);
            }

            if (aPollTime->eventclock.ns <
                since.eventclock.ns + aPollFdTimerAction->mPeriod.duration.ns)
            {
                lapTimeRestart(&aPollFdTimerAction->mSince, &since);
                self->mCycleCount = 0;
                break;
            }

            if (++self->mCycleCount < self->mCycleLimit)
                break;

            self->mCycleCount = self->mCycleLimit;
        }

        /* Once the timeout has expired, the timer can be cancelled because
         * there is no further need to run this state machine. */

        debug(0, "timeout after %ds", gOptions.mTimeout.mTether_s);

        aPollFdTimerAction->mPeriod = Duration(NanoSeconds(0));

        activateFdTimerTermination(self->mTermination, aPollTime);

    } while (0);
}

/* -------------------------------------------------------------------------- */
struct PollFdTimerOrphan
{
    enum PollFdTimerKind mKind;

    struct PollFdTimerTermination *mTermination;
};

void
pollFdTimerOrphan(void                        *self_,
                  struct PollFdTimerAction    *aPollFdTimerAction,
                  const struct EventClockTime *aPollTime)
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

        aPollFdTimerAction->mPeriod = Duration(NanoSeconds(0));

        activateFdTimerTermination(self->mTermination, aPollTime);
    }
}

/* -------------------------------------------------------------------------- */
static bool
pollfdcompletion(void                     *self_,
                 struct pollfd            *aPollFds_unused,
                 struct PollFdTimerAction *aPollFdTimer)
{
    struct PollFdTether *self = self_;

    ensure(POLL_FD_TETHER == self->mKind);

    return
        ! (self->mPollFds[POLL_FD_CHILD].events |
           self->mPollFds[POLL_FD_TETHER].events);
}

/* -------------------------------------------------------------------------- */
static void
monitorChild(struct ChildProcess *self)
{
    debug(0, "start monitoring child");

    struct ChildMonitor childmonitor =
    {
        .mChildPid = self->mPid,

        .mPollFdTimerActions =
        {
            [POLL_FD_TIMER_TETHER]        = { 0 },
            [POLL_FD_TIMER_UMBILICAL]     = { 0 },
            [POLL_FD_TIMER_ORPHAN]        = { 0 },
            [POLL_FD_TIMER_TERMINATION]   = { 0 },
            [POLL_FD_TIMER_DISCONNECTION] =
            {
                .mAction = pollFdTimerChild,
                .mSelf   = &childmonitor,
                .mSince  = EVENTCLOCKTIME_INIT,
                .mPeriod = Duration(NanoSeconds(0)),
            },
        },
    };

    struct PollFdTimerAction *pollfdtimeractions =
        childmonitor.mPollFdTimerActions;

    if (createPipe(&childmonitor.mNullPipe, O_CLOEXEC | O_NONBLOCK))
        terminate(
            errno,
            "Unable to create null pipe");

    /* Create a thread to use a blocking copy to transfer data from a
     * local pipe to stdout. This is primarily because SPLICE_F_NONBLOCK
     * cannot guarantee that the operation is non-blocking unless both
     * source and destination file descriptors are also themselves non-blocking.
     *
     * The child thread is used to perform a potentially blocking
     * transfer between an intermediate pipe and stdout, while
     * the main monitoring thread deals exclusively with non-blocking
     * file descriptors. */

    createTetherThread(&childmonitor.mTetherThread, &childmonitor.mNullPipe);

    struct PollFdChild pollfdchild =
    {
        .mKind    = POLL_FD_CHILD,
        .mMonitor = &childmonitor,
    };

    childmonitor.mPollFdTimerActions[POLL_FD_TIMER_DISCONNECTION].mSelf = &pollfdchild;

    struct PollFdUmbilical pollfdumbilical =
    {
        .mKind               = POLL_FD_UMBILICAL,
        .mChildPid           = self->mPid,
        .mDisconnectionTimer = &pollfdtimeractions[POLL_FD_TIMER_DISCONNECTION],
        .mUmbilicalTimer     = &pollfdtimeractions[POLL_FD_TIMER_UMBILICAL],
        .mUmbilicalSocket    = &self->mUmbilicalSocket,
        .mUmbilicalPeer      = 0,
    };

    pollfdumbilical.mUmbilicalTimer->mAction = pollFdTimerUmbilical;
    pollfdumbilical.mUmbilicalTimer->mSelf   = &pollfdumbilical;
    pollfdumbilical.mUmbilicalTimer->mSince  = EVENTCLOCKTIME_INIT;
    pollfdumbilical.mUmbilicalTimer->mPeriod = Duration(NanoSeconds(0));

    struct ChildSignalPlan sharedPgrpPlan[] =
    {
        { self->mPid, SIGTERM },
        { self->mPid, SIGKILL },
        { 0 }
    };

    struct ChildSignalPlan ownPgrpPlan[] =
    {
        {  self->mPid, SIGTERM },
        { -self->mPid, SIGTERM },
        { -self->mPid, SIGKILL },
        { 0 }
    };

    struct PollFdTimerTermination pollfdtimertermination =
    {
        .mKind = POLL_FD_TIMER_TERMINATION,

        .mTerminationTimer   = &pollfdtimeractions[POLL_FD_TIMER_TERMINATION],
        .mDisconnectionTimer = &pollfdtimeractions[POLL_FD_TIMER_DISCONNECTION],

        .mPeriod = Duration(NSECS(Seconds(gOptions.mTimeout.mSignal_s))),
        .mPlan   = gOptions.mSetPgid ? ownPgrpPlan : sharedPgrpPlan,
    };

    pollfdtimertermination.mTerminationTimer->mAction = pollFdTimerTermination;
    pollfdtimertermination.mTerminationTimer->mSelf   = &pollfdtimertermination;

    /* Divide the timeout into two cycles so that if the child process is
     * stopped, the first cycle will have a chance to detect it and
     * defer the timeout. */

    const unsigned timeoutCycles = 2;

    struct PollFdTether pollfdtether =
    {
        .mKind = POLL_FD_TETHER,

        .mTetherTimer = &pollfdtimeractions[POLL_FD_TIMER_TETHER],

        .mThread      = &childmonitor.mTetherThread,
        .mTermination = &pollfdtimertermination,
        .mNullPipe    = &childmonitor.mNullPipe,

        .mChildPid     = self->mPid,
        .mCycleCount   = 0,
        .mCycleLimit   = timeoutCycles,
    };

    /* Note that a zero value for gOptions.mTimeout.mTether_s will
     * disable the tether timeout in which case the watchdog will
     * supervise the child, but not impose any timing requirements
     * on activity on the tether. */

    pollfdtether.mTetherTimer->mAction = pollFdTimerTether;
    pollfdtether.mTetherTimer->mSelf   = &pollfdtether;
    pollfdtether.mTetherTimer->mSince  = EVENTCLOCKTIME_INIT;
    pollfdtether.mTetherTimer->mPeriod =
        Duration(NanoSeconds(
            NSECS(Seconds(
                gOptions.mTether
                ? gOptions.mTimeout.mTether_s : 0)).ns / timeoutCycles));

    /* If requested to be aware when the watchdog becomes an orphan,
     * check if init(8) is the parent of this process. If this is
     * detected, start sending signals to the child to encourage it
     * to exit. */

    struct PollFdTimerOrphan pollfdtimerorphan =
    {
        .mKind = POLL_FD_TIMER_ORPHAN,

        .mTermination = &pollfdtimertermination,
    };

    if (gOptions.mOrphaned)
    {
        pollfdtimeractions[POLL_FD_TIMER_ORPHAN].mAction = pollFdTimerOrphan;
        pollfdtimeractions[POLL_FD_TIMER_ORPHAN].mSelf   = &pollfdtimerorphan;
        pollfdtimeractions[POLL_FD_TIMER_ORPHAN].mPeriod =
            Duration(NSECS(Seconds(3)));
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

    struct pollfd *pollfds = childmonitor.mPollFds;

    pollfds[POLL_FD_CHILD] = (struct pollfd) {
            .fd     = self->mTermPipe.mRdFile->mFd,
            .events = POLL_DISCONNECTEVENT };
    pollfds[POLL_FD_UMBILICAL] = (struct pollfd) {
            .fd     = self->mUmbilicalSocket.mFile->mFd,
            .events = POLL_INPUTEVENTS };
    pollfds[POLL_FD_TETHER] = (struct pollfd) {
            .fd     = pollfdtether.mThread->mControlPipe.mWrFile->mFd,
            .events = POLL_DISCONNECTEVENT, };

    pollfdchild.mPollFds     = pollfds;
    pollfdumbilical.mPollFds = pollfds;
    pollfdtether.mPollFds    = pollfds;

    /* It is unfortunate that O_NONBLOCK is an attribute of the underlying
     * open file, rather than of each file descriptor. Since stdin and
     * stdout are typically inherited from the parent, setting O_NONBLOCK
     * would affect all file descriptors referring to the same open file,
     so this approach cannot be employed directly. */

    if ( ! gOptions.mTether)
        disconnectPollFdTether(&pollfdtether, pollfds);

    for (size_t ix = 0; NUMBEROF(pollfds) > ix; ++ix)
    {
        if ( ! ownFdNonBlocking(pollfds[ix].fd))
            terminate(
                0,
                "Expected %s fd %d to be non-blocking",
                sPollFdNames[ix],
                pollfds[ix].fd);
    }

    struct PollFdAction *pollfdactions = childmonitor.mPollFdActions;

    pollfdactions[POLL_FD_CHILD] = (struct PollFdAction) {
        pollFdChild,     &pollfdchild };

    pollfdactions[POLL_FD_UMBILICAL] = (struct PollFdAction) {
        pollFdUmbilical, &pollfdumbilical };

    pollfdactions[POLL_FD_TETHER] = (struct PollFdAction) {
        pollFdTether,    &pollfdtether };

    struct PollFd pollfd;
    if (createPollFd(
            &pollfd,
            pollfds, pollfdactions, sPollFdNames, POLL_FD_KINDS,
            pollfdtimeractions, sPollFdTimerNames, POLL_FD_TIMER_KINDS,
            pollfdcompletion, &pollfdtether))
        terminate(
            errno,
            "Unable to initialise polling loop");

    if (runPollFdLoop(&pollfd))
        terminate(
            errno,
            "Unable to run polling loop");

    if (closePollFd(&pollfd))
        terminate(
            errno,
            "Unable to close polling loop");

    if (closeFdUmbilical(&pollfdumbilical))
        terminate(
            errno,
            "Unable to close umbilical peer");

    closeTetherThread(&childmonitor.mTetherThread);

    if (closePipe(&childmonitor.mNullPipe))
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

                    monotonicSleep(Duration(NanoSeconds(resolution)));

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

    struct PushedProcessSigMask pushedSigMask;
    if (pushProcessSigMask(&pushedSigMask, ProcessSigMaskBlock, 0))
        terminate(
            errno,
            "Unable to push process signal mask");

    if (ignoreProcessSigPipe())
        terminate(
            errno,
            "Unable to ignore SIGPIPE");

    /* The instance of the StdFdFiller guarantees that any further file
     * descriptors that are opened will not be mistaken for stdin,
     * stdout or stderr. */

    struct StdFdFiller stdFdFiller;

    if (createStdFdFiller(&stdFdFiller))
        terminate(
            errno,
            "Unable to create stdin, stdout, stderr filler");

    struct ChildProcess childProcess;
    createChild(&childProcess);

    if (watchProcessChildren(0, reapChild, &childProcess))
        terminate(
            errno,
            "Unable to add watch on child process termination");

    if (watchProcessSignals(0, killChild, &childProcess))
        terminate(
            errno,
            "Unable to add watch on signals");

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
    if (createPipe(&syncPipe, 0))
        terminate(
            errno,
            "Unable to create sync pipe");

    if (forkChild(&childProcess, aCmd, &stdFdFiller, &syncPipe, &pushedSigMask))
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
            pid = childProcess.mPid; break;
        }

        pidFile = &pidFile_;

        announceChild(pid, pidFile, pidFileName);
    }

    if (popProcessSigMask(&pushedSigMask))
        terminate(
            errno,
            "Unable to restore process signal mask");

    /* The creation time of the child process is earlier than
     * the creation time of the pidfile. With the pidfile created,
     * and the signal delivery to the child activated, identify
     * and release the waiting child process. */

    if (gOptions.mIdentify)
        RACE
        ({
            if (-1 == dprintf(STDOUT_FILENO,
                              "%jd\n", (intmax_t) childProcess.mPid))
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

    if (STDIN_FILENO != dup2(
            childProcess.mTetherPipe->mRdFile->mFd, STDIN_FILENO))
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

    /* Now that the tether has been duplicated onto stdin and stdout
     * as required, it is important to close the tether to ensure that
     * the only possible references to the tether pipe remain in the
     * child process, if required, and stdin and stdout in this process. */

    closeChildTether(&childProcess);

    if (purgeProcessOrphanedFds())
        terminate(
            errno,
            "Unable to purge orphaned files");

    /* Monitor the running child until it has either completed of
     * its own accord, or terminated. Once the child has stopped
     * running, release the pid file if one was allocated. */

    monitorChild(&childProcess);

    if (unwatchProcessSignals())
        terminate(
            errno,
            "Unable to remove watch from signals");

    if (unwatchProcessChildren())
        terminate(
            errno,
            "Unable to remove watch on child process termination");

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

    debug(0, "reaping child pid %jd", (intmax_t) childProcess.mPid);

    pid_t childPid = childProcess.mPid;

    int status = closeChild(&childProcess);

    debug(0, "reaped child pid %jd status %d", (intmax_t) childPid, status);

    if (resetProcessSigPipe())
        terminate(
            errno,
            "Unable to reset SIGPIPE");

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
