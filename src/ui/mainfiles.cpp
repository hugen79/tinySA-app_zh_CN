// MainWindow: traces on the graph, memories, CSV/image files and recording.
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMenu>
#include <QStandardPaths>

void MainWindow::renderTraces(bool memories)
{
    const TraceSet &t = m_session.traces();
    ui->spectrum->setTrace(0, t.normalizedView(t.live()));
    if (memories)
        for (int m = 0; m < TraceSet::kMemories; ++m)
            ui->spectrum->setTrace(m + 1, t.normalizedView(t.memory(m)));
    renderMarkers();  // readings follow the data
}

void MainWindow::showAverage()
{
    const TraceSet &t = m_session.traces();
    m_avgLabel->setVisible(t.average() > 1 && m_session.activity() == DeviceLink::Analyzer);
    m_avgLabel->setText(QString("Avg %1/%2").arg(t.averaged()).arg(t.average()));
}

void MainWindow::setupMemoryButton(int m, QToolButton *b)
{
    connect(b, &QToolButton::toggled, this, [this, m](bool on) { ui->spectrum->setShown(m + 1, on); });
    auto *menu = new QMenu(b);
    menu->addAction(tr("Store live trace"), this, [this, m, b] {
        m_session.traces().store(m);
        renderTraces(true);
        b->setChecked(true);
    });
    menu->addAction(tr("Load CSV..."), this, [this, m, b] {
        const QString path = QFileDialog::getOpenFileName(this, b->text() + " - load", m_settings.recordFolder,
                                                          "CSV files (*.csv);;All files (*)");
        if (!path.isEmpty() && loadMemory(m, path))
            b->setChecked(true);
    });
    menu->addAction(tr("Save CSV..."), this, [this, m, b] {
        const QString path = QFileDialog::getSaveFileName(this, b->text() + " - save", m_settings.recordFolder,
                                                          "CSV files (*.csv)");
        QString err;
        if (!path.isEmpty() && !writeTraceCsv(path, m_session.traces().memory(m), m_settings.csv, &err, b->text()))
            error("Cannot save " + path + ": " + err);
    });
    menu->addAction(tr("Clear"), this, [this, m, b] {
        m_session.traces().setMemory(m, {});
        renderTraces(true);
        b->setChecked(false);
    });
    b->setMenu(menu);
}

bool MainWindow::loadMemory(int m, const QString &path)
{
    QString err;
    const auto t = readTraceCsv(path, &err);
    if (!t) {
        error("Cannot load " + path + ": " + err);
        return false;
    }
    m_session.traces().setMemory(m, *t);
    renderTraces(true);
    return true;
}

QString MainWindow::memoryFile(int m) const
{
    return QFileInfo(m_settingsPath).absolutePath() + QString("/tinySA-App.M%1.csv").arg(m + 1);
}

QString MainWindow::imageHeader() const
{
    const QString when = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    const QString name = m_settings.sweepName.trimmed();
    return name.isEmpty() ? when : name + "    " + when;
}

static QString defaultName(const QString &folder, const QString &ext)
{
    QDir().mkpath(folder);
    return folder + "/tinySA " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh-mm-ss") + ext;
}

// path "" = ask, "clipboard:" = copy
bool MainWindow::saveImage(const QString &path)
{
    const QImage img = ui->spectrum->image(imageHeader());
    if (path == "clipboard:") {
        QGuiApplication::clipboard()->setImage(img);
        statusBar()->showMessage("Graph copied to the clipboard", 3000);
        return true;
    }
    QString file = path;
    if (file.isEmpty())
        file = QFileDialog::getSaveFileName(this, "Save image", defaultName(m_settings.recordFolder, ".png"),
                                            "PNG (*.png);;JPEG (*.jpg);;Bitmap (*.bmp)");
    if (file.isEmpty())
        return false;
    if (!img.save(file)) {
        error("Cannot save " + file);
        return false;
    }
    return true;
}

bool MainWindow::saveCsv(const QString &path)
{
    const TraceSet &t = m_session.traces();
    if (t.live().isEmpty()) {
        error("No trace to save yet");
        return false;
    }
    QString file = path;
    if (file.isEmpty())
        file = QFileDialog::getSaveFileName(this, "Save CSV", defaultName(m_settings.recordFolder, ".csv"),
                                            "CSV files (*.csv)");
    if (file.isEmpty())
        return false;
    QString err, title = imageHeader();
    if (t.normalized())
        title += " (normalized, dB)";
    if (!writeTraceCsv(file, t.normalizedView(t.live()), m_settings.csv, &err, title)) {
        error("Cannot save " + file + ": " + err);
        return false;
    }
    return true;
}

void MainWindow::setRecording(bool on)
{
    if (on == m_recorder.active())
        return;
    QString err;
    if (on && !m_recorder.start(m_settings.recordFolder, m_settings.csv, &err)) {
        error("Cannot record: " + err);
        on = false;
    } else if (on) {
        statusBar()->showMessage("Recording to " + m_recorder.folder(), 5000);
    } else {
        m_recorder.stop();
    }
    QSignalBlocker block(ui->actionRecord);
    ui->actionRecord->setChecked(on);
}
