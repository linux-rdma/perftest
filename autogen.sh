#! /bin/sh

test -d ./config || mkdir ./config
mkdir -p m4 config/m4 config/aux
aclocal -I config
libtoolize --force --copy
autoheader
automake --foreign --add-missing --copy
autoconf
