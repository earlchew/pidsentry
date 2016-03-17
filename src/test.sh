#!/bin/sh

set -eu

# Use explicit call to /bin/echo to ensure that echo is run as a separate
# process from the shell. This is important because SIGPIPE will be
# delivered to the separate echo process, rather than to the shell.

pidsentryexec()
{
    unset PIDSENTRY_TEST_ERROR
    exec libtool --mode=execute $VALGRIND ./pidsentry "$@"
}

pidsentry()
{
    if [ $# -eq 0 -o x"${1:-}" != x"exec" ] ; then
        ( pidsentryexec "$@" )
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

runTest()
{
    :
}

runTests()
{
    testCase 'Usage'
    testExit 1 pidsentry -? -- true

    testCase 'Missing -p option value'
    testExit 1 pidsentry -p

    testCase 'Valid -p option value'
    testExit 0 pidsentry -d -p $PIDFILE -- true
    [ ! -f $PIDFILE ]

    testCase 'Long --pidfile option'
    testExit 0 pidsentry --pidfile $PIDFILE -- true
    [ ! -f $PIDFILE ]

    testCase 'Missing command'
    testExit 1 pidsentry

    testCase 'Simple command'
    REPLY=$(pidsentry -dd /bin/echo test)
    [ x"$REPLY" = x"test" ]

    testCase 'Simple command in test mode'
    REPLY=$(pidsentry -dd --test=1 /bin/echo test)
    [ x"$REPLY" = x"test" ]

    testCase 'Empty pid file'
    rm -f $PIDFILE
    : > $PIDFILE
    testExit 0 pidsentry --test=1 -p $PIDFILE -- true
    [ ! -f $PIDFILE ]

    testCase 'Invalid content in pid file'
    rm -f $PIDFILE
    dd if=/dev/zero bs=1K count=1 > $PIDFILE
    testExit 0 pidsentry --test=1 -p $PIDFILE -- true
    [ ! -f $PIDFILE ]

    testCase 'Dead process in pid file'
    rm -f $PIDFILE
    pidsentry -i -p $PIDFILE -- sh -c 'kill -9 $PPID' || :
    [ -s $PIDFILE ]
    testExit 0 pidsentry --test=1 -d -p $PIDFILE -- true
    [ ! -f $PIDFILE ]

    testCase 'Existing process in pid file'
    rm -f $PIDFILE
    testOutput "OK" = '$(
        pidsentry -i -p $PIDFILE -u -- sh -c "
            while : ; do sleep 1 ; done" | {
                read PARENT UMBILICAL
                read CHILD
                pidsentry -p $PIDFILE -- true || echo OK
             kill -9 $CHILD
        }
    )'
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
    pidsentry --test=1 -d -p $PIDFILE -- true
    wait
    [ ! -f $PIDFILE ]

    testCase 'Read non-existent pid file'
    rm -f $PIDFILE
    testExit 1 pidsentry --test=1 -p $PIDFILE -c -- true
    [ ! -f $PIDFILE ]

    testCase 'Read malformed pid file'
    rm -f $PIDFILE
    date > $PIDFILE
    testExit 1 pidsentry --test=1 -p $PIDFILE -c -- true
    [ -f $PIDFILE ]

    testCase 'Identify processes'
    for REPLY in $(
      exec sh -c '
        /bin/echo $$
        set -- '"$VALGRIND"' ./pidsentry --test=1 -i -- sh -c '\''/bin/echo $$'\'
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
        pidsentry -- grep SigBlk /proc/self/status |
        {
            read HEADING SIGNALS
            /bin/echo "$SIGNALS"
        }
    )'
    testCase 'Test ignored signals in child'
    testOutput "0000000000000000" = '$(
        pidsentry -- grep SigIgn /proc/self/status |
        {
            read HEADING SIGNALS
            /bin/echo "$SIGNALS"
        }
    )'

    testCase 'Environment propagation'
    testOutput '0' = '"$(
        pidsentry -- sh -c '\''date ; printenv'\'' | grep "^K9_" | wc -l)"'

    testCase 'Exit code propagation'
    testExit 2 pidsentry --test=1 -- sh -c 'exit 2'

    testCase 'Signal exit code propagation'
    testExit $((128 + 9)) pidsentry --test=1 -- sh -c '
        /bin/echo Killing $$ ; kill -9 $$'

    testCase 'Child process group'
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

    testCase 'Umbilical process file descriptors'
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

    testCase 'Watchdog process file descriptors'
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

    testCase 'Untethered watchdog process file descriptors'
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

    testCase 'Untethered child process'
    testOutput '$(
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)' = '$(
      pidsentry --test=1 -u -- ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)'

    testCase 'Untethered child process with 8M data'
    testOutput 8192000 = '$(
      pidsentry --test=1 -u -- dd if=/dev/zero bs=8K count=1000 | wc -c)'

    testCase 'Tether with new file descriptor'
    testOutput '$(( 1 + $(
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l) ))' = '$(
      pidsentry --test=1 -f - -- ls -l /proc/self/fd |
          grep "[0-9]-[0-9]" |
          wc -l)'

    testCase 'Tether using stdout'
    testOutput '$(( 0 + $(
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l) ))' = '$(
      pidsentry --test=1 -- ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l)'

    testCase 'Tether using named stdout'
    testOutput '$(( 0 + $(
      ls -l /proc/self/fd | grep "[0-9]-[0-9]" | wc -l) ))' = '$(
      pidsentry --test=1 -f 1 -- ls -l /proc/self/fd |
          grep "[0-9]-[0-9]" |
          wc -l)'

    testCase 'Tether using stdout with 8M data'
    testOutput 8192000 = '$(
      pidsentry --test=1 -- dd if=/dev/zero bs=8K count=1000 | wc -c)'

    testCase 'Tether quietly using stdout with 8M data'
    testOutput 0 = '$(
      pidsentry --test=1 -q -- dd if=/dev/zero bs=8K count=1000 | wc -c)'

    testCase 'Tether named in environment'
    testOutput "TETHER=1" = '$(
      pidsentry --test=1 -n TETHER -- printenv | grep TETHER)'

    testCase 'Tether named alone in argument'
    testOutput "1" = '$(
      pidsentry --test=1 -n @tether@ -- /bin/echo @tether@ | grep "1")'

    testCase 'Tether named as suffix in argument'
    testOutput "x1" = '$(
      pidsentry --test=1 -n @tether@ -- /bin/echo x@tether@ | grep "1")'

    testCase 'Tether named as prefix argument'
    testOutput "1x" = '$(
      pidsentry --test=1 -n @tether@ -- /bin/echo @tether@x | grep "1")'

    testCase 'Tether named as infix argument'
    testOutput "x1x" = '$(
      pidsentry --test=1 -n @tether@ -- /bin/echo x@tether@x | grep "1")'

    testCase 'Early parent death'
    pidsentry -i --test=1 -dd sh -cx 'while : pidsentry ; do sleep 1 ; done' | {
        read PARENT UMBILICAL
        randomsleep 1
        kill -9 $PARENT
        sleep 3
        ! ps -C 'pidsentry sh' -o user=,ppid=,pid=,pgid=,args= | grep k9
    }

    testCase 'Early umbilical death'
    ! ps -C 'pidsentry sh' -o user=,ppid=,pid=,pgid=,args= | grep k9
    pidsentry -i --test=1 -dd sh -cx 'while : pidsentry ; do sleep 1 ; done' | {
        read PARENT UMBILICAL
        randomsleep 1
        kill -9 $UMBILICAL
        SLEPT=0
        while : ; do
            ps -C 'pidsentry sh' -o user=,ppid=,pid=,pgid=,args= |
                grep pidsentry ||
                break
            sleep 1
            [ $(( ++SLEPT )) -lt 60 ] || exit 1
        done
    }

    testCase 'Early child death'
    ! ps -C 'pidsentry sh' -o user=,ppid=,pid=,pgid=,args= | grep k9
    pidsentry -i --test=1 -dd sh -cx 'while : pidsentry ; do sleep 1 ; done' | {
        read PARENT UMBILICAL
        read CHILD
        randomsleep 1
        kill -9 $CHILD
        SLEPT=0
        while : ; do
            ps -C 'pidsentry sh' -o user=,ppid=,pid=,pgid=,args= |
                grep pidsentry ||
                break
            sleep 1
            [ $(( ++SLEPT )) -lt 60 ] || exit 1
        done
    }

    testCase 'Unexpected death of child'
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

    testCase 'Stopped child'
    testOutput OK = '"$(
        pidsentry --test=1 -i -d -t 2,,2 -- sh -c '\''kill -STOP $$'\'' | {
            read PARENT UMBILICAL
            read CHILD
            sleep 8
            kill -CONT $CHILD || { echo NOTOK ; exit 1 ; }
            echo OK
        })"'

    testCase 'Stopped parent'
    testOutput OK = '"$(pidsentry --test=1 -i -d -t 8,2 -- sleep 4 | {
        read PARENT UMBILCAL
        read CHILD
        kill -STOP $PARENT
        sleep 8
        kill -CONT $PARENT || { echo NOTOK ; exit 1 ; }
        echo OK
    })"'

    testCase 'Randomly stopped parent'
    testOutput 'OK' = '$(
        { pidsentry -i -dd sleep 3 && /bin/echo OK ; } | {
            read PARENT UMBILICAL
            read CHILD
            randomsleep 3
            if ! kill -STOP $PARENT ; then
                kill -0 $PARENT && { /bin/echo NOTOK ; exit 1 ; }
            else
                randomsleep 10
                kill -CONT $PARENT || { /bin/echo NOTOK ; exit 1 ; }
            fi
            read REPLY
            /bin/echo $REPLY
        }
    )'

    testCase 'Randomly stopped process family'
    testOutput 'OK' = '$(
        { pidsentry -i -dd sleep 3 && /bin/echo OK ; } | {
            read PARENT UMBILICAL
            read CHILD
            randomsleep 3
            if ! kill -TSTP $PARENT ; then
                kill -0 $PARENT && { /bin/echo NOTOK ; exit 1 ; }
            else
                randomsleep 10
                kill -CONT $PARENT || { /bin/echo NOTOK ; exit 1 ; }
            fi
            read REPLY
            /bin/echo $REPLY
        }
    )'

    testCase 'Broken umbilical'
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
            ! kill -0 $CHILD 2>/dev/null || echo NOTOK
            [ x"$RC" != x"0" ] || echo OK
        }
    )'

    testCase 'Fast signal queueing'
    SIGNALS="1 2 3 15"
    for SIG in $SIGNALS ; do
      pidsentry --test=1 -i -dd -- sh -c "
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
      pidsentry -i --test=1 -dd -- sh -c "
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
        pidsentry --test=1 -i -dd -t 3,,4 -- sh -cx "
            trap : 15 6
            /bin/echo READY
            while : ; do sleep 1 ; done" |
        {
            set -x
            # t+3s : Watchdog times out child and issues kill -ABRT
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
        if pidsentry --test=1 -d -d -- sh -cx "
            while : ; do /bin/echo X || exit \$?; sleep 1 ; done " ; then
            /bin/echo "X-$?" >&3
        else
            /bin/echo "X-$?" >&3
        fi | read
    )'

    testCase 'Test EPIPE propagates to child'
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

    testCase 'Test output is non-blocking even when pipe buffer is filled'
    testOutput AABB = '$(
        exec 3>&1
        {
            trap '\''[ -z "$K9PID" ] || kill -- "$K9PID"'\'' 15
            sh -c '\''/bin/echo $PPID'\''
            dd if=/dev/zero bs=$((64 * 1024)) count=1
            ( sleep 2
                dd if=/dev/zero bs=$((32 * 1024)) count=1 ) &
            pidsentry exec -dd -- sh -c "
               trap '\''/bin/echo -n AA >&3; exit 2'\'' 15
               sleep 2
               dd if=/dev/zero bs=$((64 * 1024)) count=1 &
               while : ; do sleep 1 ; done" &
            K9PID=$!
            wait $K9PID
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

    testCase 'Timeout with data that must be flushed after 6s'
    REPLY=$(
        START=$(date +%s)
        pidsentry --test=1 -t 4 -- sh -c 'trap : 6 ; sleep 6'
        STOP=$(date +%s)
        /bin/echo $(( STOP - START))
    )
    [ "$REPLY" -ge 6 ]
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

trap 'rm -rf scratch/*' 0
mkdir -p scratch

PIDFILE=scratch/pidfile

testCase 'No running watchdogs'
testOutput "" = '$(ps -C pidsentry -o user=,ppid=,pid=,pgid=,command=)'

for TEST in runTest runTests ; do
    VALGRIND=
    $TEST
    VALGRIND="valgrind --error-exitcode=128 --leak-check=yes"
    $TEST
done

testCase 'No lost watchdogs'
testOutput "" = '$(ps -C pidsentry -o user=,ppid=,pid=,pgid=,command=)'

testCase 'Valgrind run over unit tests'
testOutput "" != "/bin/echo $VALGRIND"

TESTS=$(/bin/echo $(ls -1 _* | grep -v -F .) )
TESTS_ENVIRONMENT="${TESTS_ENVIRONMENT+$TESTS_ENVIRONMENT }$VALGRIND"
make check TESTS_ENVIRONMENT="$TESTS_ENVIRONMENT" TESTS="$TESTS"

# The error handling test takes a very long time to run, so run a quick
# version of the test, unless TEST_MODE is configured.

testCase 'Error handling'
(
    [ -n "${TEST_MODE_EXTENDED++}" ] || export PIDSENTRY_TEST_ERROR=once
    ./errortest.sh
)
