#! /bin/sh

rm -rf autom4te.cache
test -d ./config || mkdir ./config
mkdir -p m4 config/m4 config/aux
aclocal -I config
libtoolize --force --copy
autoheader
automake --foreign --add-missing --copy
autoreconf --force --install -I config -I m4
rm -rf autom4te.cache
