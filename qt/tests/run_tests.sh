#!/usr/bin/env bash
# Builds and runs every Qt-free core suite with the host g++. No Qt needed.
#
#   ./qt/tests/run_tests.sh              # build + run everything
#   ./qt/tests/run_tests.sh --regen      # regenerate vectors from Baileys first
#
# Suites:
#   codec       BinaryNode encoder/decoder + raw DEFLATE
#   crypto      SHA-256/HMAC/HKDF, X25519, AES-256-GCM/CBC
#   proto       handshake protobuf
#   noise       Noise handshake against a real Baileys transcript
#   ed25519     SHA-512 / Ed25519 / XEdDSA (curve25519-js vectors)
#   signal      Signal double ratchet, both directions against libsignal*
#   wa_proto    Message/HistorySync/HistorySyncNotification protobuf
#   history     HistorySync blob decrypt (HKDF+AES-CBC+HMAC) + inflate
#   message     end-to-end <message> stanza build/parse against libsignal*,
#               plus group (sender-key) stanza build/decrypt
#   sender_key  group sender keys against Baileys' GroupCipher*
#   media       media upload/download (CDN keys, hashes, URLs) against Baileys
#
# * signal and message run three phases: the C++ side encrypts, a Node script
#   using the real libsignal decrypts and replies, then C++ decrypts the reply.
#   Those phases need BAILEYS_DIR (the @whiskeysockets/baileys package dir).

set -euo pipefail
cd "$(dirname "$0")/../.."

SRC=qt/src/direct
T=qt/tests
OUT=${TMPDIR:-/tmp}/openknot-qt-tests
mkdir -p "$OUT"

CXX=${CXX:-g++}
CFLAGS="-std=c++17 -O2 -Wall -Wextra -I $SRC"
COMMON="$SRC/sha256.cpp $SRC/x25519.cpp $SRC/aes_gcm.cpp $SRC/proto_min.cpp"
WAPROTO="$SRC/wa_proto.cpp $COMMON"

if [[ "${1:-}" == "--regen" ]]; then
  echo "== regenerating vectors from Baileys + Node =="
  ( cd "$T" && node gen_vectors.mjs && node gen_crypto.mjs && node gen_proto.mjs &&
      node gen_noise.mjs && node gen_sig.mjs && node gen_wa_proto.mjs &&
      node gen_history.mjs && node gen_signal.mjs && node gen_message.mjs &&
      node gen_sender_key.mjs && node gen_media.mjs )
fi

echo "== building test suites =="
$CXX $CFLAGS $T/test_codec.cpp $SRC/binary_node.cpp $SRC/inflate.cpp -o "$OUT/test_codec"
$CXX $CFLAGS $T/test_crypto.cpp $COMMON -o "$OUT/test_crypto"
$CXX $CFLAGS $T/test_proto.cpp $SRC/proto_min.cpp -o "$OUT/test_proto"
$CXX $CFLAGS $T/test_noise.cpp $SRC/noise.cpp $COMMON -o "$OUT/test_noise"
$CXX $CFLAGS $T/test_ed25519.cpp $SRC/ed25519.cpp $SRC/x25519.cpp $SRC/sha256.cpp -o "$OUT/test_ed25519"
$CXX $CFLAGS $T/test_signal.cpp $SRC/signal.cpp $SRC/ed25519.cpp $COMMON -o "$OUT/test_signal"
$CXX $CFLAGS $T/test_wa_proto.cpp $WAPROTO -o "$OUT/test_wa_proto"
$CXX $CFLAGS $T/test_history.cpp $SRC/wa_history.cpp $SRC/media_crypto.cpp \
    $SRC/wa_proto.cpp $SRC/binary_node.cpp $SRC/inflate.cpp $COMMON -o "$OUT/test_history"
$CXX $CFLAGS $T/test_message.cpp $SRC/wa_message.cpp $SRC/signal.cpp $SRC/ed25519.cpp \
    $SRC/sender_key.cpp $SRC/wa_proto.cpp $SRC/binary_node.cpp $SRC/inflate.cpp $COMMON \
    -o "$OUT/test_message"
$CXX $CFLAGS $T/test_sender_key.cpp $SRC/sender_key.cpp $SRC/signal.cpp $SRC/ed25519.cpp \
    $SRC/binary_node.cpp $SRC/inflate.cpp $COMMON -o "$OUT/test_sender_key"
$CXX $CFLAGS $T/test_media.cpp $SRC/media_upload.cpp $SRC/media_crypto.cpp \
    $SRC/wa_proto.cpp $SRC/binary_node.cpp $SRC/inflate.cpp $COMMON -o "$OUT/test_media"

echo
"$OUT/test_codec" $T/vectors.txt
"$OUT/test_crypto" $T/crypto_vectors.txt
"$OUT/test_proto" $T/proto_vectors.txt
"$OUT/test_noise" $T/noise_vectors.txt
"$OUT/test_ed25519" $T/sig_vectors.txt
"$OUT/test_wa_proto" $T/wa_proto_vectors.txt
"$OUT/test_history" $T/history_vectors.txt
"$OUT/test_sender_key" $T/sender_key_vectors.txt "$OUT"
"$OUT/test_media" $T/media_vectors.txt "$OUT"

# ── Signal interop: C++ -> libsignal -> C++ ────────────────────────────────
if [[ -n "${BAILEYS_DIR:-}" ]]; then
  echo
  echo "== Signal interop (needs BAILEYS_DIR) =="
  rm -f "$OUT/cpp_to_bob.txt" "$OUT/js_reply.txt"
  "$OUT/test_signal" $T/signal_vectors.txt "$OUT/cpp_to_bob.txt"
  node "$T/verify_signal.mjs" $T/signal_vectors.txt "$OUT/cpp_to_bob.txt" "$OUT/js_reply.txt"
  "$OUT/test_signal" $T/signal_vectors.txt "$OUT/cpp_to_bob.txt" "$OUT/js_reply.txt"

  echo
  echo "== Message layer interop =="
  rm -f "$OUT/cpp_message.txt" "$OUT/cpp_message_store.txt" \
        "$OUT/node_message.txt" "$OUT/node_message2.txt" "$OUT/node_wrapped.txt"
  "$OUT/test_message" $T/message_vectors.txt "$OUT"
  node "$T/verify_message.mjs" $T/message_vectors.txt "$OUT"
  "$OUT/test_message" $T/message_vectors.txt "$OUT" "$OUT"

  echo
  echo "== Group sender-key interop =="
  rm -f "$OUT/cpp_group.txt" "$OUT/node_group.txt"
  "$OUT/test_sender_key" $T/sender_key_vectors.txt "$OUT"
"$OUT/test_media" $T/media_vectors.txt "$OUT"
  node "$T/verify_group.mjs" $T/sender_key_vectors.txt "$OUT"
  "$OUT/test_sender_key" $T/sender_key_vectors.txt "$OUT" "$OUT"

  echo
  echo "== Media upload/download interop =="
  rm -f "$OUT/cpp_media.txt" "$OUT/node_media.txt"
  "$OUT/test_media" $T/media_vectors.txt "$OUT"
  node "$T/verify_media.mjs" $T/media_vectors.txt "$OUT"
  "$OUT/test_media" $T/media_vectors.txt "$OUT" "$OUT"
else
  echo
  echo "!! BAILEYS_DIR is unset: skipping the libsignal interop phases"
  echo "   (set BAILEYS_DIR to the @whiskeysockets/baileys package directory)"
  "$OUT/test_signal" $T/signal_vectors.txt "$OUT/cpp_to_bob.txt"
  "$OUT/test_message" $T/message_vectors.txt "$OUT"
fi

echo
echo "All core suites passed."
