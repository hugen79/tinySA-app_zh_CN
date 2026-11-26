#include "appsettings.h"
#include "autotest.h"
#include "mainwindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QLocale>
#include <QTranslator>

#ifndef APP_VERSION  // set by CMakeLists.txt from git
#define APP_VERSION "2.0.0"
#endif

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("tinySA-App");
    app.setApplicationVersion(APP_VERSION);

    QCommandLineParser cli;
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addOptions({
        {"sim", "Offer simulated tinySA units in the port list."},
        {"port", "Connect to this port at start (COMx, sim:basic, sim:ultra).", "port"},
        {"ini", "Settings file (default: next to the exe, else AppData).", "file"},
        {"script", "Run an autotest script, then exit with the number of failures.", "file"},
        {"out", "Autotest output directory.", "dir", "test/out"},
        {"lang", "Interface language (zh_CN), or auto-detect from the system.", "lang"},
    });
    cli.process(app);

    // Interface translation: --lang zh_CN, or Chinese system locale, or English.
    QTranslator translator;
    QString lang = cli.value("lang");
    if (lang.isEmpty() && QLocale::system().language() == QLocale::Chinese)
        lang = QStringLiteral("zh_CN");
    if (!lang.isEmpty() && lang.compare(QStringLiteral("en"), Qt::CaseInsensitive) != 0
        && translator.load(QStringLiteral(":/translations/tinySA-app_") + lang))
        app.installTranslator(&translator);

    const QString ini = cli.isSet("ini") ? cli.value("ini") : AppSettings::defaultPath();
    MainWindow win(cli.isSet("sim") || cli.value("port").startsWith("sim:"), ini);
    win.show();
    if (cli.isSet("port"))
        win.connectTo(cli.value("port"));
    if (cli.isSet("script")) {
        auto *t = new AutoTest(&win, cli.value("script"), cli.value("out"), &app);
        t->start();
    }
    return app.exec();
}
