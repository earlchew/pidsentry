#!/bin/sh

set -eu

# Use explicit call to /bin/echo to ensure that echo is run as a separate
# process from the shell. This is important because SIGPIPE will be
# delivered to the separate echo process, rather than to the shell.

k9()
{
    setsid libtool --mode=execute $VALGRIND ./k9 "$@"
}

random()
{
    printf '%s' $(( 0x$(openssl rand -hex 4) ))
}

randomsleep()
{
    sleep $(( $(random) % $1 )).$(( $(random) % 1000 ))
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
            if ( shift ; VALGRIND= ; "$@" ) ; then
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
    testExit 0 k9 -d -p $PIDFILE -- true
    [ ! -f $PIDFILE ]

    testCase 'Long --pidfile option'
    testExit 0 k9 --pidfile $PIDFILE -- true
    [ ! -f $PIDFILE ]

    testCase 'Missing command'
    testExit 1 k9

    testCase 'Simple command'
    REPLY=$(k9 -dd /bin/echo test)
    [ x"$REPLY" = x"test" ]

    testCase 'Simple command in test mode'
    REPLY=$(k9 -dd -T /bin/echo test)
    [ x"$REPLY" = x"test" ]

    testCase 'Empty pid file'
    rm -f $PIDFILE
    : > $PIDFILE
    testExit 0 k9 -T -p $PIDFILE -- true
    [ ! -f $PIDFILE ]

    testCase 'Invalid content in pid file'
    rm -f $PIDFILE
    dd if=/dev/zero bs=1K count=1 > $PIDFILE
    testExit 0 k9 -T -p $PIDFILE -- true
    [ ! -f $PIDFILE ]

    testCase 'Dead process in pid file'
    rm -f $PIDFILE
    sh -c '/bin/echo $$' > $PIDFILE
    testExit 0 k9 -T -d -p $PIDFILE -- true
    [ ! -f $PIDFILE ]

    testCase 'Aliased process in pid file'
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
    k9 -T -d -p $PIDFILE -- true
    wait
    [ ! -f $PIDFILE ]

    testCase 'Read non-existent pid file'
    rm -f $PIDFILE
    testExit 1 k9 -T -p $PIDFILE
    [ ! -f $PIDFILE ]

    testCase 'Read malformed pid file'
    rm -f $PIDFILE
    date > $PIDFILE
    testExit 1 k9 -T -p $PIDFILE
    [ -f $PIDFILE ]

    testCase 'Identify processes'
    for REPLY in $(
      exec sh -c '
        /bin/echo $$
        set -- '"$VALGRIND"' ./k9 -T -i -- sh -c '\''/bin/echo $$'\'
        exec libtool --mode=execute "$@"
      {
        read REALPARENT
        read PARENT UMBILICAL ; read CHILD
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

    testCase 'Child process group'
    k9 -i -- ps -o pid,pgid,cmd | {
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
        read PARENT UMBILICAL ; /bin/echo "Parent $PARENT $UMBILICAL"
        read CHILD            ; /bin/echo "Child $CHILD"
        kill -9 -- "$CHILD"
      })
    [ x"$REPLY" = x$((128 + 9)) ]

    testCase 'Stopped child'
    testOutput OK = '"$(k9 -T -i -d -t 2,,2 -- sh -c '\''kill -STOP $$'\'' | {
        read PARENT UMBILICAL
        read CHILD
        sleep 8
        kill -CONT $CHILD || { echo NOTOK ; exit 1 ; }
        echo OK
    })"'

    testCase 'Stopped parent'
    testOutput OK = '"$(k9 -T -i -d -t 8,2 -- sleep 4 | {
        read PARENT UMBILCAL
        read CHILD
        kill -STOP $PARENT
        sleep 8
        kill -CONT $PARENT || { echo NOTOK ; exit 1 ; }
        echo OK
    })"'

    testCase 'Randomly stopped parent'
    testOutput 'OK' = '$(
        { k9 -i -dd sleep 3 && /bin/echo OK ; } | {
            read PARENT UMBILICAL
            read CHILD
            randomsleep 3
            kill -STOP $PARENT || { /bin/echo NOTOK ; exit 1 ; }
            randomsleep 10
            kill -CONT $PARENT || { /bin/echo NOTOK ; exit 1 ; }
            read REPLY
            /bin/echo $REPLY
        }
    )'

    testCase 'Broken umbilical'
    testOutput "OK" = '$(
        exec 3>&1
        k9 -dd -i -- sleep 9 | {
            read PARENT UMBILICAL
            read CHILD
            sleep 3
            kill -0 $CHILD
            RC=$?
            kill -9 $PARENT
            sleep 3
            ! kill -0 $CHILD 2>/dev/null || echo NOTOK
            [ x"$RC" != x"0" ] || echo OK
        }
    )'

    testCase 'Fast signal queueing'
    SIGNALS="1 2 3 15"
    for SIG in $SIGNALS ; do
      k9 -T -i -dd -- sh -c "
            trap 'exit 1' $SIGNALS
            while : ; do sleep 1 ; done" |
      {
         read PARENT UMBILICAL
         while kill -0 "$PARENT" 2>&- ; do
             kill -"$SIG" "$PARENT"
             date ; /bin/echo kill -"$SIG" "$PARENT"
             kill -0 "$PARENT" 2>&- || break
             date
             sleep 1
         done >&2
         read CHILD
         kill -0 $CHILD 2>&- || /bin/echo OK
      } | {
          read REPLY
          [ x"$REPLY" = x"OK" ]
      }
    done

    testCase 'Slow signal queueing'
    SIGNALS="1 2 3 15"
    for SIG in $SIGNALS ; do
      k9 -i -T -dd -- sh -c "
            trap 'exit 1' $SIGNALS
            while : ; do sleep 1 ; done" |
      {
         read PARENT UMBILICAL
         sleep 1
         while kill -0 "$PARENT" 2>&- ; do
             kill -"$SIG" "$PARENT"
             date ; /bin/echo kill -"$SIG" "$PARENT"
             kill -0 "$PARENT" 2>&- || break
             date
             sleep 1
         done >&2
         read CHILD
         kill -0 $CHILD 2>&- || /bin/echo OK
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
            read PARENT UMBILCAL
            read CHILD
            read READY # Signal handler established
            while : ; do sleep 1 ; kill $PARENT 2>&- ; done &
            SLAVE=$!
            sleep 4 # Wait for watchdog to send first signal
            read -t 0 && echo FAIL
            sleep 10 # Watchdog should have terminated child and exited
            read -t 0 && echo OK || echo FAIL
            kill -9 $SLAVE 2>&-
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
        fi | dd of=/dev/null bs=1 count=$((1 + $(random) % 128))
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
            read PARENT UMBILCAL

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

trap 'rm -rf scratch/*' 0
mkdir -p scratch

PIDFILE=scratch/pidfile

testCase 'No running watchdogs'
testOutput "" = '$(ps -C k9 -o user=,ppid=,pid=,pgid=,command=)'

for TEST in runTest runTests ; do
    VALGRIND=
    $TEST
    VALGRIND="valgrind --error-exitcode=128 --leak-check=yes"
    $TEST
done

testCase 'No lost watchdogs'
testOutput "" = '$(ps -C k9 -o user=,ppid=,pid=,pgid=,command=)'
