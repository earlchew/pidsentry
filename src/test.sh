#!/usr/bin/env bash

set -eu

# Use explicit call to /bin/echo to ensure that echo is run as a separate
# process from the shell. This is important because SIGPIPE will be
# delivered to the separate echo process, rather than to the shell.

PIDSENTRY_VALGRIND_ERROR=128

PIDSENTRY_TEST_PID=$(sh -c '/bin/echo $PPID')

unset PIDSENTRY_TEST_FAILED
trap 'PIDSENTRY_TEST_FAILED= ; trap - 15' 15

pidsentryexec()
{
    unset PIDSENTRY_TEST_ERROR
    exec libtool --mode=execute $VALGRIND ./pidsentry "$@"
}

pidsentry()
{
    if [ $# -eq 0 -o x"${1:-}" != x"exec" ] ; then
        ( pidsentryexec "$@" ) ||
        {
            set -- $?
            [ x"$1" != x"$PIDSENTRY_VALGRIND_ERROR" ] ||
                kill -15 "$PIDSENTRY_TEST_PID"
            return $1
        }
        return 0
    else
        shift
        pidsentryexec "$@"
    fi
}

waitwhile()
{
    while eval "$@" ; do
        sleep 1
    done
}

waituntil()
{
    until eval "$@" ; do
        sleep 1
    done
}

random()
{
    printf '%s' $(( 0x$(openssl rand -hex 4) ))
}

randomsleep()
{
    sleep $(( $(random) % $1 )).$(( $(random) % 1000 ))
}

liveprocess()
{
    ( STATE=$(ps -o 'state=' -p "$1") && [ x"$STATE" != xZ ] ) || return $?
    return 0
}

stoppedprocess()
{
    ( STATE=$(ps -o 'state=' -p "$1") && [ x"$STATE" == xT ] ) || return $?
    return 0
}

testCaseBegin()
{
    TESTCASE=$1
    set -- "$(date +'%Y-%m-%d %H:%M:%S')" "$TESTCASE"
    printf '\n%s : testCase - %s - BEGIN\n' "$@"
}

testCaseEnd()
{
    [ -z "${PIDSENTRY_TEST_FAILED++}" ] || testFail "$TESTCASE"
    set -- "$(date +'%Y-%m-%d %H:%M:%S')" "$TESTCASE"
    printf '\n%s : testCase - %s - END\n' "$@"
}

testTrace_()
{
    {
        printf '%s:' "$1"
        shift
        printf ' %s' "$@" | tr '\n' ' '
        printf '\n'
    } >&2
}

testTrace()
{
    testTrace_ TEST "$@"
}

testFail()
{
    testTrace_ FAIL "$@"
    printf '\ntestCase - %s - FAILED\n' "$TESTCASE"
    exit 1
}

testExit()
{
    while : ; do
        if [ x"$1" = x0 ] ; then
            shift
            "$@" && break
        else
            if ( shift ;
                 VALGRIND=${VALGRIND%--leak-check=yes} ;
                 "$@" ) ; then
                shift
            else
                [ $? -ne "$1" ] || break
                shift
            fi
        fi
        testFail "$@"
    done
}

testOutput()
{
    eval set -- '"$@"' \""$1"\"
    eval set -- '"$@"' \""$3"\"
    if ! [ x"'$4'" "$2" x"'$5'" ] ; then
       testTrace [ x"'$4'" "$2" x"'$5'" ]
       set -- "$1" "$2" "$3"
       testFail "$@"
    fi
}

testLostWatchdogs()
{
    ps -awwo user=,ppid=,pid=,pgid=,command= |
    {
        while read REPLY ; do
            [ -n "${REPLY##*pidsentry*}" ] || {
                /bin/echo "$REPLY" >&2
                exit 1
            }
        done
        exit 0
    }
}

runTest()
{
    :
}

runTests()
{
    testCaseBegin 'Usage'
    testExit 1 pidsentry -?
    testCaseEnd

    testCaseBegin 'Missing -p option value'
    testExit 1 pidsentry -s -p
    testCaseEnd

    testCaseBegin 'Valid -p option value'
    testExit 0 pidsentry -s -d -p $PIDFILE -- true
    [ ! -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Long --pidfile option'
    testExit 0 pidsentry -s --pidfile $PIDFILE -- true
    [ ! -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Missing command'
    testExit 1 pidsentry
    testCaseEnd

    testCaseBegin 'Simple command'
    REPLY=$(pidsentry -s -dd /bin/echo test)
    [ x"$REPLY" = x"test" ]
    testCaseEnd

    testCaseBegin 'Simple command in test mode'
    REPLY=$(pidsentry -s -dd --test=1 /bin/echo test)
    [ x"$REPLY" = x"test" ]
    testCaseEnd

    testCaseBegin 'Empty pid file'
    rm -f $PIDFILE
    : > $PIDFILE
    testExit 0 pidsentry -s --test=1 -p $PIDFILE -- true
    [ ! -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Invalid content in pid file'
    rm -f $PIDFILE
    dd if=/dev/zero bs=1K count=1 > $PIDFILE
    testExit 0 pidsentry -s --test=1 -p $PIDFILE -- true
    [ ! -f $PIDFILE ]
    testCaseEnd

    for SUPERVISOR in PARENT SENTRY ; do
        testCaseBegin "Dead process in pid file - $SUPERVISOR"
        rm -f $PIDFILE
        pidsentry -s -i -u -p $PIDFILE -- sh -c 'while : ; do sleep 1 ; done' | {
            read PARENT SENTRY UMBILICAL
            read CHILD
            eval kill -9 \$$SUPERVISOR
            kill -9 $CHILD

            eval waitwhile liveprocess '$'$SUPERVISOR
            waitwhile liveprocess $SENTRY
            waitwhile liveprocess $CHILD
        }
        ! [ -s $PIDFILE ] || REPLY=$(cksum $PIDFILE)
        # Ensure that it is not possible to run a command against the child
        ! pidsentry -c -- $PIDFILE true
        ! [ -s $PIDFILE ] || [ x"$REPLY" = x"$(cksum $PIDFILE)" ]
        # Ensure that it is possible to create a new child
        testExit 0 pidsentry -s --test=1 -d -p $PIDFILE -- true
        [ ! -f $PIDFILE ]
        testCaseEnd
    done

    testCaseBegin 'Existing process in pid file'
    rm -f $PIDFILE
    testOutput "OK" = '$(
        pidsentry -s -i -p $PIDFILE -u -- sh -c "
            while : ; do sleep 1 ; done" | {
                read PARENT SENTRY UMBILICAL
                read CHILD
                pidsentry -s -p $PIDFILE -- true || /bin/echo OK
                kill -9 $CHILD
                waitwhile liveprocess $CHILD
        }
    )'
    [ ! -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Aliased process in pid file'
    rm -f $PIDFILE
    sh -c '
      stat -c %y /proc/$$/.
      { /bin/echo $$
        UUID=$(cat /proc/sys/kernel/random/boot_id)
        TIME=$(awk "{ print \$22 }" "/proc/$$/stat")
        /bin/echo "$UUID:$TIME" ; } > '$PIDFILE.new'
      mv -f '$PIDFILE.new' '$PIDFILE'' &
    while [ ! -s $PIDFILE ] ; do
        sleep 1
    done
    cat $PIDFILE
    read PID < $PIDFILE
    TIMESTAMP=$(date -d @$(( $(stat -c %Y $PIDFILE) - 3600 )) +%Y%m%d%H%M)
    touch -t $TIMESTAMP $PIDFILE
    stat -c %y $PIDFILE
    pidsentry -s --test=1 -d -p $PIDFILE -- true
    wait
    [ ! -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Read non-existent pid file'
    rm -f $PIDFILE
    testExit 1 pidsentry -c --test=1 -- $PIDFILE true
    [ ! -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Read malformed pid file'
    rm -f $PIDFILE
    date > $PIDFILE
    testExit 1 pidsentry -c --test=1 -- $PIDFILE true
    [ -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Identify processes'
    for REPLY in $(
      exec sh -c '
        /bin/echo $$
        set --
        set -- "$@" '"$VALGRIND"'
        set -- "$@" ./pidsentry -s --test=1 -i -- sh -c '\''/bin/echo $$'\'
        exec libtool --mode=execute "$@"
      {
        read REALSENTRY
        read PARENT SENTRY UMBILICAL ; read CHILD
        read REALCHILD
        /bin/echo "$REALSENTRY/$SENTRY $REALCHILD/$CHILD"
      }) ; do

      [ x"${REPLY%% *}" = x"${REPLY#* }" ]
    done
    testCaseEnd

    testCaseBegin 'Test blocked signals in child'
    testOutput "0000000000000000" = '$(
        pidsentry -s -- grep SigBlk /proc/self/status |
        {
            read HEADING SIGNALS
            /bin/echo "$SIGNALS"
        }
    )'
    testCaseEnd

    testCaseBegin 'Test ignored signals in child'
    REPLY=$(
        grep SigIgn /proc/self/status |
        {
            read HEADING SIGNALS
            /bin/echo "$SIGNALS"
        }
    )
    testOutput "$REPLY" = '$(
        pidsentry -s -- grep SigIgn /proc/self/status |
        {
            read HEADING SIGNALS
            /bin/echo "$SIGNALS"
        }
    )'
    testCaseEnd

    testCaseBegin 'Environment propagation'
    testOutput '0' = '"$(
        pidsentry -s -- sh -c '\''date ; printenv'\'' |
            grep "^PIDSENTRY_" |
            wc -l)"'
    testCaseEnd

    testCaseBegin 'Exit code propagation'
    testExit 2 pidsentry -s --test=1 -- sh -c 'exit 2'
    testCaseEnd

    testCaseBegin 'Signal exit code propagation'
    testExit $((128 + 9)) pidsentry -s --test=1 -- sh -c '
        /bin/echo Killing $$ ; kill -9 $$'
    testCaseEnd

    testCaseBegin 'Child process group'
    pidsentry -s -i -- ps -o pid,pgid,cmd | {
        read PARENT SENTRY UMBILICAL
        read CHILD
        read HEADING
        SENTRYPGID=
        CHILDPGID=
        while read PID PGID CMD ; do
            /bin/echo "$SENTRY - $CHILD - $PID $PGID $CMD" >&2
            if [ x"$SENTRY" = x"$PID" ] ; then
                CHILDPGID=$PGID
            elif [ x"$CHILD" = x"$PID" ] ; then
                SENTRYPGID=$PGID
            fi
        done
        [ -n "$SENTRYPGID" ]
        [ -n "$CHILDPGID" ]
        [ x"$SENTRYPGID" != x"$CHILDPGID" ]
        [ x"$CHILD" != x"$CHILDPGID" ]
    }
    testCaseEnd

    testCaseBegin 'Umbilical process file descriptors'
    # i.   stdin
    # ii.  stdout
    # iii. stderr
    [ -n "$VALGRIND" ] || testOutput "4" = '$(
        pidsentry -s --test=3 -i -- sh -c "while : ; do sleep 1 ; done" |
        {
            read PARENT SENTRY UMBILICAL
            read CHILD
            for P in $SENTRY $UMBILICAL ; do
                while ! grep -q " [tT] " /proc/$P/stat ; do sleep 1 ; done
            done
            ls -l /proc/$UMBILICAL/fd |
                grep -v " -> /proc/$PARENT" |
                grep "[0-9]:[0-9]" |
                wc -l
            for P in $SENTRY $UMBILICAL ; do
               kill -CONT $P || { /bin/echo NOTOK ; exit 1 ; }
            done
            kill $CHILD || { /bin/echo NOTOK ; exit 1 ; }
        }
    )'
    testCaseEnd

    testCaseBegin 'Umbilical process file descriptors with pid server'
    # i.   stdin
    # ii.  stdout
    # iii. stderr
    # iv.  pid server socket
    # v.   pid server poll
    [ -n "$VALGRIND" ] || testOutput "6" = '$(
        pidsentry -s --test=3 -p $PIDFILE -i -- sh -c "
            while : ; do sleep 1 ; done" |
        {
            read PARENT SENTRY UMBILICAL
            read CHILD
            for P in $SENTRY $UMBILICAL ; do
                while ! grep -q " [tT] " /proc/$P/stat ; do sleep 1 ; done
            done
            ls -l /proc/$UMBILICAL/fd |
                grep -v " -> /proc/$PARENT" |
                grep "[0-9]:[0-9]" |
                wc -l
            for P in $SENTRY $UMBILICAL ; do
               kill -CONT $P || { /bin/echo NOTOK ; exit 1 ; }
            done
            kill $CHILD || { /bin/echo NOTOK ; exit 1 ; }
        }
    )'
    testCaseEnd

    testCaseBegin 'Watchdog process file descriptors'
    # i.   stdin
    # ii.  stdout
    # iii. stderr
    # iv.  Agent tether
    # v.   Umbilical tether
    [ -n "$VALGRIND" ] || testOutput "6" = '$(
        pidsentry -s --test=3 -i -- sh -c "while : ; do sleep 1 ; done" |
        {
            read PARENT SENTRY UMBILICAL
            read CHILD
            for P in $SENTRY $UMBILICAL ; do
                while ! grep -q " [tT] " /proc/$P/stat ; do sleep 1 ; done
            done
            ls -l /proc/$SENTRY/fd |
                grep -v " -> /proc/$PARENT" |
                grep "[0-9]:[0-9]" |
                wc -l
            for P in $SENTRY $UMBILICAL ; do
               kill -CONT $P || { /bin/echo NOTOK ; exit 1 ; }
            done
            kill $CHILD || { /bin/echo NOTOK ; exit 1 ; }
        }
    )'
    testCaseEnd

    testCaseBegin 'Untethered watchdog process file descriptors'
    # i.   stdin
    # ii.  stdout
    # iii. stderr
    # iv.  Agent tether
    # v.   Umbilical tether
    [ -n "$VALGRIND" ] || testOutput "6" = '$(
        pidsentry -s --test=3 -i -u -- sh -c "while : ; do sleep 1 ; done" |
        {
            read PARENT SENTRY UMBILICAL
            read CHILD
            for P in $SENTRY $UMBILICAL ; do
                while ! grep -q " [tT] " /proc/$P/stat ; do sleep 1 ; done
            done
            ls -l /proc/$SENTRY/fd |
                grep -v " -> /proc/$PARENT" |
                grep "[0-9]:[0-9]" |
                wc -l
            for P in $SENTRY $UMBILICAL ; do
               kill -CONT $P || { /bin/echo NOTOK ; exit 1 ; }
            done
            kill $CHILD || { /bin/echo NOTOK ; exit 1 ; }
        }
    )'
    testCaseEnd

    testCaseBegin 'Untethered child process file descriptors'
    # i.   stdin
    # ii.  stdout
    # iii. stderr
    testOutput '$(
      ls -l /proc/self/fd | grep "[0-9]:[0-9]" | wc -l)' = '$(
      pidsentry -s --test=1 -u -- ls -l /proc/self/fd |
          grep "[0-9]:[0-9]" | wc -l)'
    testCaseEnd

    testCaseBegin 'Command process file descriptors'
    # i.   stdin
    # ii.  stdout
    # iii. stderr
    [ -n "$VALGRIND" ] || testOutput '$(
        ls -l /proc/self/fd | grep "[0-9][0-9]" | wc -l)' = '$(
        pidsentry -s -i -u -p $PIDFILE -- sh -c "while : ; do sleep 1 ; done" |
        {
            read PARENT SENTRY UMBILICAL
            read CHILD
            pidsentry -c $PIDFILE ls -l /proc/self/fd |
                grep "[0-9][[0-9]" | wc -l
            kill $CHILD
        }
    )'
    testCaseEnd

    testCaseBegin 'Untethered child process with 8M data'
    testOutput 8192000 = '$(
      pidsentry -s --test=1 -u -- dd if=/dev/zero bs=8K count=1000 | wc -c)'
    testCaseEnd

    testCaseBegin 'Tether with new file descriptor'
    testOutput '$(( 1 + $(
      ls -l /proc/self/fd | grep "[0-9]:[0-9]" | wc -l) ))' = '$(
      pidsentry -s --test=1 -f - -- ls -l /proc/self/fd |
          grep "[0-9]:[0-9]" |
          wc -l)'
    testCaseEnd

    testCaseBegin 'Tether using stdout'
    testOutput '$(( 0 + $(
      ls -l /proc/self/fd | grep "[0-9]:[0-9]" | wc -l) ))' = '$(
      pidsentry -s --test=1 -- ls -l /proc/self/fd | grep "[0-9]:[0-9]" | wc -l)'
    testCaseEnd

    testCaseBegin 'Tether using named stdout'
    testOutput '$(( 0 + $(
      ls -l /proc/self/fd | grep "[0-9]:[0-9]" | wc -l) ))' = '$(
      pidsentry -s --test=1 -f 1 -- ls -l /proc/self/fd |
          grep "[0-9]:[0-9]" |
          wc -l)'
    testCaseEnd

    testCaseBegin 'Tether using stdout with 8M data'
    testOutput 8192000 = '$(
      pidsentry -s --test=1 -- dd if=/dev/zero bs=8K count=1000 | wc -c)'
    testCaseEnd

    testCaseBegin 'Tether quietly using stdout with 8M data'
    testOutput 0 = '$(
      pidsentry -s --test=1 -q -- dd if=/dev/zero bs=8K count=1000 | wc -c)'
    testCaseEnd

    testCaseBegin 'Tether named in environment'
    testOutput "TETHER=1" = '$(
      pidsentry -s --test=1 -n TETHER -- printenv | grep TETHER)'
    testCaseEnd

    testCaseBegin 'Tether named alone in argument'
    testOutput "1" = '$(
      pidsentry -s --test=1 -n @tether@ -- /bin/echo @tether@ | grep "1")'
    testCaseEnd

    testCaseBegin 'Tether named as suffix in argument'
    testOutput "x1" = '$(
      pidsentry -s --test=1 -n @tether@ -- /bin/echo x@tether@ | grep "1")'
    testCaseEnd

    testCaseBegin 'Tether named as prefix argument'
    testOutput "1x" = '$(
      pidsentry -s --test=1 -n @tether@ -- /bin/echo @tether@x | grep "1")'
    testCaseEnd

    testCaseBegin 'Tether named as infix argument'
    testOutput "x1x" = '$(
      pidsentry -s --test=1 -n @tether@ -- /bin/echo x@tether@x | grep "1")'
    testCaseEnd

    for SUPERVISOR in PARENT SENTRY ; do
        testCaseBegin "Early parent death - $SUPERVISOR"
        {
            set -- sh -cx 'while : pidsentry -s ; do sleep 1 ; done'
            pidsentry -s -i --test=1 -dd "$@"
        } | {
            read PARENT SENTRY UMBILICAL
            randomsleep 1
            eval kill -9 \$$SUPERVISOR
            eval waitwhile liveprocess \$$SUPERVISOR
            eval waitwhile liveprocess $SENTRY
            sleep 3
            testLostWatchdogs
        }
        testCaseEnd
    done

    testCaseBegin 'Early umbilical death'
    testLostWatchdogs
    pidsentry -s -i --test=1 -dd sh -cx 'while : pidsentry -s ; do sleep 1 ; done' | {
        read PARENT SENTRY UMBILICAL
        randomsleep 1
        kill -9 $UMBILICAL
        SLEPT=0
        while : ; do
            ! testLostWatchdogs || break
            sleep 1
            [ $(( ++SLEPT )) -lt 60 ] || exit 1
        done
    }
    testCaseEnd

    testCaseBegin 'Early child death'
    testLostWatchdogs
    pidsentry -s -i --test=1 -dd sh -cx 'while : pidsentry -s ; do sleep 1 ; done' | {
        read PARENT SENTRY UMBILICAL
        read CHILD
        randomsleep 1
        kill -9 $CHILD
        SLEPT=0
        while : ; do
            ! testLostWatchdogs || break
            sleep 1
            [ $(( ++SLEPT )) -lt 60 ] || exit 1
        done
    }
    testCaseEnd

    testCaseBegin 'Unexpected death of child'
    REPLY=$(
      exec 3>&1
      {
        if pidsentry -s --test=1 -d -i -- sh -c '
            while : ; do : ; done ; exit 0' 3>&- ; then
          /bin/echo 0 >&3
        else
          /bin/echo $? >&3
        fi
      } | {
        exec >&2
        read PARENT SENTRY UMBILICAL ; /bin/echo "Parent $SENTRY $UMBILICAL"
        read CHILD            ; /bin/echo "Child $CHILD"
        kill -9 -- "$CHILD"
      })
    [ x"$REPLY" = x$((128 + 9)) ]
    testCaseEnd

    testCaseBegin 'Stopped child'
    testOutput OK = '"$(
        pidsentry -s --test=1 -i -d -t 2,,2 -- sh -c '\''kill -STOP $$'\'' | {
            read PARENT SENTRY UMBILICAL
            read CHILD
            sleep 8
            kill -CONT $CHILD || { /bin/echo NOTOK ; exit 1 ; }
            /bin/echo OK
        })"'
    testCaseEnd

    for SUPERVISOR in PARENT SENTRY ; do
        testCaseBegin "Stopped parent - $SUPERVISOR"
        testOutput OK = '"$(pidsentry -s --test=1 -i -d -t 8,2 -- sleep 4 | {
            read PARENT SENTRY UMBILICAL
            read CHILD
            eval kill -STOP \$$SUPERVISOR
            waituntil stoppedprocess \$$SUPERVISOR
            sleep 8
            eval kill -CONT \$$SUPERVISOR || { /bin/echo NOTOK ; exit 1 ; }
            /bin/echo OK
        })"'
        testCaseEnd
    done

    for SUPERVISOR in PARENT SENTRY ; do
        testCaseBegin "Randomly stopped parent - $SUPERVISOR"
        testOutput 'OK' = '$(
            { pidsentry -s -i -dd sleep 3 && /bin/echo OK ; } | {
                read PARENT SENTRY UMBILICAL
                read CHILD
                randomsleep 3
                if ! eval kill -STOP \$$SUPERVISOR ; then
                    ! eval liveprocess \$$SUPERVISOR || {
                        /bin/echo NOTOK ; exit 1
                    }
                else
                    randomsleep 10
                    eval kill -CONT \$$SUPERVISOR || {
                        /bin/echo NOTOK ; exit 1
                    }
                fi
                read REPLY
                /bin/echo $REPLY
            }
        )'
        testCaseEnd
    done

    for SUPERVISOR in PARENT SENTRY ; do
        testCaseBegin "Randomly stopped process family - $SUPERVISOR"
        testOutput 'OK' = '$(
            { pidsentry -s -i -dd sleep 3 && /bin/echo OK ; } | {
                read PARENT SENTRY UMBILICAL
                read CHILD
                randomsleep 3
                if ! eval kill -TSTP \$$SUPERVISOR ; then
                    ! eval liveprocess \$$SUPERVISOR || {
                        /bin/echo NOTOK ; exit 1
                    }
                else
                    randomsleep 10
                    eval kill -CONT \$$SUPERVISOR || {
                        /bin/echo NOTOK ; exit 1
                    }
                fi
                read REPLY
                /bin/echo $REPLY
            }
        )'
        testCaseEnd
    done

    for SUPERVISOR in PARENT SENTRY ; do
        testCaseBegin "Broken umbilical - $SUPERVISOR"
        testOutput "OK" = '$(
            exec 3>&1
            pidsentry -s -dd -i -- sleep 9 | {
                read PARENT SENTRY UMBILICAL
                read CHILD
                sleep 3
                kill -0 $CHILD
                RC=$?
                eval kill -9 \$$SUPERVISOR
                eval waitwhile kill -0 \$$SUPERVISOR 2>&-
                sleep 3
                ! kill -0 $CHILD 2>&- || /bin/echo NOTOK
                [ x"$RC" != x"0" ] || /bin/echo OK
            }
        )'
        testCaseEnd
    done

    testCaseBegin 'Competing child processes'
    (
    set -x
    TASKS="1 2 3 4"
    trap '
        for REPLY in $TASKS ; do
            eval '\''[ -z ${'\''TASK_$REPLY'\''++} ]'\'' ||
                eval '\''kill -9 $'\''TASK_$REPLY
        done ' 0
    for REPLY in $TASKS ; do
        {
            TASK_PID=$BASHPID
            PS4='+ $TASK_PID:$BASHPID '
            for (( IX = 0; IX < 8; ++IX )) ; do

                # Try to run an instance of the child process using
                # a common pidfile. Exactly one child process should
                # run even though several compete to run.

                {
                    RC=0
                    set -- tail -f /dev/null
                    pidsentry -s -d -i --test=1 -p $PIDFILE -u -- "$@" ||
                    {
                        RC=$?
                        [ x"$RC" != x137 ] || RC=0
                    }
                    /bin/echo "$RC"
                    exit "$RC"
                } | {
                    trapexit()
                    {
                        [ -z "${CHILD++}" ] || kill -9 "$CHILD" || :
                        set -- 255
                        while read REPLY ; do
                            set -- "$REPLY"
                        done
                        /bin/echo "$1"
                        exit "$1"
                    }

                    trap 'trapexit $?' 0

                    { read PARENT SENTRY UMBILICAL && read CHILD ; } || {
                        /bin/echo "Skipping pidfile from $TASK_PID" >&2
                        /bin/echo X
                        /bin/echo 0
                        trap - 0
                        exit 0
                    }

                    : PARENT $PARENT SENTRY $SENTRY UMBILICAL $UMBILICAL
                    : CHILD $CHILD

                    /bin/echo $CHILD

                    # Verify that the pidfile contains a reference to
                    # the currently running instance of the child
                    # process.

                    randomsleep 1

                    pidsentry -c -- $PIDFILE 'echo $PIDSENTRY_CHILD_PID' | {

                        read CHILD_PID || {
                            /bin/echo "Inaccessible pidfile from $TASK_PID" >&2
                            exit 1
                        }
                        [ x"$CHILD" = x"$CHILD_PID" ] || {
                            /bin/echo "Mismatched pidfile from $TASK_PID" >&2
                            exit 1
                        }
                    }

                    exit 0
                } | {

                    read CHILD
                    : CHILD $CHILD

                    while : ; do
                        read TESTRC
                        : TESTRC $TESTRC
                        [ -z "${TESTRC##X*}" ] || break
                   done

                    # Wait until no references to the child process
                    # group remain. This should mean that the process
                    # tree for this program instance has been cleaned up.

                    [ x"$CHILD" = xX ] || {
                        while kill -0 -- -"$CHILD" 2>&- ; do
                            sleep 1
                        done
                    }

                    exit $TESTRC
                }
            done
        } &
        eval TASK_$REPLY='$!'
    done
    for REPLY in $TASKS ; do
        eval wait '$'TASK_$REPLY || {
            eval unset TASK_$REPLY
            exit 1
        }
        eval unset TASK_$REPLY
    done
    trap - 0
    )
    testCaseEnd

    for SUPERVISOR in PARENT SENTRY ; do
        testCaseBegin "Fast signal queueing - $SUPERVISOR"
        SIGNALS="1 2 3 15"
        for SIG in $SIGNALS ; do
            /bin/echo "Signal $SIG"
            ( ulimit -c 0
                pidsentry -s --test=1 -i -dd -- tail -f /dev/null ||
                { RC=$? ; /bin/echo . ; /bin/echo $RC ; exit 0 ; }
                RC=$? ; /bin/echo . ; /bin/echo $RC ) |
            {
                read PARENT SENTRY UMBILICAL
                while eval kill -"$SIG" "\$$SUPERVISOR" 2>&- ; do
                    date ; eval /bin/echo kill -$SIG \$$SUPERVISOR
                    eval liveprocess \$$SUPERVISOR || break
                    sleep 1
                done >&2
                read CHILD
                { [ x"$CHILD" != x. ] && liveprocess $CHILD ; } || /bin/echo OK
                while read RC ; do
                    [ x"$RC" == x. ] || break
                done
                /bin/echo "$RC"
            } | {
                set -x
                read REPLY
                [ x"$REPLY" = x"OK" ]
                read RC
                [ x"$RC" = x"$((128 + SIG))" ]
            }
        done
        testCaseEnd
    done

    for SUPERVISOR in PARENT SENTRY ; do
        testCaseBegin "Slow signal queueing - $SUPERVISOR"
        SIGNALS="1 2 3 15"
        for SIG in $SIGNALS ; do
            /bin/echo "Signal $SIG"
            ( ulimit -c 0
                pidsentry -s -i --test=1 -dd -- tail -f /dev/null ||
                { RC=$? ; /bin/echo . ; /bin/echo $RC ; exit 0 ; }
                RC=$? ; /bin/echo . ; /bin/echo $RC ) |
            {
                read PARENT SENTRY UMBILICAL
                sleep 1
                while eval kill -"$SIG" "\$$SUPERVISOR" 2>&- ; do
                    date ; eval /bin/echo kill -$SIG \$$SUPERVISOR
                    eval liveprocess \$$SUPERVISOR || break
                    sleep 1
                done >&2
                read CHILD
                { [ x"$CHILD" != x. ] && liveprocess $CHILD ; } || /bin/echo OK
                while read RC ; do
                    [ x"$RC" == x. ] || break
                done
                /bin/echo "$RC"
            } | {
                set -x
                read REPLY
                [ x"$REPLY" = x"OK" ]
                read RC
                [ x"$RC" = x"$((128 + SIG))" ]
            }
        done
        testCaseEnd
    done

    for SUPERVISOR in PARENT SENTRY ; do
        testCaseBegin "Fixed termination deadline - $SUPERVISOR"
        testOutput OK = '$(
            pidsentry -s --test=3 -i -dd -t 3,,4 -- sh -cx "
                trap : 15 6
                while : \$PPID ; do
                    STATE=\`ps -o state= -p \$PPID\` || break
                    [ x\$STATE != xZ ] || break
                    [ x\$STATE != xT ] || break
                    sleep 1
                done
                kill -CONT \$PPID
                /bin/echo READY
                while : ; do sleep 2 ; done" |
            {
                set -x
                # t+3s : Watchdog times out child and issues kill -ABRT
                # t+7s : Watchdog escalates and issues kill -KILL
                read PARENT SENTRY UMBILICAL
                : PARENT $PARENT SENTRY $SENTRY UMBILICAL $UMBILICAL
                read CHILD
                : CHILD $CHILD
                read READY # Signal handler established
                [ x"$READY" = xREADY ]
                while : ; do
                    liveprocess $UMBILICAL || break
                    ! stoppedprocess $UMBILICAL || break
                    sleep 1
                done
                kill -CONT $UMBILICAL
                while : ; do sleep 1 ; eval kill \$$SUPERVISOR 2>&- ; done &
                SLAVE=$!
                sleep 4 # Wait for watchdog to send first signal
                read -t 0 && /bin/echo FAIL1
                sleep 10 # Watchdog should have terminated child and exited
                read -t 0 && /bin/echo OK || /bin/echo FAIL2
                kill -9 $SLAVE 2>&-
            }
        )'
        testCaseEnd
    done

    testCaseBegin 'Test SIGPIPE propagates from child'
    testOutput "X-$((128 + 13))" = '$(
        exec 3>&1
        if pidsentry -s --test=1 -d -d -- sh -cx "
            while : ; do /bin/echo X || exit \$?; sleep 1 ; done " ; then
            /bin/echo "X-$?" >&3
        else
            /bin/echo "X-$?" >&3
        fi | read
    )'
    testCaseEnd

    testCaseBegin 'Test EPIPE propagates to child'
    testOutput "X-2" = '$(
        exec 3>&1
        if pidsentry -s -dd -- sh -c "
            sleep 1 ;
            while : ; do
                /bin/echo X || exit 2
            done"
        then
            /bin/echo "X-$?" >&3
        else
            /bin/echo "X-$?" >&3
        fi | dd of=/dev/null bs=1 count=$((1 + $(random) % 128))
    )'
    testCaseEnd

    testCaseBegin 'Test output is non-blocking even when pipe buffer is filled'
    testOutput AABB = '$(
        exec 3>&1
        {
            trap '\''[ -z "$TESTPID" ] || kill -- "$TESTPID"'\'' 15
            sh -c '\''/bin/echo $PPID'\''
            dd if=/dev/zero bs=$((64 * 1024)) count=1
            ( sleep 2
                dd if=/dev/zero bs=$((32 * 1024)) count=1 ) &
            pidsentry exec -s -dd -- sh -c "
               trap '\''/bin/echo -n AA >&3; exit 2'\'' 15
               sleep 2
               dd if=/dev/zero bs=$((64 * 1024)) count=1 &
               while : ; do sleep 1 ; done" &
            TESTPID=$!
            wait $TESTPID
        } | {
            read SUBSHELL

            # Wait a little time before making some space in the pipe. The
            # two pipe writers will compete to fill the space.
            sleep 3
            dd of=/dev/null bs=$((32 * 1024)) count=1

            # Provide some time to let the competitors fill the pipe, then
            # try to have the watchdoog propagate a signal to the child.

            sleep 2
            kill $SUBSHELL
            sleep 2
            /bin/echo -n BB
        }
    )'
    testCaseEnd

    testCaseBegin 'Timeout with data that must be flushed after 6s'
    REPLY=$(
        START=$(date +%s)
        pidsentry -s --test=1 -t 4 -- sh -c 'trap : 6 ; sleep 6'
        STOP=$(date +%s)
        /bin/echo $(( STOP - START))
    )
    [ "$REPLY" -ge 6 ]
    testCaseEnd
}

unset TEST_MODE_EXTENDED
if [ -n "${TEST_MODE++}" ] ; then
    case "${TEST_MODE:-}" in
    extended)
        TEST_MODE_EXTENDED=
        ;;
    *)
        printf "Unrecognised test mode: %s\n" "$TEST_MODE"
        exit 1
        ;;
    esac
fi
unset TEST_MODE

trap 'RC=$? ; [ x$RC != x0 ] || rm -rf scratch/* || : ; exit $RC' 0
mkdir -p scratch

PIDFILE=scratch/pidfile

testCaseBegin 'No running watchdogs'
testLostWatchdogs
testCaseEnd

for TEST in runTest runTests ; do
    VALGRIND=
    $TEST
    VALGRIND="valgrind"
    VALGRIND="$VALGRIND --error-exitcode=$PIDSENTRY_VALGRIND_ERROR"
    VALGRIND="$VALGRIND --vgdb=no"
    VALGRIND="$VALGRIND --leak-check=yes"
    VALGRIND="$VALGRIND --suppressions=pidsentry.supp"
    $TEST
done

testCaseBegin 'No lost watchdogs'
testLostWatchdogs
testCaseEnd

testCaseBegin 'Valgrind run over unit tests'
testOutput "" != "$(/bin/echo $VALGRIND)"
testCaseEnd

for TEST in $(/bin/echo $(ls -1 _* | grep -v -F .) ) ; do
    testCaseBegin "Valgrind - $TEST"
    make "$TEST"
    libtool --mode=execute $VALGRIND ./"$TEST"
    testCaseEnd
done

# The error handling test takes a very long time to run, so run a quick
# version of the test, unless TEST_MODE is configured.

testCaseBegin 'Error handling'
(
    [ -n "${TEST_MODE_EXTENDED++}" ] || export PIDSENTRY_TEST_ERROR=once
    ./errortest.sh || {
        printf "\n%s\n" "Check error log in scratch/*"
        exit 1
    }
)
testCaseEnd
