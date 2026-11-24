cmake_minimum_required(VERSION 3.16)
project(openknot-qt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Qt5/Qt6: Widgets (UI), Network (HTTP proxy mode + media CDN),
# WebSockets (direct mode), Sql (local store for direct mode).
find_package(Qt6 QUIET COMPONENTS Widgets Network WebSockets Sql)
if(NOT Qt6_FOUND)
  find_package(Qt5 REQUIRED COMPONENTS Widgets Network WebSockets Sql)
endif()

add_executable(openknot-qt
  src/main.cpp
  src/api_client.cpp
  src/mainwindow.cpp
  # direct mode: transport stack (Qt-free internals + QWebSocket glue)
  src/direct/whatsapp_direct.cpp
  src/direct/binary_node.cpp
  src/direct/inflate.cpp
  src/direct/noise.cpp
  src/direct/proto_min.cpp
  src/direct/sha256.cpp
  src/direct/x25519.cpp
  src/direct/aes_gcm.cpp
  # Signal protocol, WAProto subset, media crypto, history + message layer
  src/direct/ed25519.cpp
  src/direct/signal.cpp
  src/direct/wa_proto.cpp
  src/direct/media_crypto.cpp
  src/direct/wa_history.cpp
  src/direct/wa_message.cpp
)

target_include_directories(openknot-qt PRIVATE src src/direct)

if(Qt6_FOUND)
  target_link_libraries(openknot-qt PRIVATE
    Qt6::Widgets Qt6::Network Qt6::WebSockets Qt6::Sql)
else()
  target_link_libraries(openknot-qt PRIVATE
    Qt5::Widgets Qt5::Network Qt5::WebSockets Qt5::Sql)
endif()

# The direct-mode stack is dependency-free on purpose: Noise, X25519,
# AES-256-GCM, HKDF, raw DEFLATE, Ed25519/XEdDSA, the Signal double ratchet,
# the WAProto subset and WhatsApp media crypto are all implemented in
# src/direct/, so the app needs no libsodium/zlib/protobuf/libsignal.
#
# Qt 4.7 (Symbian) note: QWebSocket does not exist before Qt 5.4, so direct
# mode needs the QTcpSocket transport described in QT_PORT.md §5. Proxy mode
# (api_client.cpp) only needs QtNetwork + a JSON parser.

# ── Core test suites (host g++, no Qt required) ────────────────────────────
# These validate the ported protocol code against vectors generated from the
# real Baileys package: BinaryNode codec, crypto, handshake protobuf, Noise.
#   cmake --build . && ctest --output-on-failure
# (or just run qt/tests/run_tests.sh)
enable_testing()

set(DIRECT_CORE
  src/direct/sha256.cpp
  src/direct/x25519.cpp
  src/direct/aes_gcm.cpp
  src/direct/proto_min.cpp
)

add_executable(test_ed25519 tests/test_ed25519.cpp src/direct/ed25519.cpp src/direct/x25519.cpp src/direct/sha256.cpp)
target_include_directories(test_ed25519 PRIVATE src/direct)

add_executable(test_signal tests/test_signal.cpp
  src/direct/signal.cpp src/direct/ed25519.cpp ${DIRECT_CORE})
target_include_directories(test_signal PRIVATE src/direct)

add_executable(test_wa_proto tests/test_wa_proto.cpp src/direct/wa_proto.cpp src/direct/proto_min.cpp)
target_include_directories(test_wa_proto PRIVATE src/direct)

add_executable(test_history tests/test_history.cpp
  src/direct/wa_history.cpp src/direct/media_crypto.cpp src/direct/wa_proto.cpp
  src/direct/binary_node.cpp src/direct/inflate.cpp ${DIRECT_CORE})
target_include_directories(test_history PRIVATE src/direct)

add_executable(test_message tests/test_message.cpp
  src/direct/wa_message.cpp src/direct/wa_proto.cpp src/direct/signal.cpp
  src/direct/ed25519.cpp src/direct/binary_node.cpp src/direct/inflate.cpp ${DIRECT_CORE})
target_include_directories(test_message PRIVATE src/direct)
# test_message runs three times (C++ encrypt -> Node/libsignal -> C++ decrypt),
# so it is driven by tests/run_tests.sh; here we only add the first phase.
add_test(NAME message_build COMMAND test_message
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/message_vectors.txt ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_codec tests/test_codec.cpp src/direct/binary_node.cpp src/direct/inflate.cpp)
target_include_directories(test_codec PRIVATE src/direct)

add_executable(test_crypto tests/test_crypto.cpp ${DIRECT_CORE})
target_include_directories(test_crypto PRIVATE src/direct)

add_executable(test_proto tests/test_proto.cpp src/direct/proto_min.cpp)
target_include_directories(test_proto PRIVATE src/direct)

add_executable(test_noise tests/test_noise.cpp src/direct/noise.cpp ${DIRECT_CORE})
target_include_directories(test_noise PRIVATE src/direct)

foreach(tc test_codec test_crypto test_proto test_noise test_ed25519 test_signal
          test_wa_proto test_history test_message)
  target_compile_options(${tc} PRIVATE -Wall -Wextra)
endforeach()

add_test(NAME codec COMMAND test_codec ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors.txt)
add_test(NAME crypto COMMAND test_crypto ${CMAKE_CURRENT_SOURCE_DIR}/tests/crypto_vectors.txt)
add_test(NAME proto COMMAND test_proto ${CMAKE_CURRENT_SOURCE_DIR}/tests/proto_vectors.txt)
add_test(NAME noise COMMAND test_noise ${CMAKE_CURRENT_SOURCE_DIR}/tests/noise_vectors.txt)
add_test(NAME ed25519 COMMAND test_ed25519 ${CMAKE_CURRENT_SOURCE_DIR}/tests/sig_vectors.txt)
add_test(NAME wa_proto COMMAND test_wa_proto ${CMAKE_CURRENT_SOURCE_DIR}/tests/wa_proto_vectors.txt)
add_test(NAME history COMMAND test_history ${CMAKE_CURRENT_SOURCE_DIR}/tests/history_vectors.txt)
# signal / message need libsignal on the Node side: use tests/run_tests.sh
