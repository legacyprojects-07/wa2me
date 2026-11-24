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
#include <QStringList>

#include <cstdio>

#include "mainwindow.h"

namespace {

// QCommandLineParser is Qt 5.2+, so the few options this client has are parsed
// by hand (also keeps the binary small on a phone).
struct Options {
    QString baseUrl;
    QString apiKey;
    QString credsPath;
    bool direct = false;
    bool help = false;
    bool version = false;
};

QString usageText() {
    return QString(
               "Usage: openknot-qt [options]\n\n"
               "Qt client for OpenKnot: proxy mode (HTTP API) or direct WhatsApp Web mode.\n\n"
               "Options:\n"
               "  -u, --url <url>       Backend base URL (proxy mode, default "
               "http://127.0.0.1:3000)\n"
               "  -k, --key <key>       Backend API key (X-API-Key)\n"
               "      --direct          Connect directly to wss://web.whatsapp.com\n"
               "      --creds <path>    Credentials JSON path (direct mode)\n"
               "  -h, --help            Show this help\n"
               "  -v, --version         Show the version\n");
}

Options parseOptions(const QStringList &args) {
    Options out;
    out.baseUrl = "http://127.0.0.1:3000";
    for (int i = 1; i < args.size(); i++) {
        const QString arg = args.at(i);
        QString key = arg;
        QString value;
        const int eq = arg.indexOf('=');
        if (arg.startsWith("--") && eq > 0) {
            key = arg.left(eq);
            value = arg.mid(eq + 1);
        }

        const bool needsValue =
            key == "-u" || key == "--url" || key == "-k" || key == "--key" ||
            key == "--creds";
        if (needsValue && value.isEmpty()) {
            if (i + 1 < args.size()) value = args.at(++i);
        }

        if (key == "-h" || key == "--help") out.help = true;
        else if (key == "-v" || key == "--version") out.version = true;
        else if (key == "--direct") out.direct = true;
        else if (key == "-u" || key == "--url") out.baseUrl = value;
        else if (key == "-k" || key == "--key") out.apiKey = value;
        else if (key == "--creds") out.credsPath = value;
    }
    return out;
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("openknot-qt");
    QApplication::setApplicationVersion("1.0.0");

    const Options options = parseOptions(QApplication::arguments());
    if (options.help) {
        std::printf("%s", usageText().toLocal8Bit().constData());
        return 0;
    }
    if (options.version) {
        std::printf("openknot-qt %s\n", qPrintable(QApplication::applicationVersion()));
        return 0;
    }

    MainWindow window;
    if (!options.credsPath.isEmpty()) window.setCredentialsPath(options.credsPath);
    window.show();
    window.start(options.baseUrl, options.apiKey, options.direct);
    return app.exec();
}
