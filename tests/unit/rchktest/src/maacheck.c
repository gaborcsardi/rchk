/*
  Test cases for maacheck and ueacheck: calls with two or more unprotected
  arguments, where evaluating one may free the other.

  See tests/unit/README.md for the meaning of the rchk: comments.
*/

#include <R.h>
#include <Rinternals.h>

SEXP maa_lang(void) {
  return lang3(install("c"), ScalarInteger(1), ScalarInteger(2)); /* rchk: maacheck: Suspicious call \(two or more unprotected arguments\) to Rf_lang3 at maa_lang */ /* rchk: ueacheck: Suspicious call \(two or more unprotected arguments\) to Rf_lang3 at maa_lang */
}

SEXP ok_maa(void) {
  SEXP a = PROTECT(ScalarInteger(1));
  SEXP b = PROTECT(ScalarInteger(2));
  SEXP res = lang3(install("c"), a, b);
  UNPROTECT(2);
  return res;
}

SEXP uea_var(SEXP x) {
  SEXP pa = allocVector(INTSXP, 1);
  INTEGER(pa)[0] = 1;
  setAttrib(x, install("pid"), pa); /* rchk: ueacheck: Suspicious call \(two or more unprotected arguments\) to Rf_setAttrib at uea_var */ /* rchk: bcheck: unprotected variable pa while calling allocating function Rf_install */
  return x;
}

SEXP ok_uea(SEXP x) {
  SEXP sym = install("pid");
  SEXP pa = PROTECT(allocVector(INTSXP, 1));
  INTEGER(pa)[0] = 1;
  setAttrib(x, sym, pa);
  UNPROTECT(1);
  return x;
}
