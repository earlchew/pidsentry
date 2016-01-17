#!/bin/sh

set -eu

# Use explicit call to /bin/echo to ensure that echo is run as a separate
# process from the shell. This is important because SIGPIPE will be
# delivered to the separate echo process, rather than to the shell.

k9()
{
    libtool --mode=execute $VALGRIND ./k9 "$@"
}

testCase()
{
    printf '\ntestCase - %s\n' "$1"
    TESTCASE=$1
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
            if ( shift ; "$@" ) ; then
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
    eval set -- '"$@"' "$1"
    eval set -- '"$@"' "$3"
    if ! [ x"$4" "$2" x"$5" ] ; then
       testTrace [ x"$4" "$2" x"$5" ]
       set -- "$1" "$2" "$3"
       testFail "$@"
    fi
}

runTest()
{
    :
}

runTests()
{
    testCase 'Usage'
    testExit 1 k9 -? -- true

    testCase 'Missing -P option value'
    testExit 1 k9 -P

    testCase 'Illegal negative -P option value'
    testExit 1 k9 -P -2 -- true

    testCase 'Valid -P option values'
    testExit 0 k9 -dd -P -1 -- true
    testExit 0 k9 -dd -P 0 -- true
    testExit 0 k9 -dd -P 1 -- true

    testCase 'Long --pid option'
    testExit 0 k9 --pid 1 -- true

    testCase 'Missing -p option value'
    testExit 1 k9 -p

    testCase 'Valid -p option value'
    testExit 0 k9 -d -p pidfile -- true
    [ ! -f pidfile ]

    testCase 'Long --pidfile option'
    testExit 0 k9 --pidfile pidfile -- true
    [ ! -f pidfile ]

    testCase 'Missing command'
    testExit 1 k9

    testCase 'Simple command'
    REPLY=$(k9 -dd /bin/echo test)
    [ x"$REPLY" = x"test" ]

    testCase 'Simple command in test mode'
    REPLY=$(k9 -dd -T /bin/echo test)
    [ x"$REPLY" = x"test" ]

    testCase 'Empty pid file'
    rm -f pidfile
    : > pidfile
    testExit 0 k9 -T -p pidfile -- true
    [ ! -f pidfile ]

    testCase 'Invalid content in pid file'
    rm -f pidfile
    dd if=/dev/zero bs=1K count=1 > pidfile
    testExit 0 k9 -T -p pidfile -- true
    [ ! -f pidfile ]

    testCase 'Dead process in pid file'
    rm -f pidfile
    sh -c '/bin/echo $$' > pidfile
    testExit 0 k9 -T -d -p pidfile -- true
    [ ! -f pidfile ]

    testCase 'Aliased process in pid file'
    rm -f pidfile
    ( sh -c '/bin/echo $(( $$ + 1))' > pidfile ; sleep 1 ) &
    sleep 1
    testExit 0 k9 -T -d -p pidfile -- true
    wait
    [ ! -f pidfile ]

    testCase 'Read non-existent pid file'
    rm -f pidfile
    testExit 1 k9 -T -p pidfile
    [ ! -f pidfile ]

    testCase 'Read malformed pid file'
    rm -f pidfile
    date > pidfile
    testExit 1 k9 -T -p pidfile
    [ -f pidfile ]

    testCase 'Identify processes'
    for REPLY in $(
      exec sh -c '
        /bin/echo $$
        set -- '"$VALGRIND"' ./k9 -T -i -- sh -c '\''/bin/echo $$'\'
        exec libtool --mode=execute "$@"
      {
        read REALPARENT
        read PARENT ; read CHILD
        read REALCHILD
        /bin/echo "$REALPARENT/$PARENT $REALCHILD/$CHILD"
      }) ; do

      [ x"${REPLY%% *}" = x"${REPLY#* }" ]
    done

    testCase 'Test blocked signals in child'
    testOutput "0000000000000000" = '$(
        k9 -- grep SigBlk /proc/self/status |
        {
            read HEADING SIGNALS
            /bin/echo "$SIGNALS"
        }
    )'
    testCase 'Test ignored signals in child'
    testOutput "0000000000000000" = '$(
        k9 -- grep SigIgn /proc/self/status |
        {
            read HEADING SIGNALS
            /bin/echo "$SIGNALS"
        }
    )'

    testCase 'Environment propagation'
    testOutput '0' = '"$(
        k9 -- sh -c '\''date ; printenv'\'' | grep "^K9_" | wc -l)"'

    testCase 'Exit code propagation'
    testExit 2 k9 -T -- sh -c 'exit 2'

    testCase 'Signal exit code propagation'
    testExit $((128 + 9)) k9 -T -- sh -c '
        /bin/echo Killing $$ ; kill -9 $$'

    testCase 'Child shares process group'
    k9 -i -dd -- ps -o pid,pgid,cmd | {
        read PARENT
        read CHILD
        read HEADING
        PARENTPGID=
        CHILDPGID=
        while read PID PGID CMD ; do
            /bin/echo "$PARENT - $CHILD - $PID $PGID $CMD"
            if [ x"$PARENT" = x"$PID" ] ; then
                CHILDPGID=$PGID
            elif [ x"$CHILD" = x"$PID" ] ; then
                PARENTPGID=$PGID
            fi
        done >&2
        [ -n "$PARENTPGID" ]
        [ -n "$CHILDPGID" ]
        [ x"$PARENTPGID" = x"$CHILDPGID" ]
    }

    testCase 'Child sets own process group'
    k9 -i -s -- ps -o pid,pgid,cmd | {
        read PARENT
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

    testCase 'Untethered child process'
    testOutput '$(
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)' = '$(
      k9 -T -u -- ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)'

    testCase 'Untethered child process with 8M data'
    testOutput 8192000 = '$(
      k9 -T -u -- dd if=/dev/zero bs=8K count=1000 | wc -c)'

    testCase 'Tether with new file descriptor'
    testOutput '$(( 1 + $(
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l) ))' = '$(
      k9 -T -f - -- ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)'

    testCase 'Tether using stdout'
    testOutput '$(( 0 + $(
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l) ))' = '$(
      k9 -T -- ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)'

    testCase 'Tether using named stdout'
    testOutput '$(( 0 + $(
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l) ))' = '$(
      k9 -T -f 1 -- ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)'

    testCase 'Tether using stdout with 8M data'
    testOutput 8192000 = '$(
      k9 -T -- dd if=/dev/zero bs=8K count=1000 | wc -c)'

    testCase 'Tether quietly using stdout with 8M data'
    testOutput 0 = '$(
      k9 -T -q -- dd if=/dev/zero bs=8K count=1000 | wc -c)'

    testCase 'Tether named in environment'
    testOutput "TETHER=1" = '$(
      k9 -T -n TETHER -- printenv | grep TETHER)'

    testCase 'Tether named alone in argument'
    testOutput "1" = '$(
      k9 -T -n @tether@ -- /bin/echo @tether@ | grep "1")'

    testCase 'Tether named as suffix in argument'
    testOutput "x1" = '$(
      k9 -T -n @tether@ -- /bin/echo x@tether@ | grep "1")'

    testCase 'Tether named as prefix argument'
    testOutput "1x" = '$(
      k9 -T -n @tether@ -- /bin/echo @tether@x | grep "1")'

    testCase 'Tether named as infix argument'
    testOutput "x1x" = '$(
      k9 -T -n @tether@ -- /bin/echo x@tether@x | grep "1")'

    testCase 'Unexpected death of child'
    REPLY=$(
      exec 3>&1
      {
        if k9 -T -d -i -- sh -c '
            while : ; do : ; done ; exit 0' 3>&- ; then
          /bin/echo 0 >&3
        else
          /bin/echo $? >&3
        fi
      } | {
        exec >&2
        read PARENT ; /bin/echo "Parent $PARENT"
        read CHILD  ; /bin/echo "Child $CHILD"
        kill -9 -- "$CHILD"
      })
    [ x"$REPLY" = x$((128 + 9)) ]

    testCase 'Stopped child'
    testOutput OK = '"$(k9 -i -d -t 2,,2 -- sh -c '\''kill -STOP $$'\'' | {
        read PARENT
        read CHILD
        sleep 8
        kill -CONT $CHILD || { echo NOTOK ; exit 1 ; }
        echo OK
    })"'

    testCase 'Fast signal queueing'
    SIGNALS="1 2 3 15"
    for SIG in $SIGNALS ; do
      sh -c '
        /bin/echo $$
        exec >/dev/null
        exec setsid libtool --mode=execute '"$VALGRIND"' ./k9 -T -dd -- sh -c "
            trap '\''exit 1'\'''"$SIGNALS"'
            while : ; do sleep 1 ; done"' |
      {
         read REPLY
         while kill -0 "$REPLY" 2>&- ; do
             kill -"$SIG" "$REPLY"
             date ; /bin/echo kill -"$SIG" "$REPLY"
             kill -0 "$REPLY" 2>&- || break
             date
             sleep 1
         done >&2
         ps -s "$REPLY" -o pid,pgid,cmd | tail -n +2 || :
         kill -9 -"$REPLY" 2>&- || :
         /bin/echo OK
      } | {
          read REPLY
          [ x"$REPLY" = x"OK" ]
      }
    done

    testCase 'Slow signal queueing'
    SIGNALS="1 2 3 15"
    for SIG in $SIGNALS ; do
      sh -c '
        /bin/echo $$
        exec >/dev/null
        exec setsid libtool --mode=execute '"$VALGRIND"' ./k9 -T -dd -- sh -c "
            trap '\''exit 1'\'''"$SIGNALS"'
            while : ; do sleep 1 ; done"' |
      {
         read REPLY
         sleep 1
         while kill -0 "$REPLY" 2>&- ; do
             kill -"$SIG" "$REPLY"
             date ; /bin/echo kill -"$SIG" "$REPLY"
             kill -0 "$REPLY" 2>&- || break
             date
             sleep 1
         done >&2
         ps -s "$REPLY" -o pid,pgid,cmd | tail -n +2 || :
         kill -9 -"$REPLY" 2>&- || :
         /bin/echo OK
      } | {
          read REPLY
          [ x"$REPLY" = x"OK" ]
      }
    done

    testCase 'Fixed termination deadline'
    testOutput OK = '$(
        k9 -T -i -dd -t 3,,4 -- sh -cx "
            trap : 15
            /bin/echo READY
            while : ; do sleep 1 ; done" |
        {
            set -x
            # t+3s : Watchdog times out child and issues kill -TERM
            # t+7s : Watchdog escalates and issues kill -KILL
            read PARENT
            read CHILD
            read READY # Signal handler established
            while : ; do sleep 1 ; kill $PARENT 2>&- ; done &
            SLAVE=$!
            sleep 4 # Wait for watchdog to send first signal
            read -t 0 && echo FAIL
            sleep 10 # Watchdog should have terminated child and exited
            read -t 0 && echo OK || echo FAIL
            kill -9 $SLAVE 2>&-
            kill -9 $CHILD 2>&-
        }
    )'

    testCase 'Test SIGPIPE propagates from child'
    testOutput "X-$((128 + 13))" = '$(
        exec 3>&1
        if k9 -T -d -d -- sh -cx "
            while : ; do /bin/echo X || exit \$?; sleep 1 ; done " ; then
            /bin/echo "X-$?" >&3
        else
            /bin/echo "X-$?" >&3
        fi | read
    )'

    testCase 'Test EPIPE propagates to child'
    testOutput "X-2" = '$(
        exec 3>&1
        if k9 -dd -- sh -c "
            sleep 1 ;
            while : ; do
                /bin/echo X || exit 2
            done"
        then
            /bin/echo "X-$?" >&3
        else
            /bin/echo "X-$?" >&3
        fi | dd of=/dev/null bs=1 count=$((1 + RANDOM % 128))
    )'

    testCase 'Test output is non-blocking even when pipe buffer is filled'
    testOutput AABB = '$(
        exec 3>&1
        {
            sh -c '\''/bin/echo $PPID'\''
            dd if=/dev/zero bs=$((64 * 1024)) count=1
            ( sleep 2
                dd if=/dev/zero bs=$((32 * 1024)) count=1 ) &
            exec libtool --mode=execute '"$VALGRIND"' ./k9 -dd -- sh -c "
               trap '\''/bin/echo -n AA >&3; exit 2'\'' 15
               sleep 2
               dd if=/dev/zero bs=$((64 * 1024)) count=1 &
               while : ; do sleep 1 ; done" || :
        } | {
            read PARENT

            # Wait a little time before making some space in the pipe. The
            # two pipe writers will compete to fill the space.
            sleep 3
            dd of=/dev/null bs=$((32 * 1024)) count=1

            # Provide some time to let the competitors fill the pipe, then
            # try to have the watchdoog propagate a signal to the child.

            sleep 2
            kill $PARENT
            sleep 2
            /bin/echo -n BB
        }
    )'

    testCase 'Timeout with data that must be flushed after 6s'
    REPLY=$(
      /usr/bin/time -p libtool --mode=execute $VALGRIND ./k9 -T -t 4 -- sh -c '
        trap "" 15 ; sleep 6' 2>&1 | grep real)
    REPLY=${REPLY%%.*}
    REPLY=${REPLY##* }
    [ "$REPLY" -ge 6 ]
}

for TEST in runTest runTests ; do
    VALGRIND=
    $TEST
    VALGRIND="valgrind --error-exitcode=128 --leak-check=yes"
    $TEST
done
