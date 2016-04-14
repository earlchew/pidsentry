#!/bin/sh

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
    ( STATE=$(ps -o 'state=' -p "$1") && [ x"$STATE" != xT ] ) || return $?
    return 0
}

testCaseBegin()
{
    TESTCASE=$1
    printf '\n%s : testCase - %s\n' "$(date +'%Y-%m-%d %H:%M:%S')" "$TESTCASE"
}

testCaseEnd()
{
    [ -z "${PIDSENTRY_TEST_FAILED++}" ] || testFail "$TESTCASE"
    printf '\n%s : testCase - %s\n' "$(date +'%Y-%m-%d %H:%M:%S')" "$TESTCASE"
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
            [ -n "${REPLY##*pidsentry*}" ] || exit 1
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
    testExit 1 pidsentry -? -- true
    testCaseEnd

    testCaseBegin 'Missing -p option value'
    testExit 1 pidsentry -p
    testCaseEnd

    testCaseBegin 'Valid -p option value'
    testExit 0 pidsentry -d -p $PIDFILE -- true
    [ ! -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Long --pidfile option'
    testExit 0 pidsentry --pidfile $PIDFILE -- true
    [ ! -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Missing command'
    testExit 1 pidsentry
    testCaseEnd

    testCaseBegin 'Simple command'
    REPLY=$(pidsentry -dd /bin/echo test)
    [ x"$REPLY" = x"test" ]
    testCaseEnd

    testCaseBegin 'Simple command in test mode'
    REPLY=$(pidsentry -dd --test=1 /bin/echo test)
    [ x"$REPLY" = x"test" ]
    testCaseEnd

    testCaseBegin 'Empty pid file'
    rm -f $PIDFILE
    : > $PIDFILE
    testExit 0 pidsentry --test=1 -p $PIDFILE -- true
    [ ! -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Invalid content in pid file'
    rm -f $PIDFILE
    dd if=/dev/zero bs=1K count=1 > $PIDFILE
    testExit 0 pidsentry --test=1 -p $PIDFILE -- true
    [ ! -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Dead process in pid file'
    rm -f $PIDFILE
    pidsentry -i -u -p $PIDFILE -- sh -c 'while : ; do sleep 1 ; done' | {
        read PARENT UMBILICAL
        read CHILD
        kill -9 $PARENT
        kill -9 $CHILD
    }
    [ -s $PIDFILE ]
    REPLY=$(cksum $PIDFILE)
    # Ensure that it is not possible to run a command against the child
    ! pidsentry -p $PIDFILE -c -- true
    [ x"$REPLY" = x"$(cksum $PIDFILE)" ]
    # Ensure that it is possible to create a new child
    testExit 0 pidsentry --test=1 -d -p $PIDFILE -- true
    [ ! -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Existing process in pid file'
    rm -f $PIDFILE
    testOutput "OK" = '$(
        pidsentry -i -p $PIDFILE -u -- sh -c "
            while : ; do sleep 1 ; done" | {
                read PARENT UMBILICAL
                read CHILD
                pidsentry -p $PIDFILE -- true || /bin/echo OK
             kill -9 $CHILD
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
    pidsentry --test=1 -d -p $PIDFILE -- true
    wait
    [ ! -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Read non-existent pid file'
    rm -f $PIDFILE
    testExit 1 pidsentry --test=1 -p $PIDFILE -c -- true
    [ ! -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Read malformed pid file'
    rm -f $PIDFILE
    date > $PIDFILE
    testExit 1 pidsentry --test=1 -p $PIDFILE -c -- true
    [ -f $PIDFILE ]
    testCaseEnd

    testCaseBegin 'Identify processes'
    for REPLY in $(
      exec sh -c '
        /bin/echo $$
        set --
        set -- "$@" '"$VALGRIND"'
        set -- "$@" ./pidsentry --test=1 -i -- sh -c '\''/bin/echo $$'\'
        exec libtool --mode=execute "$@"
      {
        read REALPARENT
        read PARENT UMBILICAL ; read CHILD
        read REALCHILD
        /bin/echo "$REALPARENT/$PARENT $REALCHILD/$CHILD"
      }) ; do

      [ x"${REPLY%% *}" = x"${REPLY#* }" ]
    done
    testCaseEnd

    testCaseBegin 'Test blocked signals in child'
    testOutput "0000000000000000" = '$(
        pidsentry -- grep SigBlk /proc/self/status |
        {
            read HEADING SIGNALS
            /bin/echo "$SIGNALS"
        }
    )'
    testCaseEnd

    testCaseBegin 'Test ignored signals in child'
    testOutput "0000000000000000" = '$(
        pidsentry -- grep SigIgn /proc/self/status |
        {
            read HEADING SIGNALS
            /bin/echo "$SIGNALS"
        }
    )'
    testCaseEnd

    testCaseBegin 'Environment propagation'
    testOutput '0' = '"$(
        pidsentry -- sh -c '\''date ; printenv'\'' |
            grep "^PIDSENTRY_" |
            wc -l)"'
    testCaseEnd

    testCaseBegin 'Exit code propagation'
    testExit 2 pidsentry --test=1 -- sh -c 'exit 2'
    testCaseEnd

    testCaseBegin 'Signal exit code propagation'
    testExit $((128 + 9)) pidsentry --test=1 -- sh -c '
        /bin/echo Killing $$ ; kill -9 $$'
    testCaseEnd

    testCaseBegin 'Child process group'
    pidsentry -i -- ps -o pid,pgid,cmd | {
        read PARENT UMBILICAL
        read CHILD
        read HEADING
        PARENTPGID=
        CHILDPGID=
        while read PID PGID CMD ; do
            /bin/echo "$PARENT - $CHILD - $PID $PGID $CMD" >&2
            if [ x"$PARENT" = x"$PID" ] ; then
                CHILDPGID=$PGID
            elif [ x"$CHILD" = x"$PID" ] ; then
                PARENTPGID=$PGID
            fi
        done
        [ -n "$PARENTPGID" ]
        [ -n "$CHILDPGID" ]
        [ x"$PARENTPGID" != x"$CHILDPGID" ]
        [ x"$CHILD" != x"$CHILDPGID" ]
    }
    testCaseEnd

    testCaseBegin 'Umbilical process file descriptors'
    # i.   stdin
    # ii.  stdout
    # iii. stderr
    [ -n "$VALGRIND" ] || testOutput "3" = '$(
        pidsentry --test=3 -i -- sh -c "while : ; do sleep 1 ; done" |
        {
            read PARENT UMBILICAL
            read CHILD
            for P in $PARENT $UMBILICAL ; do
                while ! grep -q " [tT] " /proc/$P/stat ; do sleep 1 ; done
            done
            ls -l /proc/$UMBILICAL/fd |
                grep -v " -> /proc/$PARENT" |
                grep "[0-9]-[0-9]" |
                wc -l
            for P in $PARENT $UMBILICAL ; do
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
    [ -n "$VALGRIND" ] || testOutput "5" = '$(
        pidsentry --test=3 -p $PIDFILE -i -- sh -c "
            while : ; do sleep 1 ; done" |
        {
            read PARENT UMBILICAL
            read CHILD
            for P in $PARENT $UMBILICAL ; do
                while ! grep -q " [tT] " /proc/$P/stat ; do sleep 1 ; done
            done
            ls -l /proc/$UMBILICAL/fd |
                grep -v " -> /proc/$PARENT" |
                grep "[0-9]-[0-9]" |
                wc -l
            for P in $PARENT $UMBILICAL ; do
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
    # iv.  Umbilical tether
    [ -n "$VALGRIND" ] || testOutput "4" = '$(
        pidsentry --test=3 -i -- sh -c "while : ; do sleep 1 ; done" |
        {
            read PARENT UMBILICAL
            read CHILD
            for P in $PARENT $UMBILICAL ; do
                while ! grep -q " [tT] " /proc/$P/stat ; do sleep 1 ; done
            done
            ls -l /proc/$PARENT/fd |
                grep -v " -> /proc/$PARENT" |
                grep "[0-9]-[0-9]" |
                wc -l
            for P in $PARENT $UMBILICAL ; do
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
    # iv.  Umbilical tether
    [ -n "$VALGRIND" ] || testOutput "4" = '$(
        pidsentry --test=3 -i -u -- sh -c "while : ; do sleep 1 ; done" |
        {
            read PARENT UMBILICAL
            read CHILD
            for P in $PARENT $UMBILICAL ; do
                while ! grep -q " [tT] " /proc/$P/stat ; do sleep 1 ; done
            done
            ls -l /proc/$PARENT/fd |
                grep -v " -> /proc/$PARENT" |
                grep "[0-9]-[0-9]" |
                wc -l
            for P in $PARENT $UMBILICAL ; do
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
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)' = '$(
      pidsentry --test=1 -u -- ls -l /proc/self/fd |
          grep "[0-9]-[0-9]" | wc -l)'
    testCaseEnd

    testCaseBegin 'Command process file descriptors'
    # i.   stdin
    # ii.  stdout
    # iii. stderr
    [ -n "$VALGRIND" ] || testOutput '$(
        ls -l /proc/self/fd | grep "[0-9][0-9]" | wc -l)' = '$(
        pidsentry -i -u -p $PIDFILE -- sh -c "while : ; do sleep 1 ; done" |
        {
            read PARENT UMBILICAL
            read CHILD
            pidsentry -p $PIDFILE -c ls -l /proc/self/fd |
                grep "[0-9][[0-9]" | wc -l
            kill $CHILD
        }
    )'
    testCaseEnd

    testCaseBegin 'Untethered child process with 8M data'
    testOutput 8192000 = '$(
      pidsentry --test=1 -u -- dd if=/dev/zero bs=8K count=1000 | wc -c)'
    testCaseEnd

    testCaseBegin 'Tether with new file descriptor'
    testOutput '$(( 1 + $(
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l) ))' = '$(
      pidsentry --test=1 -f - -- ls -l /proc/self/fd |
          grep "[0-9]-[0-9]" |
          wc -l)'
    testCaseEnd

    testCaseBegin 'Tether using stdout'
    testOutput '$(( 0 + $(
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l) ))' = '$(
      pidsentry --test=1 -- ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)'
    testCaseEnd

    testCaseBegin 'Tether using named stdout'
    testOutput '$(( 0 + $(
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l) ))' = '$(
      pidsentry --test=1 -f 1 -- ls -l /proc/self/fd |
          grep "[0-9]-[0-9]" |
          wc -l)'
    testCaseEnd

    testCaseBegin 'Tether using stdout with 8M data'
    testOutput 8192000 = '$(
      pidsentry --test=1 -- dd if=/dev/zero bs=8K count=1000 | wc -c)'
    testCaseEnd

    testCaseBegin 'Tether quietly using stdout with 8M data'
    testOutput 0 = '$(
      pidsentry --test=1 -q -- dd if=/dev/zero bs=8K count=1000 | wc -c)'
    testCaseEnd

    testCaseBegin 'Tether named in environment'
    testOutput "TETHER=1" = '$(
      pidsentry --test=1 -n TETHER -- printenv | grep TETHER)'
    testCaseEnd

    testCaseBegin 'Tether named alone in argument'
    testOutput "1" = '$(
      pidsentry --test=1 -n @tether@ -- /bin/echo @tether@ | grep "1")'
    testCaseEnd

    testCaseBegin 'Tether named as suffix in argument'
    testOutput "x1" = '$(
      pidsentry --test=1 -n @tether@ -- /bin/echo x@tether@ | grep "1")'
    testCaseEnd

    testCaseBegin 'Tether named as prefix argument'
    testOutput "1x" = '$(
      pidsentry --test=1 -n @tether@ -- /bin/echo @tether@x | grep "1")'
    testCaseEnd

    testCaseBegin 'Tether named as infix argument'
    testOutput "x1x" = '$(
      pidsentry --test=1 -n @tether@ -- /bin/echo x@tether@x | grep "1")'
    testCaseEnd

    testCaseBegin 'Early parent death'
    pidsentry -i --test=1 -dd sh -cx 'while : pidsentry ; do sleep 1 ; done' | {
        read PARENT UMBILICAL
        randomsleep 1
        kill -9 $PARENT
        sleep 3
        testLostWatchdogs
    }
    testCaseEnd

    testCaseBegin 'Early umbilical death'
    testLostWatchdogs
    pidsentry -i --test=1 -dd sh -cx 'while : pidsentry ; do sleep 1 ; done' | {
        read PARENT UMBILICAL
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
    pidsentry -i --test=1 -dd sh -cx 'while : pidsentry ; do sleep 1 ; done' | {
        read PARENT UMBILICAL
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
        if pidsentry --test=1 -d -i -- sh -c '
            while : ; do : ; done ; exit 0' 3>&- ; then
          /bin/echo 0 >&3
        else
          /bin/echo $? >&3
        fi
      } | {
        exec >&2
        read PARENT UMBILICAL ; /bin/echo "Parent $PARENT $UMBILICAL"
        read CHILD            ; /bin/echo "Child $CHILD"
        kill -9 -- "$CHILD"
      })
    [ x"$REPLY" = x$((128 + 9)) ]
    testCaseEnd

    testCaseBegin 'Stopped child'
    testOutput OK = '"$(
        pidsentry --test=1 -i -d -t 2,,2 -- sh -c '\''kill -STOP $$'\'' | {
            read PARENT UMBILICAL
            read CHILD
            sleep 8
            kill -CONT $CHILD || { /bin/echo NOTOK ; exit 1 ; }
            /bin/echo OK
        })"'
    testCaseEnd

    testCaseBegin 'Stopped parent'
    testOutput OK = '"$(pidsentry --test=1 -i -d -t 8,2 -- sleep 4 | {
        read PARENT UMBILICAL
        read CHILD
        kill -STOP $PARENT
        sleep 8
        kill -CONT $PARENT || { /bin/echo NOTOK ; exit 1 ; }
        /bin/echo OK
    })"'
    testCaseEnd

    testCaseBegin 'Randomly stopped parent'
    testOutput 'OK' = '$(
        { pidsentry -i -dd sleep 3 && /bin/echo OK ; } | {
            read PARENT UMBILICAL
            read CHILD
            randomsleep 3
            if ! kill -STOP $PARENT ; then
                ! liveprocess $PARENT || { /bin/echo NOTOK ; exit 1 ; }
            else
                randomsleep 10
                kill -CONT $PARENT || { /bin/echo NOTOK ; exit 1 ; }
            fi
            read REPLY
            /bin/echo $REPLY
        }
    )'
    testCaseEnd

    testCaseBegin 'Randomly stopped process family'
    testOutput 'OK' = '$(
        { pidsentry -i -dd sleep 3 && /bin/echo OK ; } | {
            read PARENT UMBILICAL
            read CHILD
            randomsleep 3
            if ! kill -TSTP $PARENT ; then
                ! liveprocess $PARENT || { /bin/echo NOTOK ; exit 1 ; }
            else
                randomsleep 10
                kill -CONT $PARENT || { /bin/echo NOTOK ; exit 1 ; }
            fi
            read REPLY
            /bin/echo $REPLY
        }
    )'
    testCaseEnd

    testCaseBegin 'Broken umbilical'
    testOutput "OK" = '$(
        exec 3>&1
        pidsentry -dd -i -- sleep 9 | {
            read PARENT UMBILICAL
            read CHILD
            sleep 3
            kill -0 $CHILD
            RC=$?
            kill -9 $PARENT
            sleep 3
            ! kill -0 $CHILD 2>/dev/null || /bin/echo NOTOK
            [ x"$RC" != x"0" ] || /bin/echo OK
        }
    )'
    testCaseEnd

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
                    pidsentry -d -i --test=1 -p $PIDFILE -u -- "$@" ||
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

                    { read PARENT UMBILICAL && read CHILD ; } || {
                        /bin/echo "Skipping pidfile from $TASK_PID" >&2
                        /bin/echo X
                        /bin/echo 0
                        trap - 0
                        exit 0
                    }

                    : PARENT $PARENT UMBILICAL $UMBILICAL
                    : CHILD $CHILD

                    /bin/echo $CHILD

                    # Verify that the pidfile contains a reference to
                    # the currently running instance of the child
                    # process.

                    randomsleep 1

                    pidsentry -p $PIDFILE -c -- sh -c '
                        /bin/echo $PIDSENTRY_CHILD_PID' | {

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

    testCaseBegin 'Fast signal queueing'
    SIGNALS="1 2 3 15"
    for SIG in $SIGNALS ; do
      ( ulimit -c 0
        pidsentry --test=1 -i -dd -- tail -f /dev/null ||
            { /bin/echo $? ; exit 0 ; }
        /bin/echo $? ) |
      {
         read PARENT UMBILICAL
         while kill -"$SIG" "$PARENT" 2>&- ; do
             date ; /bin/echo kill -"$SIG" "$PARENT"
             liveprocess $PARENT || break
             sleep 1
         done >&2
         read CHILD
         liveprocess $CHILD || /bin/echo OK
         read RC
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

    testCaseBegin 'Slow signal queueing'
    SIGNALS="1 2 3 15"
    for SIG in $SIGNALS ; do
      ( ulimit -c 0
        pidsentry -i --test=1 -dd -- tail -f /dev/null ||
            { /bin/echo $? ; exit 0 ; }
        /bin/echo $? ) |
      {
         read PARENT UMBILICAL
         sleep 1
         while kill -"$SIG" "$PARENT" 2>&- ; do
             date ; /bin/echo kill -"$SIG" "$PARENT"
             liveprocess $PARENT || break
             sleep 1
         done >&2
         read CHILD
         liveprocess $CHILD || /bin/echo OK
         read RC
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

    testCaseBegin 'Fixed termination deadline'
    testOutput OK = '$(
        pidsentry --test=3 -i -dd -t 3,,4 -- sh -cx "
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
            read PARENT UMBILICAL
            : PARENT $PARENT UMBILICAL $UMBILICAL
            read CHILD
            : CHILD $CHILD
            read READY # Signal handler established
            [ x"$READY" = xREADY ]
            while : ; do
                liveprocess $UMBILICAL || break
                stoppedprocess $UMBILICAL || break
                sleep 1
            done
            kill -CONT $UMBILICAL
            while : ; do sleep 1 ; kill $PARENT 2>&- ; done &
            SLAVE=$!
            sleep 4 # Wait for watchdog to send first signal
            read -t 0 && /bin/echo FAIL1
            sleep 10 # Watchdog should have terminated child and exited
            read -t 0 && /bin/echo OK || /bin/echo FAIL2
            kill -9 $SLAVE 2>&-
        }
    )'
    testCaseEnd

    testCaseBegin 'Test SIGPIPE propagates from child'
    testOutput "X-$((128 + 13))" = '$(
        exec 3>&1
        if pidsentry --test=1 -d -d -- sh -cx "
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
        if pidsentry -dd -- sh -c "
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
            pidsentry exec -dd -- sh -c "
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
        pidsentry --test=1 -t 4 -- sh -c 'trap : 6 ; sleep 6'
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

trap 'RC=$? ; [ x$RC = x0 ] || rm -rf scratch/* || : ; exit $RC' 0
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
    VALGRIND="$VALGRIND --leak-check=yes"
    VALGRIND="$VALGRIND --suppressions=pidsentry.supp"
    $TEST
done

testCaseBegin 'No lost watchdogs'
testLostWatchdogs
testCaseEnd

testCaseBegin 'Valgrind run over unit tests'
testOutput "" != "/bin/echo $VALGRIND"
testCaseEnd

TESTS=$(/bin/echo $(ls -1 _* | grep -v -F .) )
TESTS_ENVIRONMENT="${TESTS_ENVIRONMENT+$TESTS_ENVIRONMENT }$VALGRIND"
make check TESTS_ENVIRONMENT="$TESTS_ENVIRONMENT" TESTS="$TESTS"

# The error handling test takes a very long time to run, so run a quick
# version of the test, unless TEST_MODE is configured.

testCaseBegin 'Error handling'
(
    [ -n "${TEST_MODE_EXTENDED++}" ] || export PIDSENTRY_TEST_ERROR=once
    ./errortest.sh
)
testCaseEnd
