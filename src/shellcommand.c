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

#include "shellcommand.h"

#include "ert/process.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* -------------------------------------------------------------------------- */
struct ShellCommand *
closeShellCommand(struct ShellCommand *self)
{
    if (self) {
        self->mArgList = ert_closeParseArgList(self->mArgList);

        free(self->mCmd);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
int
createShellCommand(struct ShellCommand *self,
                   const char * const  *aCmd)
{
    int rc = -1;

    self->mArgList = 0;
    self->mShell   = false;
    self->mCmd     = 0;

    ERROR_UNLESS(
        aCmd && aCmd[0] && aCmd[0][0],
        {
            errno = EINVAL;
        });

    ERROR_IF(
        ert_createParseArgListCopy(&self->mArgList_, aCmd));
    self->mArgList = &self->mArgList_;

    ensure(self->mArgList->mArgv && self->mArgList->mArgv[0]);

    if ( ! self->mArgList->mArgv[1])
    {
        for (const char *chPtr = self->mArgList->mArgv[0]; *chPtr; ++chPtr)
        {
            if (isspace((unsigned char) *chPtr))
            {
                self->mShell = true;
                break;
            }
        }
    }

    if (self->mShell)
        ERROR_UNLESS(
            self->mCmd = strdup(self->mArgList->mArgv[0]));
    else
    {
        const char *lastWord  = self->mArgList->mArgv[0];
        const char *nextWord  = lastWord;
        const char *lastSlash = strchr(nextWord, 0);

        while (*nextWord)
        {
            while (*nextWord && '/' == *nextWord)
                ++nextWord;

            if ( ! *nextWord)
                break;

            lastWord = nextWord;

            const char *nextSlash = lastWord;

            while (*nextSlash && '/' != *nextSlash)
                ++nextSlash;

            lastSlash = nextSlash;
            nextWord  = nextSlash;
        }

        size_t cmdLen = lastSlash - lastWord;

        ERROR_UNLESS(
            self->mCmd = malloc(cmdLen + 1));

        memcpy(self->mCmd, lastWord, cmdLen);
        self->mCmd[cmdLen] = 0;
    }

    rc = 0;

Finally:

    FINALLY
    ({
        if (rc)
            self = closeShellCommand(self);
    });

    return rc;
}

/* -------------------------------------------------------------------------- */
const char *
ownShellCommandName(const struct ShellCommand *self)
{
    return self->mCmd;
}

/* -------------------------------------------------------------------------- */
const char *
ownShellCommandText(const struct ShellCommand *self)
{
    return self->mArgList->mArgv[0];
}

/* -------------------------------------------------------------------------- */
void
execShellCommand(struct ShellCommand *self)
{
    if (self->mShell)
        ERROR_IF(
            (ert_execShell(self->mArgList->mArgv[0]), true));

    const char * const *argv = ert_ownParseArgListArgv(self->mArgList);

    ERROR_IF(
        (ert_execProcess(argv[0], argv), true));

Finally:

    FINALLY({});
}

/* -------------------------------------------------------------------------- */
