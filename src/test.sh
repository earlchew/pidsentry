#!/bin/sh

set -eu

testCase()
{
    printf '\ntestCase - %s\n' "$1"
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
    if ! eval \[ x"$1" $2 x"$3" \] ; then
       eval testTrace \[ x"$1" $2 x"$3" \]
       testFail "$@"
    fi
}

runWait()
{
    $VALGRIND "$@"
}

runTests()
{
    testCase 'Usage'
    testExit 1 runWait ./k9 -? -- true

    testCase 'Missing -P option value'
    testExit 1 runWait ./k9 -P

    testCase 'Illegal negative -P option value'
    testExit 1 runWait ./k9 -P -2 -- true

    testCase 'Valid -P option values'
    testExit 0 runWait ./k9 -P -1 -- true
    testExit 0 runWait ./k9 -P 0 -- true
    testExit 0 runWait ./k9 -P 1 -- true

    testCase 'Long --pid option'
    testExit 0 runWait ./k9 --pid 1 -- true

    testCase 'Missing -p option value'
    testExit 1 runWait ./k9 -p

    testCase 'Valid -p option value'
    testExit 0 runWait ./k9 -d -p pidfile -- true
    [ ! -f pidfile ]

    testCase 'Long --pidfile option'
    testExit 0 runWait ./k9 --pidfile pidfile -- true
    [ ! -f pidfile ]

    testCase 'Missing command'
    testExit 1 runWait ./k9

    testCase 'Simple command'
    REPLY=$(runWait ./k9 echo test)
    [ x"$REPLY" = x"test" ]

    testCase 'Simple command in test mode'
    REPLY=$(runWait ./k9 -T echo test)
    [ x"$REPLY" = x"test" ]

    testCase 'Empty pid file'
    rm -f pidfile
    : > pidfile
    testExit 0 runWait ./k9 -T -p pidfile -- true
    [ ! -f pidfile ]

    testCase 'Invalid content in pid file'
    rm -f pidfile
    dd if=/dev/zero bs=1K count=1 > pidfile
    testExit 0 runWait ./k9 -T -p pidfile -- true
    [ ! -f pidfile ]

    testCase 'Dead process in pid file'
    rm -f pidfile
    sh -c 'echo $$' > pidfile
    testExit 0 runWait ./k9 -T -d -p pidfile -- true
    [ ! -f pidfile ]

    testCase 'Aliased process in pid file'
    rm -f pidfile
    ( sh -c 'echo $(( $$ + 1))' > pidfile ; sleep 1 ) &
    sleep 1
    testExit 0 runWait ./k9 -T -d -p pidfile -- true
    wait
    [ ! -f pidfile ]

    testCase 'Read non-existent pid file'
    rm -f pidfile
    testExit 1 runWait ./k9 -T -p pidfile
    [ ! -f pidfile ]

    testCase 'Read malformed pid file'
    rm -f pidfile
    date > pidfile
    testExit 1 runWait ./k9 -T -p pidfile
    [ -f pidfile ]

    testCase 'Identify processes'
    for REPLY in $(
      exec sh -c 'echo $$ ; exec ./k9 -T -i -- sh -c '\''echo $$'\' |
      {
        read REALPARENT
        read PARENT ; read CHILD
        read REALCHILD
        echo "$REALPARENT/$PARENT $REALCHILD/$CHILD"
      }) ; do

      [ x"${REPLY%% *}" = x"${REPLY#* }" ]
    done

    testCase 'Exit code propagation'
    testExit 2 runWait ./k9 -T -- sh -c 'exit 2'

    testCase 'Signal exit code propagation'
    testExit $((128 + 9)) runWait ./k9 -T -- sh -c '
        echo Killing $$ ; kill -9 $$'

    testCase 'Child shares process group'
    runWait ./k9 -i -- ps -o pid,pgid,cmd | {
        read PARENT
        read CHILD
        read HEADING
        PARENTPGID=
        CHILDPGID=
        while read PID PGID CMD ; do
            echo "$PARENT - $CHILD - $PID $PGID $CMD" >&2
            if [ x"$PARENT" = x"$PID" ] ; then
                CHILDPGID=$PGID
            elif [ x"$CHILD" = x"$PID" ] ; then
                PARENTPGID=$PGID
            fi
        done
        [ -n "$PARENTPGID" ]
        [ -n "$CHILDPGID" ]
        [ x"$PARENTPGID" = x"$CHILDPGID" ]
    }

    testCase 'Child sets own process group'
    runWait ./k9 -i -s -- ps -o pid,pgid,cmd | {
        read PARENT
        read CHILD
        read HEADING
        PARENTPGID=
        CHILDPGID=
        while read PID PGID CMD ; do
            echo "$PARENT - $CHILD - $PID $PGID $CMD" >&2
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
      runWait ./k9 -T -u -- ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)'

    testCase 'Untethered child process with 8M data'
    testOutput 8192000 = '$(
      runWait ./k9 -T -u -- dd if=/dev/zero bs=8K count=1000 | wc -c)'

    testCase 'Tether with new file descriptor'
    testOutput '$(( 1 + $(
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l) ))' = '$(
      runWait ./k9 -T -f - -- ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)'

    testCase 'Tether using stdout'
    testOutput '$(
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)' = '$(
      runWait ./k9 -T -- ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)'

    testCase 'Tether using named stdout'
    testOutput '$(
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)' = '$(
      runWait ./k9 -T -f 1 -- ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)'

    testCase 'Tether using stdout with 8M data'
    testOutput 8192000 = '$(
      runWait ./k9 -T -- dd if=/dev/zero bs=8K count=1000 | wc -c)'

    testCase 'Tether quietly using stdout with 8M data'
    testOutput 0 = '$(
      runWait ./k9 -T -q -- dd if=/dev/zero bs=8K count=1000 | wc -c)'

    testCase 'Tether named in environment'
    testOutput "TETHER=1" = '$(
      runWait ./k9 -T -n TETHER -- printenv | grep TETHER)'

    testCase 'Tether named alone in argument'
    testOutput "1" = '$(
      runWait ./k9 -T -n @tether@ -- echo @tether@ | grep "1")'

    testCase 'Tether named as suffix in argument'
    testOutput "x1" = '$(
      runWait ./k9 -T -n @tether@ -- echo x@tether@ | grep "1")'

    testCase 'Tether named as prefix argument'
    testOutput "1x" = '$(
      runWait ./k9 -T -n @tether@ -- echo @tether@x | grep "1")'

    testCase 'Tether named as infix argument'
    testOutput "x1x" = '$(
      runWait ./k9 -T -n @tether@ -- echo x@tether@x | grep "1")'

    testCase 'Unexpected death of child'
    REPLY=$(
      exec 3>&1
      {
        if $VALGRIND ./k9 -T -d -i -- sh -c '
            while : ; do : ; done ; exit 0' 3>&- ; then
          echo 0 >&3
        else
          echo $? >&3
        fi
      } | {
        exec >&2
        read PARENT ; echo "Parent $PARENT"
        read CHILD  ; echo "Child $CHILD"
        kill -- "$CHILD"
      })
    [ x"$REPLY" = x$((128 + 15)) ]

    testCase 'Fast signal queueing'
    SIGNALS="1 2 3 15"
    for SIG in $SIGNALS ; do
        sh -c '
          echo $$
          exec >/dev/null
          exec setsid '"$VALGRIND"' ./k9 -T -dd -- sh -c "
              trap '\''exit 1'\'''"$SIGNALS"'
              while : ; do sleep 1 ; done"' |
        {
           read REPLY
           while kill -0 "$REPLY" 2>&- ; do
               kill -"$SIG" "$REPLY"
               date ; echo kill -"$SIG" "$REPLY"
               kill -0 "$REPLY" 2>&- || break
               date
               sleep 1
           done >&2
           ps -s "$REPLY" -o pid,pgid,cmd | tail -n +2 || :
           kill -9 -"$REPLY" 2>&- || :
           echo OK
        } | {
            read REPLY
            [ x"$REPLY" = x"OK" ]
        }
    done

    testCase 'Slow signal queueing'
    SIGNALS="1 2 3 15"
    for SIG in $SIGNALS ; do
        sh -c '
          echo $$
          exec >/dev/null
          exec setsid '"$VALGRIND"' ./k9 -T -dd -- sh -c "
              trap '\''exit 1'\'''"$SIGNALS"'
              while : ; do sleep 1 ; done"' |
        {
           read REPLY
           sleep 1
           while kill -0 "$REPLY" 2>&- ; do
               kill -"$SIG" "$REPLY"
               date ; echo kill -"$SIG" "$REPLY"
               kill -0 "$REPLY" 2>&- || break
               date
               sleep 1
           done >&2
           ps -s "$REPLY" -o pid,pgid,cmd | tail -n +2 || :
           kill -9 -"$REPLY" 2>&- || :
           echo OK
        } | {
            read REPLY
            [ x"$REPLY" = x"OK" ]
        }
    done

    testCase 'Timeout with data that must be flushed after 6s'
    REPLY=$(
        /usr/bin/time -p $VALGRIND ./k9 -T -t 4 -- sh -c '
            trap "" 15 ; sleep 6' 2>&1 | grep real)
    REPLY=${REPLY%%.*}
    REPLY=${REPLY##* }
    [ "$REPLY" -ge 6 ]
}

VALGRIND=
runTests

VALGRIND="valgrind --error-exitcode=128 --leak-check=yes"
runTests
