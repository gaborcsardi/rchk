#! /bin/bash

# Runs the rchk tools on R and on the test packages, inside the test image.
# Raw tool outputs go to <outdir>/raw/<tool>/, run times to <outdir>/timings.tsv.
#
# Usage: run-tests.sh <outdir>

set -u

OUT=${1:-/out}
. $RCHK/tests/config.sh

cd $RCHK/trunk
. $RCHK/scripts/cmpconfig.inc

mkdir -p $OUT/raw
echo -e "target\ttool\tseconds\tmax_rss_kb\texit" > $OUT/timings.tsv

run_timed() {
  # run_timed <target> <tool> <command...>
  # max_rss_kb is the largest of the command and its children
  local target=$1 tool=$2 status
  shift 2
  /usr/bin/time -o /tmp/time.out -f "%e\t%M" "$@"
  status=$?
  echo -e "$target\t$tool\t$(tail -1 /tmp/time.out)\t$status" >> $OUT/timings.tsv
}

# R itself
for T in $TOOLS ; do
  mkdir -p $OUT/raw/$T/R
  echo "Running $T on R"
  run_timed R $T sh -c "$RCHK/src/$T ./src/main/R.bin.bc > $OUT/raw/$T/R/R.bin.$T 2>&1"
done

# test packages
grep -v '^#' $RCHK/tests/packages.txt | while read PKG VERSION ; do
  echo "Installing $PKG $VERSION"
  echo "
    source('$RCHK/scripts/utils.r')
    curl <- '$CRAN_SNAPSHOT/src/contrib'
    ap <- available.packages(curl)
    if (!'$PKG' %in% rownames(ap) || ap['$PKG', 'Version'] != '$VERSION') {
      stop('$PKG $VERSION is not in the CRAN snapshot')
    }
    install_package_libs('$PKG', contriburl = curl)
  " | R --vanilla --no-echo > $OUT/raw/install-$PKG.log 2>&1
  if [ ! -d $R_LIBSONLY/$PKG/libs ] ; then
    echo "  FAILED, see raw/install-$PKG.log"
    echo -e "$PKG\tinstall\t0\t0\t1" >> $OUT/timings.tsv
    continue
  fi
  for T in $TOOLS ; do
    echo "Running $T on $PKG"
    run_timed $PKG $T $RCHK/scripts/check_package.sh $PKG $T
    mkdir -p $OUT/raw/$T/$PKG
    find $R_LIBSONLY/$PKG/libs -name "*.$T" -exec cp {} $OUT/raw/$T/$PKG/ \;
  done
done

echo "Done, results are in $OUT/raw"
