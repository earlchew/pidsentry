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
#ifndef PID_H
#define PID_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
struct Tid
Tid_(pid_t aTid);

#define PRId_Tid "jd"
#define FMTd_Tid(Tid) ((intmax_t) (Tid).mTid)
struct Tid
{
#ifdef __cplusplus
    explicit Tid(pid_t aTid)
    { *this = Tid_(aTid); }
#endif

    pid_t mTid;
};

#ifndef __cplusplus
static inline struct Tid
Tid(pid_t aTid)
{
    return Tid_(aTid);
}
#endif

/* -------------------------------------------------------------------------- */
struct Pid
Pid_(pid_t aPid);

#define PRId_Pid "jd"
#define FMTd_Pid(Pid) ((intmax_t) (Pid).mPid)
struct Pid
{
#ifdef __cplusplus
    explicit Pid(pid_t aPid)
    { *this = Pid_(aPid); }
#endif

    pid_t mPid;
};

#ifndef __cplusplus
static inline struct Pid
Pid(pid_t aPid)
{
    return Pid_(aPid);
}
#endif

/* -------------------------------------------------------------------------- */
struct Pgid
Pgid_(pid_t aPgid);

#define PRId_Pgid "jd"
#define FMTd_Pgid(Pgid) ((intmax_t) (Pgid).mPgid)
struct Pgid
{
#ifdef __cplusplus
    explicit Pgid(pid_t aPgid)
    { *this = Pgid_(aPgid); }
#endif

    pid_t mPgid;
};

#ifndef __cplusplus
static inline struct Pgid
Pgid(pid_t aPgid)
{
    return Pgid_(aPgid);
}
#endif

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* PID_H */
