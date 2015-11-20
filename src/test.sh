#!/bin/sh

set -eu

testCase()
{
    printf '\ntestCase - %s\n' "$1"
}

testExit()
{
    if [ x$1 = x0 ] ; then
        shift ; "$@"
    else
        if ( shift ; "$@" ) ; then
            false
        else
            [ $? -eq $1 ]
        fi
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
    runWait ./k9 -P -1 -- true
    runWait ./k9 -P 0 -- true
    runWait ./k9 -P 1 -- true

    testCase 'Long --pid option'
    runWait ./k9 --pid 1 -- true

    testCase 'Missing -p option value'
    testExit 1 runWait ./k9 -p

    testCase 'Valid -p option value'
    runWait ./k9 -d -p pidfile -- true

    testCase 'Long --pidfile option'
    runWait ./k9 --pidfile pidfile -- true

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
    runWait ./k9 -T -p pidfile -- true

    testCase 'Invalid content in pid file'
    rm -f pidfile
    dd if=/dev/zero bs=1K count=1 > pidfile
    runWait ./k9 -T -p pidfile -- true

    testCase 'Dead process in pid file'
    rm -f pidfile
    sh -c 'echo $$' > pidfile
    runWait ./k9 -T -d -p pidfile -- true

    testCase 'Aliased process in pid file'
    rm -f pidfile
    ( sh -c 'echo $(( $$ + 1))' > pidfile ; sleep 1 ) &
    sleep 1
    runWait ./k9 -T -d -p pidfile -- true
    wait

    testCase 'Read non-existent pid file'
    rm -f pidfile
    testExit 1 runWait ./k9 -T -p pidfile

    testCase 'Read malformed pid file'
    rm -f pidfile
    date > pidfile
    testExit 1 runWait ./k9 -T -p pidfile

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

    testCase 'Signal propagation'
    testExit $((128 + 9)) runWait ./k9 -T -- sh -c 'echo $$ ; kill -9 $$'

    testCase 'Untethered child process'
    [ $(runWait ./k9 -T -u -- ls -l /proc/self/fd | grep '[0-9]-[0-9]' | wc -l) -eq 4 ]

    testCase 'Untethered child process with 8M data'
    [ $(runWait ./k9 -T -u -- dd if=/dev/zero bs=8K count=1000 | wc -c) -eq 8192000 ]

    testCase 'Tether with new file descriptor'
    [ $(runWait ./k9 -T -f - -- ls -l /proc/self/fd | grep '[0-9]-[0-9]' | wc -l) -eq 5 ]

    testCase 'Tether using stdout'
    [ $(runWait ./k9 -T -- ls -l /proc/self/fd | grep '[0-9]-[0-9]' | wc -l) -eq 4 ]

    testCase 'Tether using named stdout'
    [ $(runWait ./k9 -T -f 1 -- ls -l /proc/self/fd | grep '[0-9]-[0-9]' | wc -l) -eq 4 ]

    testCase 'Tether using stdout with 8M data'
    [ $(runWait ./k9 -T -- dd if=/dev/zero bs=8K count=1000 | wc -c) -eq 8192000 ]

    testCase 'Tether quietly using stdout with 8M data'
    [ $(runWait ./k9 -T -q -- dd if=/dev/zero bs=8K count=1000 | wc -c) -eq 0 ]

    testCase 'Tether named in environment'
    [ $(runWait ./k9 -T -n TETHER -- printenv | grep TETHER) = "TETHER=1" ]

    testCase 'Tether named alone in argument'
    [ $(runWait ./k9 -T -n @tether@ -- echo @tether@ | grep '1') = "1" ]

    testCase 'Tether named as suffix in argument'
    [ $(runWait ./k9 -T -n @tether@ -- echo x@tether@ | grep '1' ) = "x1" ]

    testCase 'Tether named as prefix argument'
    [ $(runWait ./k9 -T -n @tether@ -- echo @tether@x | grep '1' ) = "1x" ]

    testCase 'Tether named as infix argument'
    [ $(runWait ./k9 -T -n @tether@ -- echo x@tether@x | grep '1' ) = "x1x" ]

    testCase 'Unexpected death of child'
    REPLY=$(
      exec 3>&1
      {
        if $VALGRIND./k9 -T -d -i -- sh -c '
            while : ; do : ; done ; exit 0' 3>&- ; then
          echo 0 >&3
        else
          echo $? >&3
        fi
      } | {
        read PID
        kill -- "$PID"
      })
    [ x"$REPLY" = x$((128 + 15)) ]

    testCase 'Fast signal queueing'
    REPLY=$(
      SIGNALS="1 2 3 15"
      sh -c "
        echo \$\$
        exec $VALGRIND ./k9 -T -d -- sh -c '
          C=0
          trap '\\'': \$(( ++C ))'\\'' $SIGNALS
          echo \$\$
          while [ \$C -ne $(echo $SIGNALS | wc -w) ] ; do sleep 1 ; done
          echo \$C'" |
      {
        read PARENT
        read CHILD
        for SIG in $SIGNALS ; do
            kill -$SIG -- "$PARENT"
        done
        read COUNT
        echo "$COUNT"
      })
    [ x"$REPLY" = x"4" ]

    testCase 'Slow signal queueing'
    REPLY=$(
      SIGNALS="1 2 3 15"
      sh -c "
        echo \$\$
        exec $VALGRIND ./k9 -T -d -- sh -c '
          C=0
          trap '\\'': \$(( ++C ))'\\'' $SIGNALS
          echo \$\$
          while [ \$C -ne $(echo $SIGNALS | wc -w) ] ; do sleep 1 ; done
          echo \$C'" |
      {
        read PARENT
        read CHILD
        for SIG in $SIGNALS ; do
            kill -$SIG -- "$PARENT"
            sleep 1
        done
        read COUNT
        echo "$COUNT"
      })
    [ x"$REPLY" = x"4" ]

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
