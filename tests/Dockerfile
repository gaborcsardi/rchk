#
# Image for the rchk regression tests, see tests/README.md.
# Build from the root of the rchk repository:
#   docker build -f tests/Dockerfile -t rchk-tests .
#

ARG UBUNTU_VERSION=26.04
FROM ubuntu:${UBUNTU_VERSION}

ARG LLVM_VERSION=21
ARG R_SVN_REVISION=90708
ARG DEBIAN_FRONTEND=noninteractive

ENV TZ=UTC

# LLVM, R build dependencies, wllvm.
# Ubuntu 24.04 and later use deb822 sources and do not allow a plain
# 'pip install' into the system Python.
RUN if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then \
      sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources; \
    else \
      sed -i 's/^# deb-src/deb-src/' /etc/apt/sources.list; \
    fi && \
  apt-get update && \
  apt-get install -yq --no-install-recommends \
    make g++ clang-${LLVM_VERSION} llvm-${LLVM_VERSION}-dev \
    libc++-${LLVM_VERSION}-dev libc++abi-${LLVM_VERSION}-dev \
    python3-pip subversion file ca-certificates time && \
  apt-get build-dep -yq r-base && \
  (pip3 install wllvm || pip3 install --break-system-packages wllvm) && \
  rm -rf /var/lib/apt/lists/*

ENV LLVM=/usr/lib/llvm-${LLVM_VERSION} \
  WLLVM=/usr/local/bin \
  RCHK=/rchk
ENV PATH=/rchk/trunk/bin:$LLVM/bin:$PATH

# Build R in its source tree with wllvm (same flags as scripts/cmpconfig.inc).
# This comes before copying rchk, so that changes to rchk do not rebuild R.
RUN mkdir -p /rchk && cd /rchk && \
  svn checkout -q -r ${R_SVN_REVISION} https://svn.r-project.org/R/trunk trunk && \
  cd trunk && \
  export CPICFLAGS="-fPIC" CFLAGS="-Wall -g -O0 -fPIC" \
    CXXFLAGS="-Wall -g -O0 -fPIC -I/usr/include/libcxxabi" \
    CC=wllvm CXX="wllvm++ -stdlib=libc++" LLVM_COMPILER=clang \
    LD="clang++ -stdlib=libc++" && \
  { ./configure --without-recommended-packages --with-blas=no --with-lapack=no \
      --enable-R-static-lib > /rchk/r-configure.log 2>&1 || \
    { tail -50 /rchk/r-configure.log; exit 1; }; } && \
  { make -j$(nproc) > /rchk/r-make.log 2>&1 || { tail -50 /rchk/r-make.log; exit 1; }; } && \
  extract-bc src/main/R.bin

# Build rchk
COPY src /rchk/src
COPY scripts /rchk/scripts
COPY tests /rchk/tests
RUN cd /rchk/src && make -j$(nproc) LLVM=$LLVM CXX=g++ && \
  echo "# settings come from the container environment" > /rchk/scripts/config.inc

WORKDIR /rchk/trunk
