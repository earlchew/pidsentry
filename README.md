pidsentry
=========

Monitor child process with a watchdog and maintain a pid file

#### Background

When running services and daemons, commonly encountered scenarios are:
* To stop the service if it becomes blocked or unresponsive
* To track the pid of the service to find the matching process

The pidsentry is a watchdog that can be used to monitor activity on
on a specific file descriptor (usually stdout) to infer that the service is blocked.
If the monitored file descriptor becomes inactive, the watchdog will terminate
the child process.

The pidsentry can associate a pid file with the running child process. The
pid file uniquely identifies the child process, and can be used to ensure
that signals sent to the child process, or other communication with the child
process, will not be aliased to some other process which happens to reuse the same
pid, even if the host reboots.

#### Supported Platforms

* Linux 32 bit
* Linux 64 bit

#### Dependencies

* GNU Make
* GNU Automake
* GNU C
* GNU C++ for tests

#### Build

* Run `autogen.sh`
* Configure using `configure`
* Build binaries using `make`
* Run tests using `make check`

#### Usage

##### Examples for Servers

| Example | Scenario |
| ------- |----------|
| ```pidsentry -p /var/run/server.pid -- /usr/bin/server``` | Run server with pid file and monitor stdout |
| ```pidsentry -p /var/run/server.pid -u -- /usr/bin/server``` | Run server with pid file and but without monitoring |

##### Examples for Clients

| Example | Scenario |
| ------- |----------|
| ```pidsentry -p /var/run/server.pid -c -- 'kill $PIDSENTRY_CHILD_PID'``` | Run shell command against process |
| ```pidsentry -p /var/run/server.pid -c -- /usr/bin/perl command.pl``` | Run program against process |

#### Functional Specification

#### Implementation

![](https://github.com/earlchew/pidsentry/blob/master/pidsentry.png)
