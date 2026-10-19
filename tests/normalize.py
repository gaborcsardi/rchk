#!/usr/bin/env python3
"""
Normalize raw rchk tool outputs so that they can be compared between runs.

  - Paths inside the R source tree and the package build directories become
    relative, e.g. /rchk/trunk/src/main/eval.c -> src/main/eval.c and
    /rchk/trunk/packages/build/Xy12ab/zoo/src/coredata.c -> zoo/src/coredata.c
  - "Analyzed N functions, traversed N states." lines are dropped, the
    counts change with any change in the analysis.
  - Number suffixes that the linker adds to static functions with the same
    name are dropped, e.g. R_DispatchOrEvalSP.6648 -> R_DispatchOrEvalSP
  - Notes that the analysis gave up early ("... results will be incomplete")
    are dropped. They are printed to stderr, so they end up at different
    places in the output, and whether they appear at all can change between
    runs.
  - Lines are sorted, because rchk prints some results in an order that
    changes between runs. bcheck output is sorted by function, and the lines
    of each function are sorted. bcheck lines that do not belong to a
    function (e.g. "too many states") are listed first.

Usage: normalize.py <raw-dir> <results-dir>
"""

import os
import re
import sys

PATH_SUBS = [
    (re.compile(r'/rchk/trunk/packages/build/[^/\s]+/'), ''),
    (re.compile(r'/rchk/trunk/'), ''),
    (re.compile(r'(?<=[A-Za-z_])\.\d+\b'), ''),
]
DROP = re.compile(r'^\s*Analyzed \d+ functions, traversed \d+ states\.\s*$'
                  r'|results will be incomplete')


def sort_bcheck(lines):
    other = []
    functions = {}
    current = None
    for line in lines:
        if line.startswith('Function '):
            current = functions.setdefault(line, [])
        elif line.startswith((' ', '\t')) and current is not None:
            current.append(line)
        else:
            other.append(line)
            current = None
    out = sorted(other)
    for header in sorted(functions):
        if functions[header]:
            out += [header] + sorted(functions[header])
    return out


def normalize(text, tool=None):
    for pattern, repl in PATH_SUBS:
        text = pattern.sub(repl, text)
    lines = [l for l in text.splitlines() if l.strip() and not DROP.search(l)]
    lines = sort_bcheck(lines) if tool == 'bcheck' else sorted(lines)
    return '\n'.join(lines) + ('\n' if lines else '')


def main(raw, results):
    for root, _, files in os.walk(raw):
        for name in sorted(files):
            if name.startswith('install-'):
                continue
            src = os.path.join(root, name)
            dst = os.path.join(results, os.path.relpath(src, raw))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(src, encoding='utf-8', errors='replace') as f:
                text = f.read()
            with open(dst, 'w', encoding='utf-8') as f:
                f.write(normalize(text, tool=os.path.splitext(name)[1][1:]))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
