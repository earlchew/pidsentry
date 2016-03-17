#!/bin/sh

set -eu

random()
{
    printf '%s' $(( 0x$(openssl rand -hex 4) ))
}

pidsentry()
{
    libtool --mode=execute $VALGRIND $VALGRINDOPT ./pidsentry "$@"
}

pidsentrytest()
{
    pidsentry -dd --test=2 -- dd if=/dev/zero bs=64K count=4
}

TRIGGER=1
if [ -n "${PIDSENTRY_TEST_ERROR++}" ] ; then
    case "$PIDSENTRY_TEST_ERROR" in
    [0-9]*)
            TRIGGER=$((PIDSENTRY_TEST_ERROR - 1))
            ;;
    once)
            TRIGGER=once
            ;;
    *)
            echo "Unrecognised test specification: $PIDSENTRY_TEST_ERROR" >&2
            exit 1
            ;;
    esac
    unset PIDSENTRY_TEST_ERROR
fi

VALGRIND="valgrind"
VALGRIND="$VALGRIND --error-exitcode=128"
VALGRIND="$VALGRIND --leak-check=yes"
VALGRIND="$VALGRIND --suppressions=pidsentry.supp"

VALGRINDOPT="--log-file=errortest.log"

# Find the number of error injection points when running the test.
# Round up the result to determine the number of iterations to use
# when running the test.

RANGE=$(
    VALGRINDOPT="--log-file=/dev/null"
    pidsentrytest 2>&1 >/dev/null | tail -1)
RANGE=$(( (RANGE + 999) / 500 * 500 ))

if [ x"$TRIGGER" = x"once" ] ; then
    TRIGGER=$(( $(random) % RANGE ))
    RANGE=$(( TRIGGER + 1 ))
fi

while [ $TRIGGER -lt $RANGE ] ; do

    : $(( ++TRIGGER ))

    [ -z "$(ps -C pidsentry -o user=,ppid=,pid=,pgid=,command=)" ]

    export PIDSENTRY_TEST_ERROR="$TRIGGER"
    printf ""
    printf "%s\n" "PIDSENTRY_TEST_ERROR=$PIDSENTRY_TEST_ERROR # $RANGE"

    pidsentrytest >/dev/null || {
        RC=$?
        printf "Test exit code %s\n" "$RC"
        [ $RC -eq 1 ]   && continue # EXIT_FAILURE
        [ $RC -eq 134 ] && continue # SIGABRT
        [ $RC -eq 137 ] && continue # SIGKILL
        [ $RC -eq 143 ] && continue # SIGTERM
        exit 1
    }

    [ -z "$(ps -C pidsentry -o user=,ppid=,pid=,pgid=,command=)" ]
done
