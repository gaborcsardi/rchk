#! /bin/bash

# Runs the rchk unit tests only: builds the test image (R is cached after
# the first build), checks the rchktest package and compares the reports
# with the annotations in its sources. Run from the root of the repository.
#
# Usage: tests/unit/run.sh [outdir]

set -eu

OUT=${1:-tests/out/unit}
. tests/config.sh
IMAGE=rchk-tests:llvm$LLVM_VERSION

docker build -f tests/Dockerfile -t $IMAGE \
  --build-arg UBUNTU_VERSION=$UBUNTU_VERSION \
  --build-arg LLVM_VERSION=$LLVM_VERSION \
  --build-arg R_SVN_REVISION=$R_SVN_REVISION .

rm -rf $OUT
mkdir -p $OUT
docker run --rm --memory=$DOCKER_MEMORY --memory-swap=$DOCKER_MEMORY \
  -v "$(cd $OUT && pwd)":/out \
  -e CRAN_SNAPSHOT="$CRAN_SNAPSHOT" \
  $IMAGE /rchk/tests/unit/run-unit.sh /out

python3 tests/unit/check.py tests/unit/rchktest/src $OUT
