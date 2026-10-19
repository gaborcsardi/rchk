#! /bin/bash

# Installs the rchktest package and runs the rchk tools on it, inside the
# test image. Tool outputs go to <outdir>/<tool>/.
#
# Usage: run-unit.sh <outdir>

set -u

OUT=${1:-/out/unit}
UNIT_TOOLS=${UNIT_TOOLS:-"bcheck maacheck ueacheck fficheck glcheck errcheck"}
. $RCHK/tests/config.sh

cd $RCHK/trunk
. $RCHK/scripts/cmpconfig.inc

mkdir -p $OUT
BUILD=$(mktemp -d)
(cd $BUILD && R CMD build --no-build-vignettes $RCHK/tests/unit/rchktest) > $OUT/install.log 2>&1
echo "
  source('$RCHK/scripts/utils.r')
  install_package_libs('$(ls $BUILD/rchktest_*.tar.gz)',
                       contriburl = '$CRAN_SNAPSHOT/src/contrib')
" | R --vanilla --no-echo >> $OUT/install.log 2>&1

if [ ! -d $R_LIBSONLY/rchktest/libs ] ; then
  echo "Installing rchktest failed:" >&2
  tail -30 $OUT/install.log >&2
  exit 1
fi

for T in $UNIT_TOOLS ; do
  echo "Running $T on rchktest"
  $RCHK/scripts/check_package.sh rchktest $T
  mkdir -p $OUT/$T
  find $R_LIBSONLY/rchktest/libs -name "*.$T" -exec cp {} $OUT/$T/ \;
done
