#!/usr/bin/env python3
"""
Checks rchk outputs for the rchktest package against the annotations in
its sources. See tests/unit/README.md.

Usage: check.py <package-src-dir> <outputs-dir>

<outputs-dir> has one <tool>/ folder per tool, with the tool outputs for
the package (e.g. bcheck/rchktest.so.bcheck).
"""

import os
import re
import sys

ANNOTATION = re.compile(
    r'rchk(-missed|-false)?:\s*(\w+):\s*(.*?)\s*(?:--.*?)?\s*\*/')
LOCATION = re.compile(r'/rchktest/src/([\w.]+):(\d+)')
PKG_PATH = '/rchktest/src/'


class Annotation:
    def __init__(self, kind, tool, regex, file, line):
        self.kind = kind          # 'expected', 'missed' or 'false'
        self.tool = tool
        self.regex = regex
        self.pattern = re.compile(regex)
        self.file = file
        self.line = line
        self.matches = []

    def where(self):
        return f'{self.file}:{self.line}'

    def matches_report(self, report):
        if not self.pattern.search(report):
            return False
        loc = LOCATION.search(report)
        if loc and (loc.group(1), int(loc.group(2))) != (self.file, self.line):
            return False
        return True


def read_annotations(srcdir):
    annotations = []
    words = set()
    for name in sorted(os.listdir(srcdir)):
        if not name.endswith(('.c', '.cpp')):
            continue
        with open(os.path.join(srcdir, name)) as f:
            for lineno, text in enumerate(f, 1):
                words.update(re.findall(r'\w+', text))
                for m in ANNOTATION.finditer(text):
                    kind = {None: 'expected', '-missed': 'missed',
                            '-false': 'false'}[m.group(1)]
                    annotations.append(
                        Annotation(kind, m.group(2), m.group(3), name, lineno))
    return annotations, words


def is_package_name(name, words):
    # also accept mangled C++ names of internal globals, e.g. _ZL7gholder
    return name in words or re.sub(r'^_ZL\d+', '', name) in words


def read_reports(tool, path, words):
    """Returns the lines of a tool output that report problems in the package."""
    reports = []
    function = None
    with open(path, errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if tool == 'bcheck':
                m = re.match(r'^Function (\S+)', line)
                if m:
                    function = m.group(1)
                elif line.startswith(' ') and PKG_PATH in line:
                    reports.append(f'Function {function}: {line.strip()}')
            elif tool in ('maacheck', 'ueacheck'):
                if line.startswith('WARNING') and PKG_PATH in line:
                    reports.append(line)
            elif tool == 'errcheck':
                if not line.startswith('Marked') and PKG_PATH in line:
                    reports.append(line)
            elif tool == 'fficheck':
                if line.startswith('ERROR'):
                    reports.append(line)
            elif tool == 'glcheck':
                m = re.match(r'^(structure with SEXP fields|non-symbol SEXP global variable) (\S+)', line)
                if m and is_package_name(m.group(2), words):
                    reports.append(line)
            else:
                if PKG_PATH in line:
                    reports.append(line)
    return reports


def main(srcdir, outdir):
    annotations, words = read_annotations(srcdir)
    tools = sorted({a.tool for a in annotations})
    failures = []
    unexpected = []

    for tool in tools:
        tooldir = os.path.join(outdir, tool)
        files = sorted(os.listdir(tooldir)) if os.path.isdir(tooldir) else []
        if not files:
            failures.append(f'{tool}: no output in {tooldir}')
            continue
        reports = []
        for name in files:
            reports += read_reports(tool, os.path.join(tooldir, name), words)
        tool_annotations = [a for a in annotations if a.tool == tool]
        for report in reports:
            hits = [a for a in tool_annotations if a.matches_report(report)]
            for a in hits:
                a.matches.append(report)
            if not hits:
                unexpected.append(f'{tool}: {report}')

    print('rchk unit tests\n')
    for a in annotations:
        if a.kind == 'expected':
            ok = bool(a.matches)
            status = 'ok' if ok else 'NOT FOUND'
        elif a.kind == 'missed':
            ok = not a.matches
            status = 'known miss' if ok else 'NOW FOUND (change rchk-missed to rchk)'
        else:
            ok = True
            status = 'known false alarm' if a.matches else 'known false alarm (not reported)'
        print(f'  {status:<18} {a.where():<22} {a.tool}: {a.regex}')
        if not ok:
            failures.append(f'{a.where()} {a.tool}: {status}: {a.regex}')

    if unexpected:
        print('\nUnexpected reports (false alarms, or missing annotations):')
        for u in unexpected:
            print(f'  {u}')
    failures += unexpected

    if failures:
        print(f'\nFAILED: {len(failures)} problem(s)')
        return 1
    print('\nAll rchk unit tests passed.')
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
