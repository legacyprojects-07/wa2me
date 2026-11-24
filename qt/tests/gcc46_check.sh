#!/bin/sh
# Guard for the Symbian toolchain (GCGE = GCC 4.6.3).
#
# The sources are C++11, but the Symbian compiler is 4.6.3: no non-static data
# member initialisers (4.7), no `override` (4.7), no std::to_string/stoi
# (4.9), no <chrono> guarantees, no C++14 features at all. The host compiler
# accepts all of them, so this script is the gate:
#
#   1. grep for constructs GCC 4.6 rejects,
#   2. compile the protocol core with -std=c++0x (the 4.6 spelling of C++11),
#      which also rejects C++14/17-only syntax.
#
# Usage: ./qt/tests/gcc46_check.sh
set -u
cd "$(dirname "$0")/../.." || exit 2
CXX=${CXX:-g++}
fail=0

echo "== constructs GCC 4.6.3 does not support"

check() {   # check <description> <pattern>
    # Comment lines are skipped: they mention the constructs they explain.
    hits=$(grep -rnE "$2" qt/src --include=*.h --include=*.cpp \
                --exclude=wa_tokens.h --exclude=compat.h 2>/dev/null |
           grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*|/\*)')
    if [ -n "$hits" ]; then
        echo "FAIL $1"
        echo "$hits" | head -20 | sed 's/^/     /'
        fail=1
    else
        echo "ok   $1"
    fi
}

check "no 'override' keyword"              '(^|[^A-Za-z_])override([^A-Za-z_]|$)'
check "no std::to_string"                  'std::to_string'
check "no std::sto[iuls]"                  'std::sto(i|l|ul|ull|d|f)\('
check "no <chrono>"                        'std::chrono'
check "no std::make_unique"                'std::make_unique'
check "no generic lambdas"                 '\[[^]]*\]\([^)]*auto'
check "no auto return type deduction"      '^(auto|[A-Za-z_:<>]+[[:space:]]+auto)[^=]*\([^)]*\)[[:space:]]*(const[[:space:]]*)?\{'
check "no if constexpr / structured bindings" '(if constexpr|auto[[:space:]]*\[)'
# Non-static data member initialisers (GCC 4.7+): a member declaration with an
# initialiser, i.e. no '(' before the '=' (that would be a default argument).
# Headers only (member initialisers live there) and values without parentheses,
# so that local variables inside inline functions do not trip the check.
hits=$(grep -rnE '^[[:space:]]+[A-Za-z_][A-Za-z0-9_:<>]*([[:space:]]*\*)?[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[[:space:]]*-?[A-Za-z0-9_:."]+;[[:space:]]*(//.*)?$' \
            qt/src --include=*.h --exclude=wa_tokens.h --exclude=compat.h 2>/dev/null |
       grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*|/\*)')
if [ -n "$hits" ]; then
    echo "FAIL no non-static data member initialisers"
    echo "$hits" | head -20 | sed 's/^/     /'
    fail=1
else
    echo "ok   no non-static data member initialisers"
fi

echo
echo "== constructs the Symbian STL (STLport 5.2) does not have"
# The Symbian SDK ships STLport 5.2, which predates C++11: no <functional>
# std::function, no smart pointers, no initializer_list, no <random>, no
# <cstdint>, no std::move, no vector::data()/emplace_back. wa::function,
# compat::bytes/bytesFromText/move/dataOf and jsonObj() exist for exactly this
# reason — see qt/src/direct/callable.h and compat.h.

check "no std::function (use wa::function)"     'std::function'
check "no std::shared_ptr/std::unique_ptr"      'std::(shared|unique|weak)_ptr'
check "no std::make_shared"                     'std::make_shared'
check "no std::initializer_list"                'initializer_list'
check "no std::move (use compat::move)"         'std::move\('
check "no std::unordered_*"                     'std::unordered_'
check "no std::random_device / <random>"        '(std::random_device|<random>)'
check "no std::atomic/thread/mutex/regex"       'std::(atomic|thread|mutex|regex)'
check "no <cstdint> (use <stdint.h>)"           '#include[[:space:]]*<cstdint>'
check "no <memory>/<functional>/<chrono>/<array>/<tuple>" \
      '#include[[:space:]]*<(memory|functional|chrono|array|tuple)>'
check "no emplace/emplace_back"                 '\bemplace(_back)?[[:space:]]*\('
check "no braced container initialisation"      '(Bytes|std::vector|std::string|std::map|QStringList)[[:space:]]*\{'

# vector::data() is C++11; STLport 5.2 has no such member. The protocol core
# therefore goes through compat::dataOf() (vector *and* string) and the proto
# Reader/Writer expose bytes(). whatsapp_direct.cpp is Qt glue and may use
# QByteArray::data(), which Qt 4 has always had.
hits=$(grep -rnE '\.data\(\)' qt/src/direct --include=*.h --include=*.cpp 2>/dev/null |
       grep -v 'whatsapp_direct.cpp' |
       grep -v '^qt/src/direct/compat.h:')
if [ -n "$hits" ]; then
    echo "FAIL no .data() in the protocol core (use compat::dataOf / bytes())"
    echo "$hits" | head -20 | sed 's/^/     /'
    fail=1
else
    echo "ok   no .data() in the protocol core (use compat::dataOf / bytes())"
fi

echo
echo "== protocol core compiles as -std=c++0x (GCC 4.6's C++11)"
for f in qt/src/direct/*.cpp; do
    # whatsapp_direct.cpp is the Qt glue; qt4_check.sh compiles that one.
    case "$f" in *whatsapp_direct.cpp) continue;; esac
    if $CXX -std=c++0x -fsyntax-only -Wall -Wextra -I qt/src/direct -I qt/src "$f" 2>/tmp/gcc46_err.txt; then
        echo "ok   $f"
    else
        echo "FAIL $f"
        head -10 /tmp/gcc46_err.txt | sed 's/^/     /'
        fail=1
    fi
done

echo
if [ "$fail" = "0" ]; then
    echo "GCC 4.6 compatibility OK"
else
    echo "GCC 4.6 compatibility FAILED"
fi
exit $fail
