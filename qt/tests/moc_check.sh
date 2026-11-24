#!/bin/sh
# Gate for moc, the meta-object compiler.
#
# moc parses the Q_OBJECT headers itself and is much less tolerant than the C++
# compiler. On Symbian (Qt 4.7.4) a declaration it cannot classify inside a
# `signals:` block aborts the whole build with
#
#     whatsapp_direct.h:155: Error: Not a signal or slot declaration
#
# while g++/clang compile the same header happily — so the host builds never
# see it. This script catches the two shapes that are known to trigger it:
#
#   1. a type declaration (struct/class/enum/typedef/using) inside signals:
#      or a slots: block, and
#   2. a data member inside signals: (a statement without a parameter list).
#
# If a `moc` binary is on PATH it is also run for real, which is the only
# complete check; otherwise the heuristic above runs alone.
#
# Usage: ./qt/tests/moc_check.sh
set -u
cd "$(dirname "$0")/../.." || exit 2
fail=0

echo "== declarations that moc cannot parse"
python3 - <<'PY' || fail=1
import glob, re, sys

headers = sorted(glob.glob('qt/src/**/*.h', recursive=True))
bad = 0
for path in headers:
    text = open(path).read()
    if 'Q_OBJECT' not in text and 'Q_GADGET' not in text:
        continue
    section = None          # None | 'meta' (signals/slots)
    statement = ''
    lineno = 0
    for i, line in enumerate(text.split('\n'), start=1):
        stripped = line.strip()
        if re.match(r'^(signals|Q_SIGNALS|slots|Q_SLOTS|public\s+slots|private\s+slots'
                    r'|protected\s+slots)\s*:', stripped):
            section, statement = 'meta', ''
            continue
        if re.match(r'^(public|private|protected)\s*:', stripped) or \
           re.match(r'^(public|private|protected)\s+(slots|Q_SLOTS)\s*:', stripped):
            section, statement = None, ''
            continue
        if section != 'meta':
            continue
        if not stripped or stripped.startswith('//') or stripped.startswith('*') \
           or stripped.startswith('/*'):
            continue
        if not statement:
            lineno = i
        statement += ' ' + stripped
        if ';' not in stripped:
            continue
        stmt, statement = statement.strip(), ''
        if re.search(r'\b(struct|class|enum|union|typedef|using|#)\b', stmt) \
           or ('(' not in stmt) or ('{' in stmt):
            print('FAIL %s:%d: %s' % (path, lineno, ' '.join(stmt.split())))
            bad += 1
if bad:
    print('%d declaration(s) inside signals:/slots: that moc 4.7 rejects' % bad)
    sys.exit(1)
print('ok   no type or data declarations inside signals:/slots:')
PY

if command -v moc >/dev/null 2>&1; then
    echo
    echo "== moc run for real"
    for h in qt/src/*.h qt/src/direct/*.h qt/src/qt4compat/*.h; do
        grep -q 'Q_OBJECT' "$h" || continue
        if moc "$h" -o /dev/null 2>/tmp/moc_err.txt; then
            echo "ok   $h"
        else
            echo "FAIL $h"
            head -5 /tmp/moc_err.txt | sed 's/^/     /'
            fail=1
        fi
    done
else
    echo
    echo "(no moc binary on PATH: heuristic check only — Qt SDK has moc for a full run)"
fi

echo
if [ "$fail" = "0" ]; then
    echo "moc compatibility OK"
else
    echo "moc compatibility FAILED"
fi
exit $fail
