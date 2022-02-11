#!/bin/sh

URL=https://to-be-filled.com/path

die() {
	echo "$@"
	exit
}

[ "$#" -ne 1 ] && die "Usage: $0 logfile"

grep -q Backtrace: $1 || die "No crash in log file"

CURL=`which curl`
WGET=`which wget`

[ -n "$CURL" -o -n "$WGET" ] || die "Curl or wget required"

BIN=`grep vnc $1 | tail -n1 | cut -d: -f2 | cut -d\( -f1`
[ -f $BIN ] || die "Can't locate binary"

#
# prep done, filter the log file
#

TMP=`mktemp`

LANG=C date >> $TMP
md5sum $BIN >> $TMP
$BIN -version 2>&1 | grep built >> $TMP
grep -A200 Backtrace: $1 >> $TMP

if [ -n "$CURL" ]; then
	echo curl --data-binary @"$TMP" "$URL"
else
	echo wget --post-file "$TMP" "$URL"
fi

rm $TMP
