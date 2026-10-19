# rchk regression tests

These tests run rchk on a fixed version of R and on a fixed set of CRAN
packages, and compare the outputs with results stored in
`tests/expected/<arch>/`. A change to rchk that changes what it reports shows
up as a diff.

Everything is pinned, so a run can be repeated later:

- R-devel at a fixed SVN revision (`R_SVN_REVISION` in `config.sh`), built
  without recommended packages.
- CRAN packages from a dated snapshot of Posit Package Manager
  (`CRAN_SNAPSHOT` in `config.sh`). The packages and versions are listed in
  `packages.txt`; their dependencies come from the same snapshot.
- Ubuntu release and LLVM version (`UBUNTU_VERSION`, `LLVM_VERSION`).

The packages are a few of the 30 most downloaded CRAN packages with only C
code from <https://github.com/kvapca/rchk-test>: some with rchk reports,
and some with a lot of C code and no reports.

There are also unit tests: a small package with one function per bug pattern,
in `tests/unit/`. See `tests/unit/README.md`.

## Files

- `config.sh`: settings. Each one can be overridden with an environment
  variable.
- `packages.txt`: test packages and their versions.
- `Dockerfile`: test image. Builds R in its source tree with wllvm, then
  builds rchk from this repository.
- `run-tests.sh`: runs inside the image. Runs the tools on R, then installs
  each test package and runs the tools on it. Writes raw outputs to
  `<out>/raw/<tool>/<target>/` and run times to `<out>/timings.tsv`.
- `normalize.py`: makes paths relative and drops lines that change with any
  change to the analysis (state counts), writing `<out>/results/`.
- `compare.sh`: diffs the results with the expected ones.
- `run.sh`: does all of the above on your machine.
- `expected/<arch>/`: expected results, one set per CPU architecture
  (`x86_64`, `aarch64`). Results differ between architectures, because
  clang passes some structs differently.

## Running locally

From the root of the repository, with Docker:

```sh
tests/run.sh
```

Results go to `tests/out/`. On Apple Silicon this runs the `aarch64` tests.
To run the `x86_64` tests there, use a native x86_64 machine; emulation is
very slow.

To try another setup, override the settings, e.g. for the LLVM 14 version of
rchk (from the `llvm14` branch):

```sh
UBUNTU_VERSION=22.04 LLVM_VERSION=14 tests/run.sh
```

## Updating the expected results

When a change to rchk changes the results on purpose, replace the expected
results with the new ones and commit them together with the change:

```sh
rm -rf tests/expected/aarch64
cp -R tests/out/results tests/expected/aarch64
```

For the other architecture, download the `rchk-results-<arch>` artifact of
the CI run and copy its `results` folder the same way.

## CI

`.github/workflows/rchk-tests.yml` runs the tests on every push and pull
request, on `x86_64` and `aarch64` GitHub runners. It fails if the results
differ from `tests/expected/<arch>/`, and uploads the raw and normalized
results as an artifact.
