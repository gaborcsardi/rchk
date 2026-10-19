# rchk unit tests

`rchktest/` is a small R package with one C function per bug pattern that
rchk should find, and correct variants (`ok_*` functions) that rchk should
not report. Comments in the sources say what each tool should report:

```c
SEXP b = allocVector(REALSXP, 1); /* rchk: bcheck: unprotected variable a while calling allocating function Rf_allocVector */
```

The format is `rchk: <tool>: <regex>`. The test passes if some report of
`<tool>` matches `<regex>`. If the report has a location in the package
sources, it must also be on the line of the comment.

Two more forms record known problems, so that the tests pass but the
problem stays visible:

- `rchk-missed: <tool>: <regex> -- <reason>`: rchk should report this, but
  currently does not. If rchk starts reporting it, the test fails, so that
  the comment gets changed to `rchk:`.
- `rchk-false: <tool>: <regex> -- <reason>`: a known false alarm. Such a
  report is allowed.

Any other report about the package fails the test. So a new false alarm in
an `ok_*` function, or a bug report on the wrong line, is caught.

## Running

From the root of the repository, with Docker:

```sh
tests/unit/run.sh
```

This uses the same image as the other tests (see `tests/README.md`), so
after the first build it only rebuilds rchk. The tool outputs go to
`tests/out/unit/`, and `check.py` prints one line per annotation.

## Adding a test case

1. Add a function to one of the files in `rchktest/src/`, or a new file.
   Name correct variants `ok_*`.
2. Register it in `rchktest/src/init.c` if it is a `.Call` entry point, so
   that the compiler keeps it.
3. Add `rchk:` comments on the lines where rchk should report problems.
4. Run `tests/unit/run.sh`.
