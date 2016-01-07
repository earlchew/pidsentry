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
COMMON_CXXFLAGS    = $(COMMON_FLAGS) -std=c++0x \
		     -Wno-variadic-macros -Wno-long-long
COMMON_LDFLAGS     = -Wl,--gc-sections
TEST_LIBS          = libk9_.la libgoogletest.la -lrt
TEST_FLAGS         = -DUNITTEST -I ../googletest/include
TEST_CFLAGS        = $(TEST_FLAGS) $(COMMON_CFLAGS)
TEST_CXXFLAGS      = $(TEST_FLAGS) $(COMMON_CXXFLAGS)
TESTS              = $(check_PROGRAMS) $(check_SCRIPTS)

k9dir       = $(datadir)/k9
k9_PROGRAMS         = k9
check_SCRIPTS       = test.sh
check_PROGRAMS      = _splicetest
check_PROGRAMS     += _isblockingtest
check_PROGRAMS     += _timekeepingtest
check_PROGRAMS     += _envtest
check_PROGRAMS     += _unixsockettest
check_PROGRAMS     += _parsetest
noinst_PROGRAMS     = #bioscall-asm
noinst_SCRIPTS      = $(check_SCRIPTS)
noinst_LTLIBRARIES  = libgoogletest.la libk9_.la
lib_LTLIBRARIES     = libk9.la

k9_CFLAGS    = $(COMMON_CFLAGS)
k9_LDFLAGS   = $(COMMON_LDFLAGS) -Wl,-Map,k9.map -Wl,-cref
k9_LDADD     = libk9_.la libk9.la -lrt -ldl
k9_SOURCES   = _k9.c

libk9_la_CFLAGS       = $(COMMON_CFLAGS)
libk9_la_LIBADD       = -lrt -lpthread
libk9_la_SOURCES      = libk9.c k9main.c $(libk9__la_SOURCES)
libk9_la_DEPENDENCIES = libk9.exp
libk9_la_VERSION      = 0:0:0
libk9_la_LDFLAGS      =
libk9_la_LDFLAGS     += $(COMMON_LDFLAGS)
libk9_la_LDFLAGS     += -version-info $(libk9_la_VERSION) -release $(VERSION)
libk9_la_LDFLAGS     += -export-symbols libk9.exp
libk9_la_LDFLAGS     += -Wl,-Map,libk9.map -Wl,-cref

libk9.exp:	libk9.sym
	$(CPP) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) \
	    $(AM_CPPFLAGS) $(CPPFLAGS) -x c-header "$<" | tail -1 > "$@.new"
	mv -- "$@.new" "$@"

_splicetest_SOURCES = _splicetest.c

_timekeepingtest_SOURCES = _timekeepingtest.cc
_timekeepingtest_LDADD   = $(TEST_LIBS)

_envtest_SOURCES = _envtest.cc
_envtest_LDADD   = $(TEST_LIBS)

_parsetest_SOURCES = _parsetest.cc
_parsetest_LDADD   = $(TEST_LIBS)

_unixsockettest_SOURCES = _unixsockettest.cc
_unixsockettest_LDADD   = $(TEST_LIBS)

_isblockingtest_SOURCES = _isblockingtest.c
_isblockingtest_LDADD   = $(TEST_LIBS)

include libk9__la.am
$(call WILDCARD,libk9__la,libk9__la_SOURCES,[a-z]*_.[ch])
libk9__la_CFLAGS = $(COMMON_CFLAGS)

libgoogletest_la_SOURCES  = gtest-all.cc
libgoogletest_la_CPPFLAGS = -I ../googletest

clean-local::
	rm -f *.map
