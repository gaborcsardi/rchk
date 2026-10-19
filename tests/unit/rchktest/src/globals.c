/*
  Test cases for glcheck (global structures holding SEXPs, which the GC does
  not know about) and errcheck (error functions not marked NORETURN).

  See tests/unit/README.md for the meaning of the rchk: comments.
*/

#include <R.h>
#include <Rinternals.h>

static SEXP gcache; /* rchk: glcheck: non-symbol SEXP global variable gcache */

static struct {
  SEXP cache;
  int n;
} gstate; /* rchk: glcheck: structure with SEXP fields gstate */

static union {
  SEXP s;
  double d;
} gunion; /* rchk-missed: glcheck: structure with SEXP fields gunion -- unions are not checked since the LLVM 22 port (PR 46) */

static struct {
  int n;
  double d;
} gplain;

SEXP use_globals(SEXP x) {
  gcache = x;
  gstate.cache = x;
  gstate.n = 1;
  gunion.s = x;
  gplain.n = 1;
  return gstate.cache;
}

void my_stop(const char *msg) {
  error("rchktest: %s", msg); /* rchk: errcheck: UNMARKED error function my_stop */
}

NORET void ok_my_stop(const char *msg) {
  error("rchktest: %s", msg);
}

SEXP use_stops(SEXP x) {
  if (TYPEOF(x) == NILSXP) my_stop("NULL");
  if (TYPEOF(x) == ENVSXP) ok_my_stop("environment");
  return x;
}
