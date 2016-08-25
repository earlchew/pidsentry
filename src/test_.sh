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
    set -- ${VALGRINDOPT+$VALGRINDOPT} "$@"
    set -- ${VALGRIND+   $VALGRIND}    "$@"
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