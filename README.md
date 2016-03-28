pidsentry
=========

Monitor child process with a watchdog and maintain a pid file

#### Background

When running services and daemons, common requirements are:
* To stop the service if it becomes blocked or unresponsive
* To track the pid of the service to find the correct process

The pidsentry provides a watchdog that can be used to monitor activity on
on a specific file descriptor (usually stdout) to infer that the service is blocked.
If the monitored file descriptor becomes inactive, the child process is terminated,
and the watchdog exits.

The pidsentry can associate a pid file with the running child process. The
pid file uniquely identifies the child process, and can be used to ensure
that signals sent to the child process, or other communication with the child
process will not be aliased to some other process which happens to reuse the same
pid, even if the host reboots.

#### Supported Platforms

* Linux 32 bit

#### Requirements

* GNU Make
* GNU Automake
* GNU C
* GNU C++ for tests

#### Build

* Run `autogen.sh`
* Configure using `configure`
* Build binaries using `make`
* Run tests using `make check`
