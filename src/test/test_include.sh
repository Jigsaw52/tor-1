#!/bin/sh

umask 077
set -e
set -x

# emulate realpath(), in case coreutils or equivalent is not installed.
abspath() {
    f="$*"
    if [ -d "$f" ]; then
        dir="$f"
        base=""
    else
        dir="$(dirname "$f")"
        base="/$(basename "$f")"
    fi
    dir="$(cd "$dir" && pwd)"
    echo "$dir$base"
}

UNAME_OS=$(uname -s | cut -d_ -f1)
if test "$UNAME_OS" = 'CYGWIN' || \
   test "$UNAME_OS" = 'MSYS' || \
   test "$UNAME_OS" = 'MINGW' || \
   test "$UNAME_OS" = 'MINGW32' || \
   test "$UNAME_OS" = 'MINGW64'; then
  if test "$APPVEYOR" = 'True'; then
    echo "This test is disabled on Windows CI, as it requires firewall exemptions. Skipping." >&2
    exit 77
  fi
fi

# find the tor binary
if [ $# -ge 1 ]; then
  TOR_BINARY="${1}"
  shift
else
  TOR_BINARY="${TESTING_TOR_BINARY:-./src/app/tor}"
fi

TOR_BINARY="$(abspath "$TOR_BINARY")"

echo "TOR BINARY IS ${TOR_BINARY}"

if "${TOR_BINARY}" --list-modules | grep -q "relay: no"; then
  echo "This test requires the relay module. Skipping." >&2
  exit 77
fi

tmpdir=
clean () {
  if [ -n "$tmpdir" ] && [ -d "$tmpdir" ]; then
    rm -rf "$tmpdir"
  fi
}

trap clean EXIT HUP INT TERM

tmpdir="$(mktemp -d -t tor_include_test.XXXXXX)"
if [ -z "$tmpdir" ]; then
  echo >&2 mktemp failed
  exit 2
elif [ ! -d "$tmpdir" ]; then
  echo >&2 mktemp failed to make a directory
  exit 3
fi

datadir="$tmpdir/data"
mkdir "$datadir"

configdir="$tmpdir/config"
mkdir "$configdir"

# translate paths to windows format
if test "$UNAME_OS" = 'CYGWIN' || \
   test "$UNAME_OS" = 'MSYS' || \
   test "$UNAME_OS" = 'MINGW' || \
   test "$UNAME_OS" = 'MINGW32' || \
   test "$UNAME_OS" = 'MINGW64'; then
    datadir=`cygpath --windows "$datadir"`
    configdir=`cygpath --windows "$configdir"`
fi

# create test folder structure in configdir
torrcd="$configdir/torrc.d"
mkdir "$torrcd"
mkdir "$torrcd/folder"
echo "RecommendedVersions 1" > "$torrcd/01_one.conf"
echo "RecommendedVersions 2" > "$torrcd/02_two.conf"
echo "RecommendedVersions 3" > "$torrcd/aa_three.conf"
echo "RecommendedVersions 42" > "$torrcd/.hidden.conf"
echo "RecommendedVersions 6" > "$torrcd/foo"
touch "$torrcd/empty.conf"
echo "# comment" > "$torrcd/comment.conf"
echo "RecommendedVersions 4" > "$torrcd/folder/04_four.conf"
echo "RecommendedVersions 5" > "$torrcd/folder/05_five.conf"
torrc="$configdir/torrc"
echo "Sandbox 1" > "$torrc"
echo "%include $torrcd/*.conf" >> "$torrc"
echo "%include $torrcd/f*" >> "$torrc"
echo "%include $torrcd/*/*" >> "$torrc"
echo "%include $torrcd/empty.conf" >> "$torrc"
echo "%include $torrcd/comment.conf" >> "$torrc"

"${PYTHON:-python}" "${abs_top_srcdir:-.}/src/test/test_include.py" "${TOR_BINARY}" "$datadir" "$configdir"

exit $?
