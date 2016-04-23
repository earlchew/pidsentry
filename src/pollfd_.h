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
#ifndef POLLFD_H
#define POLLFD_H

#include "timekeeping_.h"

#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pollfd;

/* -------------------------------------------------------------------------- */
struct PollFdAction
{
    int (*mAction)(void                        *self,
                   const struct EventClockTime *aPollTime);
};

struct PollFdTimerAction
{
    int                (*mAction)(void                        *self,
                                  const struct EventClockTime *aPollTime);
    struct Duration       mPeriod;
    struct EventClockTime mSince;
};

struct PollFd
{
    struct pollfd *mPoll;
    void          *mObserver;
    bool         (*mCompletionQuery)(void *aObserver);

    struct
    {
        struct PollFdAction *mActions;
        const char * const  *mNames;
        size_t               mSize;
    } mFdActions;

    struct
    {
        struct PollFdTimerAction *mActions;
        const char * const       *mNames;
        size_t                    mSize;
    } mTimerActions;
};

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

/* -------------------------------------------------------------------------- */
#define POLL_INPUTEVENTS       ((unsigned) (POLLHUP|POLLERR|POLLPRI|POLLIN))
#define POLL_OUTPUTEVENTS      ((unsigned) (POLLHUP|POLLERR|POLLOUT))
#define POLL_DISCONNECTEVENT   ((unsigned) (POLLHUP|POLLERR))

/* -------------------------------------------------------------------------- */
int
createPollFd(struct PollFd            *self,
             struct pollfd            *aPoll,
             struct PollFdAction      *aFdActions,
             const char * const       *aFdNames,
             size_t                    aNumFdActions,
             struct PollFdTimerAction *aTimerActions,
             const char * const       *aTimerNames,
             size_t                    aNumTimerActions,
             bool                      aCompletionQuery(void *aObserver),
             void                     *aObserver);

int
runPollFdLoop(struct PollFd *self);

void
closePollFd(struct PollFd *self);

/* -------------------------------------------------------------------------- */
const char *
createPollEventText(
    struct PollEventText *aPollEventText, unsigned aPollEventMask);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* POLLFD_H */
