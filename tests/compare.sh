#! /bin/bash

# Compares normalized test results with the expected ones.
#
# Usage: tests/compare.sh <expected-dir> <results-dir>
#
# Exits with status 1 if they differ. To accept the new results, copy
# <results-dir> over <expected-dir> and commit it.

EXPECTED=$1
RESULTS=$2

if [ ! -d "$EXPECTED" ] ; then
  echo "No expected results in $EXPECTED. To create them:" >&2
  echo "  mkdir -p $(dirname $EXPECTED) && cp -R $RESULTS $EXPECTED" >&2
  exit 1
fi

if diff -ru "$EXPECTED" "$RESULTS" ; then
  echo "Results match $EXPECTED."
else
  echo
  echo "Results differ from $EXPECTED (see the diff above)." >&2
  exit 1
fi
