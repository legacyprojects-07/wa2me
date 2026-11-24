#!/usr/bin/env bash
# Compiles every Qt source of the client against *real* Qt 4.7.4 headers.
#
# The Symbian toolchain ships Qt 4.7.4, which has no QJson*, no QWebSocket, no
# QHttpMultiPart, no QUrlQuery, no QCommandLineParser and no functor connects.
# This script proves the client only uses what 4.7.4 provides:
#
#   1. fetch the Qt 4.7 branch of the qt/qt mirror (v4.7.4: qglobal.h says
#      QT_VERSION 0x040704) unless QT4_DIR points at an existing checkout
#   2. build an `include/` tree with forwarding headers (QString -> qstring.h…)
#      the way `make install` would, and a qconfig.h for a desktop build
#   3. run g++ -fsyntax-only on src/*.cpp and src/direct/whatsapp_direct.cpp
#
#   ./qt/tests/qt4_check.sh                     # clone into /tmp/qt4 (cached)
#   QT4_DIR=/path/to/qt4 ./qt/tests/qt4_check.sh
#
# Note: -fsyntax-only needs no moc, no libraries and no GUI, so it runs in CI.
set -euo pipefail
cd "$(dirname "$0")/../.."

QT4_DIR=${QT4_DIR:-/tmp/qt4}
QT4_INC=${QT4_INC:-/tmp/qt4-headers}
WORK=${TMPDIR:-/tmp}/openknot-qt4-check
mkdir -p "$WORK"

if [[ ! -d "$QT4_DIR/src/corelib" ]]; then
  echo "== cloning Qt 4.7 (qt/qt branch 4.7 = v4.7.4)"
  rm -rf "$QT4_DIR"
  git clone --depth 1 --branch 4.7 --filter=blob:none --sparse \
      https://github.com/qt/qt "$QT4_DIR"
  git -C "$QT4_DIR" sparse-checkout set src/corelib src/gui src/network
fi

echo "== building the Qt 4.7.4 header tree"
python3 - "$QT4_DIR/src" "$QT4_INC" <<'PY'
import os, re, sys
src, inc = sys.argv[1], sys.argv[2]
mods = {'corelib': 'QtCore', 'gui': 'QtGui', 'network': 'QtNetwork'}
pat = re.compile(r'\b(?:class|struct)\s+(?:[A-Z][A-Z0-9_]*_EXPORT\s+)?(Q[A-Z][A-Za-z0-9_]*)')

for mod, mdir in mods.items():
    os.makedirs(os.path.join(inc, mdir), exist_ok=True)
    root = os.path.join(src, mod)
    for dirpath, dirnames, filenames in os.walk(root):
        if 'tests' in dirpath.split(os.sep):
            continue
        for fn in filenames:
            if not fn.endswith('.h'):
                continue
            path = os.path.join(dirpath, fn)
            link = os.path.join(inc, mdir, fn)
            if not os.path.exists(link):
                os.symlink(path, link)

# Class-name forwarding headers (Qt installs one per class).
classes = {}
for mod, mdir in mods.items():
    for dirpath, dirnames, filenames in os.walk(os.path.join(src, mod)):
        if 'tests' in dirpath.split(os.sep):
            continue
        for fn in filenames:
            if not fn.endswith('.h'):
                continue
            try:
                text = open(os.path.join(dirpath, fn), encoding='utf-8', errors='ignore').read()
            except OSError:
                continue
            for cls in pat.findall(text):
                defines = re.search(r'\bclass\s+(?:[A-Z][A-Z0-9_]*_EXPORT\s+)?' + cls +
                                    r'\s*(?::|\{)', text) is not None
                prev = classes.get(cls)
                if prev is None or (defines and not prev[2]):
                    classes[cls] = (mdir, fn, defines)

for cls, (mdir, fn, _d) in classes.items():
    if cls.endswith('Private'):
        continue
    with open(os.path.join(inc, mdir, cls), 'w') as f:
        f.write('#include "%s"\n' % fn)

# Module umbrella headers the sources include directly.
for name, headers in (
        ('QtCore/QtGlobal', ['qglobal.h', 'qnamespace.h', 'qendian.h']),
        ('QtGui/QtGui', ['qwidget.h', 'qapplication.h', 'qmainwindow.h']),
        ('QtNetwork/QtNetwork', ['qnetworkaccessmanager.h', 'qnetworkreply.h',
                                 'qnetworkrequest.h', 'qtcpsocket.h', 'qsslsocket.h',
                                 'qurl.h'])):
    path = os.path.join(inc, name)
    if not os.path.exists(path):
        with open(path, 'w') as f:
            for header in headers:
                f.write('#include "%s"\n' % header)

# qconfig.h as `configure` would generate it for a desktop build.
qconfig = os.path.join(inc, 'QtCore', 'qconfig.h')
if not os.path.exists(qconfig):
    with open(qconfig, 'w') as f:
        f.write('#include "qconfig-dist.h"\n'
                '#define QT_EDITION QT_EDITION_DESKTOP\n'
                '#define Q_BYTE_ORDER Q_LITTLE_ENDIAN\n'
                '#define QT_LITTLE_ENDIAN 1\n'
                '#define QT_ARCH_I386\n'
                '#define QT_NO_DEBUG\n'
                '#define QT_SHARED\n')
PY

CXX=${CXX:-g++}
FLAGS=(-std=c++11 -fsyntax-only
       -isystem "$QT4_INC" -isystem "$QT4_INC/QtCore" -isystem "$QT4_INC/QtGui"
       -isystem "$QT4_INC/QtNetwork"
       -I qt/src -I qt/src/direct -I qt/src/qt4compat)

SOURCES=(
  qt/src/qt4compat/json.cpp
  qt/src/qt4compat/qt4compat.cpp
  qt/src/qt4compat/websocket.cpp
  qt/src/api_client.cpp
  qt/src/mainwindow.cpp
  qt/src/main.cpp
  qt/src/direct/whatsapp_direct.cpp
  qt/tests/test_qt4compat.cpp
)

fail=0
for src in "${SOURCES[@]}"; do
  if $CXX "${FLAGS[@]}" "$src" 2> "$WORK/$(basename "$src").log"; then
    echo "ok   $src"
  else
    echo "FAIL $src"
    grep -E "error" "$WORK/$(basename "$src").log" | head -10
    fail=1
  fi
done

if [[ $fail -eq 0 ]]; then
  echo
  echo "All Qt sources compile against Qt 4.7.4 headers."
  grep -q "define QT_VERSION 0x040704" "$QT4_INC/QtCore/qglobal.h" ||
    echo "warning: headers are not 4.7.4 (QT_VERSION differs) - check QT4_DIR"
else
  echo
  echo "Qt 4.7.4 check FAILED (logs in $WORK)"
fi
exit $fail
