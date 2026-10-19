#! /bin/bash

# Builds the test image, runs the tests, and compares the results with
# tests/expected/<arch>. Run from the root of the rchk repository.
#
# Usage: tests/run.sh [outdir]
#
# Settings come from tests/config.sh and can be overridden with environment
# variables, e.g. UBUNTU_VERSION=22.04 LLVM_VERSION=14 tests/run.sh

set -eu

OUT=${1:-tests/out}
. tests/config.sh

ARCH=$(uname -m)
case $ARCH in arm64) ARCH=aarch64 ;; esac
IMAGE=rchk-tests:llvm$LLVM_VERSION

docker build -f tests/Dockerfile -t $IMAGE \
  --build-arg UBUNTU_VERSION=$UBUNTU_VERSION \
  --build-arg LLVM_VERSION=$LLVM_VERSION \
  --build-arg R_SVN_REVISION=$R_SVN_REVISION .

rm -rf $OUT
mkdir -p $OUT
docker run --rm --memory=$DOCKER_MEMORY --memory-swap=$DOCKER_MEMORY \
  -v "$(cd $OUT && pwd)":/out \
  -e TOOLS="$TOOLS" -e CRAN_SNAPSHOT="$CRAN_SNAPSHOT" \
  $IMAGE /rchk/tests/run-tests.sh /out

python3 tests/normalize.py $OUT/raw $OUT/results
tests/compare.sh tests/expected/$ARCH $OUT/results
