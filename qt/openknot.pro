# qmake project for the Symbian Qt 4.7 toolchain (CMake is not an option there).
# Works with any Qt 4.7+ desktop install too:
#
#   qmake qt/openknot.pro && make            # desktop Qt 4.7
#   qmake -spec symbian-gcce && make         # Symbian SBSv2 toolchain
#
# The sources only use Qt 4.7 API (verified by tests/qt4_check.sh); the
# Qt 5 APIs the client would otherwise need live in src/qt4compat/.

TEMPLATE = app
TARGET   = openknot-qt
QT      += core gui network

CONFIG  += warn_on
CONFIG  -= app_bundle
CONFIG  -= qtquick

# The protocol core sticks to the C++11 subset that GCC 4.6 understands (the
# Symbian GCGE is 4.6.3), which is why the flag is spelled c++0x here:
# -std=c++11 is an unknown option to that compiler. GCC 4.7+ accepts c++0x too.
# tests/gcc46_check.sh is the gate that keeps it that way.
QMAKE_CXXFLAGS += -std=c++0x

INCLUDEPATH += src src/direct src/qt4compat

HEADERS += \
    src/models.h \
    src/api_client.h \
    src/mainwindow.h \
    src/qt4compat/json.h \
    src/qt4compat/qt4compat.h \
    src/qt4compat/websocket.h \
    src/direct/compat.h \
    src/direct/wa_tokens.h \
    src/direct/binary_node.h \
    src/direct/inflate.h \
    src/direct/sha256.h \
    src/direct/x25519.h \
    src/direct/aes_gcm.h \
    src/direct/proto_min.h \
    src/direct/noise.h \
    src/direct/ed25519.h \
    src/direct/signal.h \
    src/direct/sender_key.h \
    src/direct/wa_proto.h \
    src/direct/media_crypto.h \
    src/direct/media_upload.h \
    src/direct/wa_history.h \
    src/direct/wa_message.h \
    src/direct/whatsapp_direct.h

SOURCES += \
    src/main.cpp \
    src/api_client.cpp \
    src/mainwindow.cpp \
    src/qt4compat/json.cpp \
    src/qt4compat/qt4compat.cpp \
    src/qt4compat/websocket.cpp \
    src/direct/binary_node.cpp \
    src/direct/inflate.cpp \
    src/direct/sha256.cpp \
    src/direct/x25519.cpp \
    src/direct/aes_gcm.cpp \
    src/direct/proto_min.cpp \
    src/direct/noise.cpp \
    src/direct/ed25519.cpp \
    src/direct/signal.cpp \
    src/direct/sender_key.cpp \
    src/direct/wa_proto.cpp \
    src/direct/media_crypto.cpp \
    src/direct/media_upload.cpp \
    src/direct/wa_history.cpp \
    src/direct/wa_message.cpp \
    src/direct/whatsapp_direct.cpp

symbian {
    TARGET.UID3 = 0x2A7B9C41
    TARGET.CAPABILITY += NetworkServices ReadUserData WriteUserData
    TARGET.EPOCHEAPSIZE = 0x20000 0x2000000
    # Credentials/attachments live on the mass memory, not in C:.
    DEFINES += OPENKNOT_SYMBIAN=1
}
