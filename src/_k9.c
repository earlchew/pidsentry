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
#include "socketpair.h"
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

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/poll.h>

/* TODO
 *
 * cmdRunCommand() is too big, break it up
 * EINTR everywhere, especially flock
 * SIGALARM for occasional EINTR
 * Stuck flock
 * cloneProcessLock -- and don't use /proc/self
 * struct ProcessLock using mPathName and mPathName_
 * struct PidFile using mPathName and mPathName_
 * Use splice()
 * Correctly close socket/pipe on read and write EOF
 * Check correct operation if child closes tether first, vs stdout close first
 * Shutdown pipes
 * Parent and child process groups
 */

/* -------------------------------------------------------------------------- */
static pid_t
runChild(
    char              **aCmd,
    struct StdFdFiller *aStdFdFiller,
    struct SocketPair  *aTetherPipe,
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

        if (closeSocketPairParent(aTetherPipe))
            terminate(
                errno,
                "Unable to close tether pipe parent");

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
            if (gOptions.mTether)
            {
                int tetherFd = *gOptions.mTether;

                if (0 > tetherFd)
                    tetherFd = aTetherPipe->mChildFile->mFd;

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

                if (tetherFd == aTetherPipe->mChildFile->mFd)
                    break;

                if (dup2(aTetherPipe->mChildFile->mFd, tetherFd) != tetherFd)
                    terminate(
                        errno,
                        "Unable to dup tether pipe fd %d to fd %d",
                        aTetherPipe->mChildFile->mFd,
                        tetherFd);
            }

            if (closeSocketPair(aTetherPipe))
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

    if (wait(&status) != aChildPid)
        terminate(
            errno,
            "Unable to reap child pid '%jd'",
            (intmax_t) aChildPid);

    return status;
}

/* -------------------------------------------------------------------------- */
static void
monitorChild(pid_t aChildPid, struct Pipe *aTermPipe, struct Pipe *aSigPipe)
{
    enum PollFdKind
    {
        POLL_FD_STDIN,
        POLL_FD_STDOUT,
        POLL_FD_CHILD,
        POLL_FD_SIGNAL,
        POLL_FD_COUNT
    };

    unsigned pollInputEvents  = POLLPRI | POLLRDHUP | POLLIN;
    unsigned pollOutputEvents = POLLOUT | POLLHUP   | POLLNVAL;

    struct pollfd pollfds[POLL_FD_COUNT] =
    {
        [POLL_FD_CHILD] = {
            .fd = aTermPipe->mRdFile->mFd, .events = pollInputEvents },
        [POLL_FD_SIGNAL] = {
            .fd = aSigPipe->mRdFile->mFd,  .events = pollInputEvents },
        [POLL_FD_STDOUT] = {
            .fd = STDOUT_FILENO,  .events = 0 },
        [POLL_FD_STDIN] = {
            .fd = STDIN_FILENO,   .events = ! gOptions.mTether
                                            ? 0 : pollInputEvents },
    };

    char buffer[8192];

    char *bufend = buffer;
    char *bufptr = buffer;

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

    do
    {
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

        for (unsigned ix = 0; NUMBEROF(pollfds) > ix; ++ix)
            pollfds[ix].revents &= pollfds[ix].events;

        if (pollfds[POLL_FD_STDIN].revents)
        {
            debug(1, "poll stdin 0x%x", pollfds[POLL_FD_STDIN].revents);

            ensure(pollfds[POLL_FD_STDIN].events);

            /* The poll(2) call should return positive non-zero if
             * any events are returned. This is a defensive measure
             * against buggy implementations since the next if-clause
             * depends on this being right. */

            rc = 1;

            ssize_t bytes;

            do
                bytes = read(STDIN_FILENO, buffer, sizeof(buffer));
            while (-1 == bytes && EINTR == errno);

            debug(1, "read stdin %zd", bytes);

            if (-1 == bytes)
            {
                if (ECONNRESET == errno)
                    bytes = 0;
                else
                    terminate(
                        errno,
                        "Unable to read from tether pipe");
            }

            /* If the child has closed its end of the tether, the watchdog
             * will read EOF on the tether. Continue running the event
             * loop until the child terminates. */

            if ( ! bytes)
            {
                pollfds[POLL_FD_STDOUT].events = 0;
                pollfds[POLL_FD_STDIN].events  = 0;
            }
            else
            {
                if (sizeof(buffer) < bytes)
                    terminate(
                        errno,
                        "Read returned value %zd which exceeds buffer size",
                        bytes);

                if ( ! gOptions.mQuiet)
                {
                    bufptr = buffer;
                    bufend = bufptr + bytes;

                    pollfds[POLL_FD_STDOUT].events = pollOutputEvents;
                    pollfds[POLL_FD_STDIN].events  = 0;
                }
            }
        }

        /* If a timeout is expected and a timeout occurred, and the
         * event loop was waiting for data from the child process,
         * then declare the child terminated. */

        if ( ! rc && -1 == timeout)
        {
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
            debug(1, "poll stdout 0x%x", pollfds[POLL_FD_STDOUT].revents);

            ensure(pollfds[POLL_FD_STDOUT].events);

            ssize_t bytes;

            do
                bytes = write(STDOUT_FILENO, bufptr, bufend - bufptr);
            while (-1 == bytes && EINTR == errno);

            debug(1, "wrote stdout %zd", bytes);

            if (-1 == bytes)
            {
                if (EWOULDBLOCK != errno)
                    terminate(
                        errno,
                        "Unable to write to stdout");
                bytes = 0;
            }

            /* Once all the data that was previously read has been
             * transferred, switch the event loop to waiting for
             * more input. */

            bufptr += bytes;

            if (bufptr == bufend)
            {
                pollfds[POLL_FD_STDOUT].events = 0;
                pollfds[POLL_FD_STDIN].events  = pollInputEvents;
            }
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
            debug(1, "poll signal 0x%x", pollfds[POLL_FD_SIGNAL].revents);

            ssize_t       len;
            unsigned char sigNum;

            do
                len = read(aSigPipe->mRdFile->mFd, &sigNum, 1);
            while (-1 == len && EINTR == errno);

            if (-1 == len)
                terminate(
                    errno,
                    "Unable to read signal from queue");

            if ( ! len)
                terminate(
                    0,
                    "Signal queue closed unexpectedly");

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

        /* Record when the child has terminated, but do not exit
         * the event loop until all the IO has been flushed. */

        if (pollfds[POLL_FD_CHILD].revents)
        {
            debug(1, "poll child 0x%x", pollfds[POLL_FD_CHILD].revents);

            ensure(pollfds[POLL_FD_CHILD].events);

            pollfds[POLL_FD_CHILD].events = 0;
            deadChild = true;
        }

    } while ( ! deadChild ||
                pollfds[POLL_FD_STDOUT].events ||
                pollfds[POLL_FD_STDIN].events);
}

/* -------------------------------------------------------------------------- */
static void
announceChild(pid_t aPid, struct PidFile *aPidFile, const char *aPidFileName)
{
    for (int zombie = -1; zombie; )
    {
        if (0 < zombie)
        {
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

        zombie = zombiePidFile(aPidFile);

        if (0 > zombie)
            terminate(
                errno,
                "Unable to obtain status of pid file '%s'", aPidFileName);

        debug(0, "discarding zombie pid file '%s'", aPidFileName);
    }

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

    struct SocketPair tetherPipe;
    if (createSocketPair(&tetherPipe))
        terminate(
            errno,
            "Unable to create tether pipe");

    struct Pipe termPipe;
    if (createPipe(&termPipe))
        terminate(
            errno,
            "Unable to create termination pipe");

    struct Pipe sigPipe;
    if (createPipe(&sigPipe))
        terminate(
            errno,
            "Unable to create signal pipe");

    if (watchProcessSignals(&sigPipe))
        terminate(
            errno,
            "Unable to add watch on signals");

    if (watchProcessChildren(&termPipe))
        terminate(
            errno,
            "Unable to add watch on child process termination");

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

    if (STDIN_FILENO != dup2(tetherPipe.mParentFile->mFd, STDIN_FILENO))
        terminate(
            errno,
            "Unable to dup tether pipe to stdin");

    if (closeSocketPair(&tetherPipe))
        warn(
            errno,
            "Unable to close tether pipe");

    if (gOptions.mTether)
    {
        /* Non-blocking IO on stdout is required so that the event loop
         * remains responsive, otherwise the event loop will likely block
         * when writing each buffer in its entirety. */

        if (nonblockingFd(STDOUT_FILENO))
        {
            if (EBADF == errno)
                gOptions.mQuiet = true;
            else
                terminate(
                    errno,
                    "Unable to enable non-blocking writes to stdout");
        }
    }

    if (cleanseFileDescriptors())
        terminate(
            errno,
            "Unable to cleanse file descriptors");

    /* Monitor the running child until it has either completed of
     * its own accord, or terminated. Once the child has stopped
     * running, release the pid file if one was allocated. */

    monitorChild(childPid, &termPipe, &sigPipe);

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
    if (initProcess(argv[0]))
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

    if (exitProcess())
        terminate(
            errno,
            "Unable to finalise process state");

    return exitCode.mStatus;
}

/* -------------------------------------------------------------------------- */
