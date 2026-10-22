#!/usr/bin/env python3
"""Parse a compiler, clang-tidy or cppcheck log and fail on anything not accepted.

usage: check.py compiler|tidy|cppcheck LOG ACCEPTED ROOT

accepted.txt line: "<file>:<check> [max=N] | <reason>" (N defaults to 1: a second finding of the same check in the same file is
a new finding). Compiler warnings inside the repo can never be accepted; only third-party ones (absolute path outside ROOT).
Any line that says "warning:" or "error:" but does not parse as a diagnostic fails too, so a format surprise cannot hide a finding.
"""
import re, sys

DIAG = re.compile(r'^(?P<f>[^\s:][^:]*):(?P<l>\d+):(?P<c>\d+): (?:fatal )?(?P<sev>warning|error|performance|portability|style|information)\b.*?(?:\[(?P<id>[^\]\s]+)\])?\s*$')
SAYS = re.compile(r'(?:^|[\s:])(?:warning|error):')
PROCESSED = {
    'compiler': re.compile(r'Building\s+(?:C|CXX|OBJC|OBJCXX)\s+object\b', re.I),
    'tidy': re.compile(r'\bProcessing\s+(?:file\s+)?(.+?)\.?$', re.I),
    'cppcheck': re.compile(r'\bChecking\s+(.+?)(?:\.\.\.)?$', re.I),
}
SOURCE_SUFFIX = re.compile(r'\.(?:c|cc|cpp|cxx|c\+\+|h|hh|hpp|hxx|h\+\+)$', re.I)
TIDY_COMMAND_SOURCE = re.compile(r'\bclang-tidy\b.*?([^\s]+?\.(?:c|cc|cpp|cxx|c\+\+|h|hh|hpp|hxx|h\+\+))(?:\s|$)', re.I)


def norm(line, root):
    for p in (root.rstrip('/') + '/', '/work/'):
        if line.startswith(p):
            return line[len(p):]
    return line


def main(kind, log, acc, root):
    if kind not in PROCESSED:
        print('unknown analysis kind: ' + kind)
        return 2
    accepted = {}
    for line in open(acc):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        head = line.split('|')[0].strip()
        m = re.match(r'^(.*?)(?:\s+max=(\d+))?$', head)
        accepted[m.group(1)] = int(m.group(2) or 1)
    found, unparsed, processed = {}, [], set()
    for raw in open(log, errors='replace'):
        raw = raw.rstrip('\n')
        line = norm(raw, root)
        pm = PROCESSED[kind].search(line)
        if kind == 'tidy' and not pm:
            pm = TIDY_COMMAND_SOURCE.search(line)
        if pm:
            candidate = pm.group(1).strip().removesuffix(' ...').rstrip('.') if pm.lastindex else ''
            if kind == 'compiler':
                processed.add(raw)
            elif SOURCE_SUFFIX.search(candidate):
                processed.add(candidate)
        m = DIAG.match(line)
        if not m:
            if SAYS.search(line):
                unparsed.append(raw)
            continue
        f, cid = m.group('f'), m.group('id') or '(no-id)'
        if kind == 'compiler' and not f.startswith('/'):
            accepted.pop(f + ':' + cid, None)   # in-repo compiler warnings are never acceptable
            found.setdefault((f, cid, 'never'), set()).add((m.group('l'), m.group('c')))
            continue
        for c in cid.split(','):
            found.setdefault((f, c, ''), set()).add((m.group('l'), m.group('c')))
    bad = []
    for (f, c, never), sites in sorted(found.items()):
        n = len(sites)
        key = f + ':' + c
        if never:
            bad.append('%s (%d site(s)): in-repo compiler warning' % (key, n))
        elif key not in accepted:
            bad.append('%s (%d site(s)): not in accepted.txt' % (key, n))
        elif n > accepted[key]:
            bad.append('%s: %d site(s) but accepted.txt allows max=%d' % (key, n, accepted[key]))
    for u in unparsed:
        bad.append('UNPARSED diagnostic line: ' + u)
    if not processed:
        bad.append('no source files processed (no verifiable %s analysis evidence)' % kind)
    print('\n'.join(bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:5]))
