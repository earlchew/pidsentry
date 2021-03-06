# Copyright (c) 2013, Earl Chew
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the names of the authors of source code nor the names
#       of the contributors to the source code may be used to endorse or
#       promote products derived from this software without specific
#       prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL EARL CHEW BE LIABLE FOR ANY
# DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

$(eval include $(top_srcdir)/wildcard.mk)

include $(top_srcdir)/headercheck.mk

VERSION = 1.0.0
LDADD   =

AM_CPPFLAGS   = -I.
AM_CFLAGS     = $(TEST_CFLAGS)
AM_CXXFLAGS   = $(TEST_CXXFLAGS)
AM_LDFLAGS    =

OPT_FLAGS          = -O2
COMMON_FLAGS       = $(OPT_FLAGS)
COMMON_FLAGS      += -D_GNU_SOURCE -Wall -Werror
COMMON_FLAGS      += -Wno-parentheses -Wshadow
COMMON_CFLAGS      = $(COMMON_FLAGS) -std=gnu99
COMMON_CFLAGS     += -fdata-sections -ffunction-sections
COMMON_CFLAGS     += -Wmissing-prototypes -Wmissing-declarations
COMMON_CXXFLAGS    = $(COMMON_FLAGS) -std=gnu++0x
COMMON_CXXFLAGS   += -Wno-variadic-macros -Wno-long-long
COMMON_LINKFLAGS   = -Wl,--gc-sections
TEST_LIBS          = libpidsentry_.la -lert libgoogletest.la -ldl -lpthread -lrt
TEST_FLAGS         = -DUNITTEST -I ../googletest/include
TEST_CFLAGS        = $(TEST_FLAGS) $(COMMON_CFLAGS)
TEST_CXXFLAGS      = $(TEST_FLAGS) $(COMMON_CXXFLAGS)
TESTS              = $(check_PROGRAMS) $(check_SCRIPTS)

pidsentrydir        = $(bindir)
pidsentry_PROGRAMS  = pidsentry
check_SCRIPTS       = test.sh
check_PROGRAMS      = _pidsignaturetest
noinst_PROGRAMS     =
noinst_SCRIPTS      = $(check_SCRIPTS)
noinst_LTLIBRARIES  = libgoogletest.la libpidsentry_.la
lib_LTLIBRARIES     =

pidsentry_CFLAGS    = $(COMMON_CFLAGS)
pidsentry_LDFLAGS   = $(COMMON_LINKFLAGS) -Wl,-Map,pidsentry.map -Wl,-cref
pidsentry_LDADD     = libpidsentry_.la -lert -ldl -lrt -lpthread
pidsentry_SOURCES   = _pidsentry.c
pidsentry_SOURCES  += agent.c
pidsentry_SOURCES  += childprocess.c
pidsentry_SOURCES  += command.c
pidsentry_SOURCES  += parentprocess.c
pidsentry_SOURCES  += pidserver.c
pidsentry_SOURCES  += sentry.c
pidsentry_SOURCES  += shellcommand.c
pidsentry_SOURCES  += tether.c
pidsentry_SOURCES  += umbilical.c

_pidsignaturetest_SOURCES = _pidsignaturetest.cc
_pidsignaturetest_LDADD   = $(TEST_LIBS)

include libpidsentry__la.am
$(call WILDCARD,libpidsentry__la,libpidsentry__la_SOURCES,[a-z]*_.[ch])
libpidsentry__la_CFLAGS = $(COMMON_CFLAGS)

libgoogletest_la_SOURCES  = gtest-all.cc
libgoogletest_la_CPPFLAGS = -I ../googletest

programs:	all
	$(MAKE) $(AM_MAKEFLAGS) $(check_PROGRAMS) $(check_SCRIPTS)

clean-local::
	rm -f *.map
	rm -f *.exp
	rm -rf scratch

delete-orphaned-includes:
	unset MAKEFLAGS ; \
	unset MFLAGS ;    \
	unset MAKELEVEL ; \
	exec sh ./delete-orphaned-includes.sh $(MAKE) programs
