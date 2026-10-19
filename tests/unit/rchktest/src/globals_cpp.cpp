/*
  C++ test cases for glcheck.

  See tests/unit/README.md for the meaning of the rchk: comments.
*/

#define R_NO_REMAP
#include <R.h>
#include <Rinternals.h>

// debug info of a class also lists its methods and static members
class Holder {
public:
  SEXP x;
  int n;
  static SEXP shared;
  SEXP get() { return x; }
};

SEXP Holder::shared = NULL; /* rchk: glcheck: non-symbol SEXP global variable _ZN6Holder6sharedE */

static Holder gholder; /* rchk: glcheck: structure with SEXP fields _ZL7gholder */

// the only SEXP is a static member, which is not stored in the object
class StaticHolder {
public:
  int n;
  static SEXP shared;
  int get() { return n; }
};

SEXP StaticHolder::shared = NULL; /* rchk: glcheck: non-symbol SEXP global variable _ZN12StaticHolder6sharedE */

static StaticHolder gstatic;

extern "C" SEXP use_holder(SEXP x) {
  gholder.x = x;
  gholder.n = 1;
  Holder::shared = x;
  return gholder.get();
}

extern "C" SEXP use_static_holder(SEXP x) {
  gstatic.n = gstatic.get() + 1;
  StaticHolder::shared = x;
  return StaticHolder::shared;
}
