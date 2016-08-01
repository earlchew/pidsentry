pidsentry
=========

Monitor child process with a watchdog and maintain a pid file

#### Background

When running services and daemons, commonly encountered scenarios are:
* To stop the service if it becomes blocked or unresponsive
* To track the pid of the service to find the matching process

The pidsentry is a watchdog that can be used to monitor activity on
on a specific file descriptor (usually stdout) to infer that the service has hung.
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
| ```pidsentry -s -p /var/run/server.pid -- /usr/local/bin/server``` | Run server with pid file and monitor stdout |
| ```pidsentry -s -p /var/run/server.pid -u -- 'PATH="/usr/local/bin:$PATH" && server'``` | Run server with pid file and but without monitoring |

##### Examples for Clients

| Example | Scenario |
| ------- |----------|
| ```pidsentry -c -- /var/run/server.pid 'kill $PIDSENTRY_PID'``` | Run shell command against process |
| ```pidsentry -c -- /var/run/server.pid /usr/bin/perl command.pl``` | Run program against process |

#### Functional Specification

* The pidsentry shall run a child process.
* The child process shall run in its own process group.
* When the child process exits, the pidsentry shall exit with the exit code of the child process.
* If the child process is terminated by signal number S, the pidsentry shall exit with exit code 128+S.
* When the pidsentry terminates, the pidsentry shall kill the process group of the child.
* If configured, the pidsentry shall monitor its parent and terminate if it becomes orphaned.
* If configured to maintain a pid file, the pidsentry shall create a pid file for the child process.
* A pid file shall uniquely identify a process, even if that process has terminated.
* The pidsentry shall optionally monitor stdout of the child process.
* If the pidsentry is monitoring stdout of the child process, and if the child process has terminated, the pidsentry shall use a timeout interval to drain stdout before terminating.
* If the pidsentry is monitoring stdout of the child process, and if the child process has stopped, the pidsentry wait for the child to continue before continuing to monitor stdout of the child process.
* If the pidsentry is monitoring stdout of the child process, and if stdout of the child process is silent for a timeout interval, the pidsentry shall kill the child process.
* If the pidsentry hangs, the pidsentry shall kill itself and all processes in the child process group.
* If the pidsentry receives any of SIGHUP, SIGINT, SIGQUIT and SIGTERM, the pidsentry shall propagate the signal to the child process.
* If the pidsentry receives SIGTSTP, the pidsentry shall stop the child process.
* If the pidsentry receives SIGCONT, the pidsentry shall continue the child process.
* If the child process terminates due to SIGQUIT, and if the child process dumped core, the pidsentry shall terminate with SIGQUIT.
* If the pid file identifies a child process that is currently running, the pidsentry shall allow a command to run and provide the environment variable PIDSENTRY_PID to identify child process.
* If the pid file identifies a child process that is currently running, the pidsentry shall not run another child process that uses the same pid file.

#### Implementation

![](https://github.com/earlchew/pidsentry/blob/master/pidsentry.png)

The Supervisor represents a parent program that starts the pidsentry. The Supervisor can start more than one PidSentry
Program, though care must be taken to ensure that instances do not compete over the same resources (eg the same pid file
in the file system).

If the Supervisor creates the PidSentry Program in its own process group, the PidSentry Program can create its own Sentry,
otherwise an intermediate Agent is created in its own process group, and the Sentry is held by the Agent.

The Sentry creates an Umbilical in a separate process group. The Sentry and Umbilical monitor each other, and each
will terminate both itself and the other if the connection between them times out. The Umbilical maintains
a defunct process anchor in the process group of the Sentry, and the process group of the Child, to ensure that
those pids remain valid until the Umbilical terminates.

The Sentry creates the Child Process in its own process group and monitors that process.

The Sentry optionally creates a Pid File that uniquely identifies the Child Process, and the address of the
PidServer interface.

The Umbilical provides a PidServer that can be used to ensure that the pid named in the Pid File remains valid.

The PidSentry Command uses the PidServer to ensure that the pid named in the Pid File is active and is not re-used
while the Command is running. When the Command terminates, the PidSentry Command terminates and releases its
reference to the PidServer.
