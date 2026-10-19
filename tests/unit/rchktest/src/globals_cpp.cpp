/*
  C++ test cases for glcheck.

  See tests/unit/README.md for the meaning of the rchk: comments.
*/

#define R_NO_REMAP
#include <R.h>
#include <Rinternals.h>

class Holder {
public:
  SEXP x;
  int n;
};

static Holder gholder; /* rchk-missed: glcheck: structure with SEXP fields _ZL7gholder -- C++ classes are not checked since the LLVM 22 port (PR 46) */

extern "C" SEXP use_holder(SEXP x) {
  gholder.x = x;
  gholder.n = 1;
  return gholder.x;
}
