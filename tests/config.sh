# Settings for the rchk regression tests, sourced by the test scripts.

# Ubuntu release and LLVM version of the test image
UBUNTU_VERSION=${UBUNTU_VERSION:-26.04}
LLVM_VERSION=${LLVM_VERSION:-21}

# R-devel SVN revision to build and check
R_SVN_REVISION=${R_SVN_REVISION:-90708}

# Dated CRAN snapshot, the test packages and their dependencies come from here
CRAN_SNAPSHOT=${CRAN_SNAPSHOT:-https://packagemanager.posit.co/cran/2026-05-11}

# Memory limit of the test container, same as the GitHub runners (no swap)
DOCKER_MEMORY=${DOCKER_MEMORY:-16g}

# Tools to run on R itself and on each test package
TOOLS=${TOOLS:-"bcheck maacheck fficheck ueacheck alloccheck"}
