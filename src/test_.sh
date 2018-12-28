################################################################################
# Execute pidsentry taking RPATH into account
#
# Normalise the content of the environment, and handle the RPATH to
# find the location of the program interpreter if specified.

pidsentryexec()
{
    set -- ./pidsentry "$@"

    # If RPATH is configured, and the program interpreter is specified
    # using a relative path, use it to find the appropriate program
    # interpreter.
    #
    # Usually RPATH is set by the --with-test-environment configuration
    # option.

    if [ -n "${RPATH++}" ] ; then
        set -- "$(
            readelf -l ./pidsentry |
            grep -F 'Requesting program interpreter')" "$@"
        set -- "${1%]*}"  "${@:2}"
        set -- "${1#*: }" "${@:2}"
        if [ -z "${1##/*}" ] ; then
            shift
        else
            set -- "$(
                IFS=:
                for LDSO in $RPATH ; do
                    LDSO="$LDSO/$1"
                    ! [ -x "$LDSO" ] || { echo "$LDSO" ; exit 0 ; }
                done
            )" "${@:2}"
            [ -n "$1" ] || exit 1
            set -- "$1" --library-path "$RPATH" --inhibit-rpath "$2" "${@:2}"
        fi
    fi
    if [ -n "${VALGRIND++}" ] ; then
        set -- setarch "$(uname -m)" -R "$@" # Avoid mmap() collisions with vdso
        set -- ${VALGRINDOPT+$VALGRINDOPT} "$@"
        set -- ${VALGRIND+   $VALGRIND}    "$@"
    fi
    #set -- strace -o /tmp/valgrind.log "$@"
    set -- libtool --mode=execute      "$@"

    set -- PATH="$PATH"                  "$@"
    set -- ${USER+      USER="$USER"}    "$@"
    set -- ${LOGNAME+LOGNAME="$LOGNAME"} "$@"
    set -- ${HOME+      HOME="$HOME"}    "$@"
    set -- ${LANG+      LANG="$LANG"}    "$@"
    set -- ${SHELL+    SHELL="$SHELL"}   "$@"
    set -- ${TERM+      TERM="$TERM"}    "$@"
    exec env - "$@"
}

################################################################################
# Find lost pidsentry processes
#
# Scan the list of processes, and assuming that there should not be any
# more pidsentry processes running, any currently running will be orphans.

pidsentryorphans()
{
    ps -awwo user=,ppid=,pid=,pgid=,command= |
    awk '$1 == ENVIRON["USER"]' |
    {
        while read REPLY ; do
            [ -n "$REPLY" ] || continue
            [ -n "${REPLY##*pidsentry}" -a -n "${REPLY##*pidsentry *}" ] || {
                /bin/echo "$REPLY" >&2
                exit 1
            }
        done
        exit 0
    }
}

################################################################################
