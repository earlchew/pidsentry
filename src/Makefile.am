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

VERSION       = 1.0.0
INCLUDES      =
LDADD         =

AM_CPPFLAGS   = -I.
AM_CFLAGS     = $(TEST_CFLAGS)
AM_CXXFLAGS   = $(TEST_CXXFLAGS)
AM_LDFLAGS    =

COMMON_FLAGS       = -D_GNU_SOURCE -Wall -Werror -O2 -Wno-parentheses
COMMON_CFLAGS      = $(COMMON_FLAGS) -std=gnu99
COMMON_CFLAGS     += -fdata-sections -ffunction-sections
COMMON_CFLAGS     += -Wmissing-prototypes -Wmissing-declarations
COMMON_CXXFLAGS    = $(COMMON_FLAGS) -std=c++0x
COMMON_CXXFLAGS   += -Wno-variadic-macros -Wno-long-long
COMMON_LINKFLAGS   = -Wl,--gc-sections
TEST_LIBS          = libk9_.la libgoogletest.la -lrt
TEST_FLAGS         = -DUNITTEST -I ../googletest/include
TEST_CFLAGS        = $(TEST_FLAGS) $(COMMON_CFLAGS)
TEST_CXXFLAGS      = $(TEST_FLAGS) $(COMMON_CXXFLAGS)
TESTS              = $(check_PROGRAMS) $(check_SCRIPTS)

k9dir               = $(datadir)/k9
k9_PROGRAMS         = k9
check_SCRIPTS       = test.sh
check_PROGRAMS      = _splicetest
check_PROGRAMS     += _isblockingtest
check_PROGRAMS     += _systemtest
check_PROGRAMS     += _timekeepingtest
check_PROGRAMS     += _envtest
check_PROGRAMS     += _fdtest
check_PROGRAMS     += _unixsockettest
check_PROGRAMS     += _parsetest
check_PROGRAMS     += _processtest
noinst_PROGRAMS     =
noinst_SCRIPTS      = $(check_SCRIPTS)
noinst_LTLIBRARIES  = libgoogletest.la libk9_.la
lib_LTLIBRARIES     =

k9_CFLAGS    = $(COMMON_CFLAGS)
k9_LDFLAGS   = $(COMMON_LINKFLAGS) -Wl,-Map,k9.map -Wl,-cref
k9_LDADD     = libk9_.la -lrt -lpthread
k9_SOURCES   = _k9.c umbilical.c tether.c

_splicetest_SOURCES = _splicetest.c

_timekeepingtest_SOURCES = _timekeepingtest.cc
_timekeepingtest_LDADD   = $(TEST_LIBS)

_envtest_SOURCES = _envtest.cc
_envtest_LDADD   = $(TEST_LIBS)

_fdtest_SOURCES = _fdtest.cc
_fdtest_LDADD   = $(TEST_LIBS)

_parsetest_SOURCES = _parsetest.cc
_parsetest_LDADD   = $(TEST_LIBS)

_processtest_SOURCES = _processtest.cc
_processtest_LDADD   = $(TEST_LIBS)

_systemtest_SOURCES = _systemtest.cc
_systemtest_LDADD   = $(TEST_LIBS)

_unixsockettest_SOURCES = _unixsockettest.cc
_unixsockettest_LDADD   = $(TEST_LIBS)

_isblockingtest_SOURCES = _isblockingtest.c
_isblockingtest_LDADD   = $(TEST_LIBS)

include libk9__la.am
$(call WILDCARD,libk9__la,libk9__la_SOURCES,[a-z]*_.[ch])
libk9__la_CFLAGS = $(COMMON_CFLAGS)

libgoogletest_la_SOURCES  = gtest-all.cc
libgoogletest_la_CPPFLAGS = -I ../googletest

programs:	all
	$(MAKE) $(AM_MAKEFLAGS) $(check_PROGRAMS) $(check_SCRIPTS)

clean-local::
	rm -f *.map
	rm -f *.exp
	rm -rf scratch

delete-orphaned-includes:
	sh ./delete-orphaned-includes.sh $(MAKE) programs
