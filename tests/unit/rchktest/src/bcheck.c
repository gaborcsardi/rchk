/*
  Test cases for bcheck: unprotected objects at allocating calls ([UP]) and
  protection stack balance ([PB]).

  See tests/unit/README.md for the meaning of the rchk: comments.
*/

#include <R.h>
#include <Rinternals.h>

/* --- unprotected objects ------------------------------------------------ */

SEXP up_simple(void) {
  SEXP a = allocVector(REALSXP, 1);
  SEXP b = allocVector(REALSXP, 1); /* rchk: bcheck: unprotected variable a while calling allocating function Rf_allocVector */
  REAL(a)[0] = 1;
  REAL(b)[0] = 2;
  return a;
}

SEXP up_after_unprotect(void) {
  SEXP a = PROTECT(allocVector(REALSXP, 1));
  UNPROTECT(1);
  SEXP b = allocVector(REALSXP, 1); /* rchk: bcheck: unprotected variable a while calling allocating function Rf_allocVector */
  REAL(a)[0] = REAL(b)[0] = 1;
  return a;
}

SEXP up_getattrib_names(SEXP x) {
  SEXP nms = getAttrib(x, R_NamesSymbol);
  SEXP res = PROTECT(allocVector(STRSXP, 1)); /* rchk: bcheck: unprotected variable nms while calling allocating function Rf_allocVector */
  SET_STRING_ELT(res, 0, STRING_ELT(nms, 0));
  UNPROTECT(1);
  return res;
}

/* uses x after allocating, so x must be protected by the caller */
static SEXP make_pair(SEXP x) {
  SEXP res = PROTECT(allocVector(VECSXP, 2));
  SET_VECTOR_ELT(res, 0, x);
  SET_VECTOR_ELT(res, 1, x);
  UNPROTECT(1);
  return res;
}

SEXP up_fresh_arg(void) {
  SEXP a = allocVector(REALSXP, 1);
  REAL(a)[0] = 1;
  return make_pair(a); /* rchk: bcheck: calling allocating function make_pair\(V\) with a fresh pointer \(a <arg 1>\) */
}

SEXP up_allocating_arg(void) {
  return make_pair(allocVector(REALSXP, 1)); /* rchk: bcheck: calling allocating function make_pair\(V\) with argument allocated using Rf_allocVector */
}

SEXP ok_protected(void) {
  SEXP a = PROTECT(allocVector(REALSXP, 1));
  SEXP b = PROTECT(allocVector(REALSXP, 1));
  REAL(a)[0] = 1;
  REAL(b)[0] = 2;
  UNPROTECT(2);
  return a;
}

SEXP ok_setter(void) {
  SEXP klass = PROTECT(allocVector(STRSXP, 2));
  SET_STRING_ELT(klass, 0, mkChar("a"));
  SET_STRING_ELT(klass, 1, mkChar("b"));
  UNPROTECT(1);
  return klass;
}

SEXP ok_data_ptr(void) {
  SEXP a = PROTECT(allocVector(REALSXP, 2));
  double *p = REAL(a);
  SEXP b = PROTECT(allocVector(REALSXP, 1));
  p[0] = REAL(b)[0] = 1;
  UNPROTECT(2);
  return a;
}

/* --- SEXP declared in other ways ---------------------------------------- */

typedef SEXP myobj_t;

SEXP up_typedef(void) {
  myobj_t a = allocVector(REALSXP, 1);
  SEXP b = allocVector(REALSXP, 1); /* rchk: bcheck: unprotected variable a while calling allocating function Rf_allocVector */
  REAL(a)[0] = REAL(b)[0] = 1;
  return a;
}

SEXP up_const(void) {
  const SEXP a = allocVector(REALSXP, 1);
  SEXP b = allocVector(REALSXP, 1); /* rchk: bcheck: unprotected variable a while calling allocating function Rf_allocVector */
  REAL(a)[0] = REAL(b)[0] = 1;
  return a;
}

SEXP up_struct_sexprec(void) {
  struct SEXPREC *a = allocVector(REALSXP, 1);
  SEXP b = allocVector(REALSXP, 1); /* rchk: bcheck: unprotected variable a while calling allocating function Rf_allocVector */
  REAL(a)[0] = REAL(b)[0] = 1;
  return a;
}

/* p is not a SEXP variable, so it is not checked; a is protected */
SEXP ok_void_ptr(void) {
  SEXP a = allocVector(REALSXP, 1);
  void *p = a;
  PROTECT(a);
  SEXP b = PROTECT(allocVector(REALSXP, 1));
  REAL((SEXP) p)[0] = REAL(b)[0] = 1;
  UNPROTECT(2);
  return a;
}

/* --- protection stack balance ------------------------------------------ */

SEXP pb_early_return(SEXP x) {
  SEXP a = PROTECT(coerceVector(x, REALSXP));
  if (XLENGTH(a) == 0) return allocVector(REALSXP, 0);
  UNPROTECT(1);
  return a;
} /* rchk: bcheck: has possible protection stack imbalance */

SEXP pb_negative(void) {
  SEXP a = PROTECT(allocVector(REALSXP, 1));
  REAL(a)[0] = 1;
  UNPROTECT(2); /* rchk: bcheck: has negative depth */ /* rchk: bcheck: attempt to unprotect more items \(2\) than protected \(1\) */
  return a; /* rchk: bcheck: has possible protection stack imbalance */
}

SEXP ok_counter(SEXP x) {
  int nprotect = 0;
  SEXP a = x;
  if (TYPEOF(x) != REALSXP) {
    a = PROTECT(coerceVector(x, REALSXP));
    nprotect++;
  }
  SEXP res = PROTECT(allocVector(REALSXP, 1));
  nprotect++;
  REAL(res)[0] = REAL(a)[0];
  UNPROTECT(nprotect);
  return res;
}
