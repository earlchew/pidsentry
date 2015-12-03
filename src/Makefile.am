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

INCLUDES      =
LDADD         =

AM_CPPFLAGS   = -I.
AM_CFLAGS     = $(TEST_CFLAGS)
AM_CXXFLAGS   = $(TEST_CXXFLAGS)
AM_LDFLAGS    =

COMMON_FLAGS       = -D_GNU_SOURCE -Wall -Werror -pedantic -O2 -Wno-parentheses
COMMON_CFLAGS      = $(COMMON_FLAGS) -std=gnu99
COMMON_CXXFLAGS    = $(COMMON_FLAGS) -std=c++0x \
		     -Wno-variadic-macros -Wno-long-long
TEST_LIBS          = libk9.la libgoogletest.la -lrt
TEST_FLAGS         = -DUNITTEST -I ../googletest/include
TEST_CFLAGS        = $(TEST_FLAGS) $(COMMON_CFLAGS)
TEST_CXXFLAGS      = $(TEST_FLAGS) $(COMMON_CXXFLAGS)
TESTS              = $(check_PROGRAMS) $(check_SCRIPTS)

k9dir       = $(datadir)/k9
k9_PROGRAMS         = k9
check_SCRIPTS       = test.sh
check_PROGRAMS      = _splicetest _timekeepingtest
noinst_PROGRAMS     = #bioscall-asm
noinst_SCRIPTS      = $(check_SCRIPTS)
noinst_LTLIBRARIES  = libgoogletest.la libk9.la
#lib_LTLIBRARIES     = libk9.la

k9_CFLAGS    = $(COMMON_CFLAGS)
k9_LDFLAGS   = #-Wl,-Ttext=0x100000
k9_LDADD     = libk9.la -lrt
k9_SOURCES   = _k9.c

_splicetest_SOURCES = _splicetest.c

_timekeepingtest_SOURCES = _timekeepingtest.cc
_timekeepingtest_LDADD   = $(TEST_LIBS)

include libk9_la.am
$(call WILDCARD,libk9_la,libk9_la_SOURCES,[a-z]*.[ch])
libk9_la_CFLAGS = $(COMMON_CFLAGS)

libgoogletest_la_SOURCES  = gtest-all.cc
libgoogletest_la_CPPFLAGS = -I ../googletest
