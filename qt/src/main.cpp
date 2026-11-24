// OpenKnot Qt — entry point.
//
//   openknot-qt                          # proxy mode, backend at :3000
//   openknot-qt --url http://host:3000 --key APIKEY
//   openknot-qt --direct                 # connect straight to WhatsApp Web
//
// Proxy mode speaks the existing OpenKnot REST API (index.js). Direct mode
// opens wss://web.whatsapp.com itself and runs the Noise handshake; see
// QT_PORT.md for exactly which parts of the Baileys stack are implemented.

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>

#include "mainwindow.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("openknot-qt");
    QApplication::setApplicationVersion("1.0.0");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Qt client for OpenKnot — proxy mode (HTTP API) or direct WhatsApp Web mode.");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption urlOpt({"u", "url"}, "Backend base URL (proxy mode).",
                              "url", "http://127.0.0.1:3000");
    QCommandLineOption keyOpt({"k", "key"}, "Backend API key (X-API-Key).", "key");
    QCommandLineOption directOpt("direct", "Connect directly to wss://web.whatsapp.com.");
    QCommandLineOption credsOpt("creds", "Path to the credentials JSON (direct mode).",
                                "path");
    parser.addOption(urlOpt);
    parser.addOption(keyOpt);
    parser.addOption(directOpt);
    parser.addOption(credsOpt);
    parser.process(app);

    MainWindow window;
    window.show();
    window.start(parser.value(urlOpt), parser.value(keyOpt), parser.isSet(directOpt));
    return app.exec();
}
