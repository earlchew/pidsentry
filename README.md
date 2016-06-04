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

* The pidsentry shall run a child process.
* The child process shall run in its own process group.
* When the child process terminates, the pidsentry shall terminate and propagate the exit code of the child process.
* When the pidsentry terminates, the pidsentry shall kill the process group of the child.
* If configured to maintain a pid file, the pidsentry shall create a pid file for the child process.
* A pid file shall uniquely identify a process, even if that process has terminated.
* The pidsentry shall optionally monitor stdout of the child process.
* If the pidsentry is monitoring stdout of the child process, and if the child process has terminated, the pidsentry shall drain stdout before terminating.
* If the pidsentry is monitoring stdout of the child process, and if the child process has stopped, the pidsentry wait for the child to continue before continuing to monitor stdout of the child process.
* If the pidsentry is monitoring stdout of the child process, and if stdout of the child process is silent for a timeout interval, the pidsentry shall kill the child process.
* If the pidsentry hangs, the pidsentry shall kill itself and all processes in the child process group.
* If the pidsentry receives any of SIGHUP, SIGINT, SIGQUIT and SIGTERM, the pidsentry shall propagate the signal to the child process.
* If the pidsentry receives SIGTSTP, the pidsentry shall stop the child process.
* If the pidsentry receives SIGCONT, the pidsentry shall continue the child process.
* If the child process terminates due to SIGQUIT, and if the child process dumped core, the pidsentry shall terminate with SIGQUIT.
* If the pid file identifies a child process that is currently running, the pidsentry shall allow a command to run and provide the environment variable PIDSENTRY_CHILD_PID to identify child process.

#### Implementation

![](https://github.com/earlchew/pidsentry/blob/master/pidsentry.png)
