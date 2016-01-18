# Copyright (c) 2016, Earl Chew
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the names of the authors of source code nor the names
#       of the contributors to the source code may be used to endorse or
#       promote products derived from this software without specific
#       prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL EARL CHEW BE LIABLE FOR ANY
# DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

set -e

#SRC="*.h"
SRC="*.c"

"$@" || exit $?

for f in $SRC ; do
  awk '/^#include/ { ++C; print $0 " //@@ " C; next } { print $0 }' $f > $f.new
  mv $f.new $f
done

# Check each file by removing the header and testing if the source code
# still compiles. If the source code compiles without the header, remove
# it, but always keep the first included header of source files.

for f in $SRC ; do
    echo "Checking .. $f"
    N=$(grep '//@' $f | wc -l)
    X=0
    while [ $X -lt $N ] ; do
        : $(( ++X ))
        [ -z "${f##*.h}" -o $X -ne 1 ] && K=$? || K=$?
        F=$(grep -e "#include.*//@@ $X\$" $f | sed -e 's, //@@.*,,')
        sed -i -e "s,^\(#include.*//@@ $X\$\),//\1," $f
        if "$@" 2>/dev/null >&2 && ( exit $K ) ; then
            echo "  $F .. removed"
            sed -i -e "/^\\/\\/#include.* \\/\\/@@ $X\$/d" $f
        else
            echo "  $F .. ok"
            sed -i -e "s,^//\(#include.*\) //@@ $X\$,\1," $f
        fi
    done
done
