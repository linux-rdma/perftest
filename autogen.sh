#! /bin/sh

test -d ./config || mkdir ./config
mkdir -p m4 config/m4 config/aux
libtoolize --force --copy
aclocal -I config
autoheader
automake --foreign --add-missing --copy
autoconf
