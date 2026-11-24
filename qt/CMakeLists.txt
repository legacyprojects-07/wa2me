cmake_minimum_required(VERSION 3.16)
project(openknot-qt VERSION 1.0.0 LANGUAGES CXX)

# The protocol core is C++11-clean on purpose (Symbian ships GCC 4.x); desktop
# builds use 17 unless the toolchain file says otherwise.
if(NOT DEFINED CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Qt 4.7.4 is the target (Symbian): Widgets + Network only. Direct mode speaks
# RFC 6455 over QSslSocket itself (src/qt4compat/websocket.cpp) because
# QWebSocket only exists from Qt 5.3, and the QJson* classes from Qt 5.0 are
# replaced by src/qt4compat/json.cpp. Qt 5/6 still build (the shims are
# compiled out by QT_VERSION checks there).
find_package(Qt4 QUIET COMPONENTS QtCore QtGui QtNetwork)
if(Qt4_FOUND)
  set(QT_USE_QTGUI TRUE)
  include(${QT_USE_FILE})
  set(QT_LIBS ${QT_LIBRARIES})
  set(QT4_BUILD TRUE)
else()
  find_package(Qt6 QUIET COMPONENTS Widgets Network)
  if(NOT Qt6_FOUND)
    find_package(Qt5 REQUIRED COMPONENTS Widgets Network)
  endif()
endif()

add_executable(openknot-qt
  src/main.cpp
  src/api_client.cpp
  src/mainwindow.cpp
  # Qt 4.7 compatibility layer (QJson*, functor hooks, WebSocket transport)
  src/qt4compat/json.cpp
  src/qt4compat/qt4compat.cpp
  src/qt4compat/websocket.cpp
  # direct mode: transport stack (Qt-free internals + WebSocket/JSON glue)
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
  src/direct/sender_key.cpp
  src/direct/media_upload.cpp
)

target_include_directories(openknot-qt PRIVATE src src/direct src/qt4compat)

if(QT4_BUILD)
  # Qt 4 needs the network modules spelled out; no WebSockets (5.3+).
  target_link_libraries(openknot-qt PRIVATE ${QT_LIBS})
elseif(Qt6_FOUND)
  target_link_libraries(openknot-qt PRIVATE Qt6::Widgets Qt6::Network)
else()
  target_link_libraries(openknot-qt PRIVATE Qt5::Widgets Qt5::Network)
endif()

# The direct-mode stack is dependency-free on purpose: Noise, X25519,
# AES-256-GCM, HKDF, raw DEFLATE, Ed25519/XEdDSA, the Signal double ratchet,
# group sender keys, the WAProto subset and WhatsApp media crypto are all
# implemented in src/direct/, so the app needs no libsodium/zlib/protobuf/
# libsignal.
#
# Qt 4.7 (Symbian): everything the client uses from Qt is 4.7.4 API.
#   * QWebSocket (Qt 5.3)   -> src/qt4compat/websocket.cpp (RFC 6455 on QSslSocket)
#   * QJson*     (Qt 5.0)   -> src/qt4compat/json.{h,cpp}
#   * functor connects (Qt 5) -> SignalHook in src/qt4compat/qt4compat.h
#   * QHttpMultiPart (Qt 4.8), QUrlQuery/QCommandLineParser (Qt 5)
#       -> hand-built multipart body, QUrl::addQueryItem, argv parsing
# qt/tests/qt4_check.sh compiles every Qt source against the real 4.7.4
# headers (it fetches the qt/qt 4.7 branch and builds an include tree).

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
  src/direct/sender_key.cpp src/direct/ed25519.cpp src/direct/binary_node.cpp
  src/direct/inflate.cpp ${DIRECT_CORE})
target_include_directories(test_message PRIVATE src/direct)
# test_message runs three times (C++ encrypt -> Node/libsignal -> C++ decrypt),
# so it is driven by tests/run_tests.sh; here we only add the first phase.
add_test(NAME message_build COMMAND test_message
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/message_vectors.txt ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_sender_key tests/test_sender_key.cpp
  src/direct/sender_key.cpp src/direct/signal.cpp src/direct/ed25519.cpp
  src/direct/binary_node.cpp src/direct/inflate.cpp ${DIRECT_CORE})
target_include_directories(test_sender_key PRIVATE src/direct)
# Qt 4-only: exercises the JSON shim used for creds.json (needs real Qt 4).
if(QT4_FOUND)
  add_executable(test_qt4compat tests/test_qt4compat.cpp src/qt4compat/json.cpp)
  target_include_directories(test_qt4compat PRIVATE src src/qt4compat)
  target_link_libraries(test_qt4compat PRIVATE ${QT_LIBS})
  add_test(NAME qt4compat COMMAND test_qt4compat)
endif()

add_test(NAME sender_key COMMAND test_sender_key
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/sender_key_vectors.txt ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_media tests/test_media.cpp
  src/direct/media_upload.cpp src/direct/media_crypto.cpp src/direct/wa_proto.cpp
  src/direct/binary_node.cpp src/direct/inflate.cpp ${DIRECT_CORE})
target_include_directories(test_media PRIVATE src/direct)
add_test(NAME media COMMAND test_media
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/media_vectors.txt ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_codec tests/test_codec.cpp src/direct/binary_node.cpp src/direct/inflate.cpp)
target_include_directories(test_codec PRIVATE src/direct)

add_executable(test_crypto tests/test_crypto.cpp ${DIRECT_CORE})
target_include_directories(test_crypto PRIVATE src/direct)

add_executable(test_proto tests/test_proto.cpp src/direct/proto_min.cpp)
target_include_directories(test_proto PRIVATE src/direct)

add_executable(test_noise tests/test_noise.cpp src/direct/noise.cpp ${DIRECT_CORE})
target_include_directories(test_noise PRIVATE src/direct)

foreach(tc test_codec test_crypto test_proto test_noise test_ed25519 test_signal
          test_wa_proto test_history test_message test_sender_key test_media)
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
