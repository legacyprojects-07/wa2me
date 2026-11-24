#!/usr/bin/env bash
# Builds and runs the Qt-free core test suites (codec, crypto, protobuf, noise)
# with the host g++. No Qt installation is required.
#
#   ./qt/tests/run_tests.sh
#
# The vectors are generated from the real Baileys package and Node's crypto
# module (see qt/tests/gen_vectors.mjs / gen_crypto.mjs / gen_proto.mjs /
# gen_noise.mjs); regenerate them with `./qt/tests/run_tests.sh --regen` when
# bumping the Baileys version.

set -euo pipefail
cd "$(dirname "$0")/../.."

SRC=qt/src/direct
OUT=${TMPDIR:-/tmp}/openknot-qt-tests
mkdir -p "$OUT"

CXX=${CXX:-g++}
CFLAGS="-std=c++17 -O2 -Wall -Wextra -I $SRC"
COMMON="$SRC/sha256.cpp $SRC/x25519.cpp $SRC/aes_gcm.cpp $SRC/proto_min.cpp"

if [[ "${1:-}" == "--regen" ]]; then
  echo "== regenerating vectors from Baileys + Node =="
  ( cd qt/tests && node gen_vectors.mjs && node gen_crypto.mjs && node gen_proto.mjs && node gen_noise.mjs )
fi

echo "== building test suites =="
$CXX $CFLAGS qt/tests/test_codec.cpp $SRC/binary_node.cpp $SRC/inflate.cpp -o "$OUT/test_codec"
$CXX $CFLAGS qt/tests/test_crypto.cpp $COMMON -o "$OUT/test_crypto"
$CXX $CFLAGS qt/tests/test_proto.cpp $SRC/proto_min.cpp -o "$OUT/test_proto"
$CXX $CFLAGS qt/tests/test_noise.cpp $SRC/noise.cpp $COMMON -o "$OUT/test_noise"

echo
"$OUT/test_codec" qt/tests/vectors.txt
"$OUT/test_crypto" qt/tests/crypto_vectors.txt
"$OUT/test_proto" qt/tests/proto_vectors.txt
"$OUT/test_noise" qt/tests/noise_vectors.txt
echo
echo "All core suites passed."
