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
