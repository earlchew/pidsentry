pidsentry
=========

Monitor child process with a watchdog and maintain a pid file

#### Background

It is good practice to make C and C++ headers self-sufficient so
that headers do not have to be included in any particular order,
and changes to headers to not cause source files to break because
the required order of the headers has changed.

#### Requirements

* GNU Make
* GNU Automake

#### Installation

* Install `headercheck.mk` in the root of source tree.
* Add `include $(top_srcdir)/headercheck.mk` to each applicable Makefile.am.

The following is an example that shows a Makefile.am fragment demonstrating
the above steps.

```
% cat Makefile.am
include $(top_srcdir)/headercheck.mk

TESTS          = $(check_PROGRAMS)
check_PROGRAMS = _test
```
