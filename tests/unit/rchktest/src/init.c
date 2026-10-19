/*
  Registration of the .Call routines, and test cases for fficheck.

  See tests/unit/README.md for the meaning of the rchk: comments.
*/

#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>

typedef SEXP myobj_t;

/* --- fficheck cases ----------------------------------------------------- */

int ffi_bad_return(SEXP x) { /* rchk: fficheck: function ffi_bad_return .*does not return SEXP */
  return TYPEOF(x);
}

SEXP ffi_bad_param(SEXP x, int y) { /* rchk: fficheck: function ffi_bad_param .*parameter 2 is not SEXP */
  return x;
}

SEXP ffi_bad_arity(SEXP x) { /* rchk: fficheck: function ffi_bad_arity .*has arity 1 but registered arity 2 */
  return x;
}

SEXP ok_ffi_typedef(myobj_t x) {
  return x;
}

SEXP ok_ffi_const(const SEXP x) {
  return x;
}

SEXP ok_ffi_sexprec(struct SEXPREC *x) {
  return x;
}

/* --- other test cases --------------------------------------------------- */

SEXP up_simple(void);
SEXP up_after_unprotect(void);
SEXP up_getattrib_names(SEXP x);
SEXP up_fresh_arg(void);
SEXP up_allocating_arg(void);
SEXP ok_protected(void);
SEXP ok_setter(void);
SEXP ok_data_ptr(void);
SEXP up_typedef(void);
SEXP up_const(void);
SEXP up_struct_sexprec(void);
SEXP ok_void_ptr(void);
SEXP pb_early_return(SEXP x);
SEXP pb_negative(void);
SEXP ok_counter(SEXP x);
SEXP maa_lang(void);
SEXP ok_maa(void);
SEXP uea_var(SEXP x);
SEXP ok_uea(SEXP x);
SEXP use_globals(SEXP x);
SEXP use_stops(SEXP x);
SEXP use_holder(SEXP x);
SEXP use_static_holder(SEXP x);

#define CALLDEF(name, n) { #name, (DL_FUNC) &name, n }

static const R_CallMethodDef callMethods[] = {
  CALLDEF(ffi_bad_return, 1),
  CALLDEF(ffi_bad_param, 2),
  CALLDEF(ffi_bad_arity, 2),
  CALLDEF(ok_ffi_typedef, 1),
  CALLDEF(ok_ffi_const, 1),
  CALLDEF(ok_ffi_sexprec, 1),
  CALLDEF(up_simple, 0),
  CALLDEF(up_after_unprotect, 0),
  CALLDEF(up_getattrib_names, 1),
  CALLDEF(up_fresh_arg, 0),
  CALLDEF(up_allocating_arg, 0),
  CALLDEF(ok_protected, 0),
  CALLDEF(ok_setter, 0),
  CALLDEF(ok_data_ptr, 0),
  CALLDEF(up_typedef, 0),
  CALLDEF(up_const, 0),
  CALLDEF(up_struct_sexprec, 0),
  CALLDEF(ok_void_ptr, 0),
  CALLDEF(pb_early_return, 1),
  CALLDEF(pb_negative, 0),
  CALLDEF(ok_counter, 1),
  CALLDEF(maa_lang, 0),
  CALLDEF(ok_maa, 0),
  CALLDEF(uea_var, 1),
  CALLDEF(ok_uea, 1),
  CALLDEF(use_globals, 1),
  CALLDEF(use_stops, 1),
  CALLDEF(use_holder, 1),
  CALLDEF(use_static_holder, 1),
  { NULL, NULL, 0 }
};

void R_init_rchktest(DllInfo *dll) {
  R_registerRoutines(dll, NULL, callMethods, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
}
