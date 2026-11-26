// MainWindow, tracking generator: the second tinySA's port and connection, Track and level.
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QSignalBlocker>
#include <cmath>

void MainWindow::setupTracking()
{
    connect(ui->trackerConnectButton, &QPushButton::clicked, this, [this](bool checked) {
        if (!checked)
            return m_session.disconnectTracker();
        ui->trackerConnectButton->setEnabled(false);
        ui->trackerConnectButton->setText(tr("Connecting..."));
        ui->trackerLabel->setText(tr("Connecting to ") + ui->trackerPortCombo->currentText());
        m_session.connectTracker(ui->trackerPortCombo->currentData().toString());
    });
    connect(ui->trackCheck, &QCheckBox::clicked, this, [this](bool on) {
        TrackConfig t = m_session.track();
        t.on = on;
        m_session.setTrack(t);
    });
    connect(ui->trackLevelSpin, &QDoubleSpinBox::valueChanged, this, [this](double dbm) {
        TrackConfig t = m_session.track();
        t.levelDbm = dbm;
        m_session.setTrack(t);
    });
    connect(ui->trackOutputCombo, &QComboBox::activated, this, [this] {
        const QString key = ui->trackOutputCombo->currentData().toString();
        TrackConfig t = m_session.track();
        t.mixer = key.endsWith("mixer");
        t.harmonic = key.startsWith("ultra");
        m_session.setTrack(t);
        showTrackerLabel();  // its top frequency
    });
    ui->trackOutputCombo->hide();  // an Ultra connected as tracking generator
    connect(ui->trackOffsetEdit, &QLineEdit::editingFinished, this, [this] {
        const QString text = ui->trackOffsetEdit->text().trimmed();
        const auto hz = text.isEmpty() ? std::optional<double>(0) : parseHz(text, 1e6);
        TrackConfig t = m_session.track();
        if (hz)
            t.offsetHz = *hz;
        m_session.setTrack(t);  // invalid input: shows the offset again
        showTrack(m_session.track());
        showTrackerLabel();
    });
    connect(&m_session, &Session::trackChanged, this, &MainWindow::showTrack);
    connect(&m_session, &Session::trackerConnected, this, [this](const DeviceInfo &info) {
        m_settings.trackerPort = info.port;
        ui->trackerConnectButton->setEnabled(true);
        ui->trackerConnectButton->setChecked(true);
        ui->trackerConnectButton->setText(tr("Disconnect"));
        ui->trackerPortCombo->setEnabled(false);
        const GenLimits l = trackerLimits(info, m_session.track());
        QSignalBlocker block(ui->trackLevelSpin);
        ui->trackLevelSpin->setRange(l.minDbm, l.maxDbm);
        {
            QSignalBlocker block2(ui->trackOutputCombo);
            ui->trackOutputCombo->clear();
            for (const UltraOutput &o : ultraOutputs(info)) {  // the name; its top in the label above
                ui->trackOutputCombo->addItem(o.text.section(',', 0, 0), o.key);
                ui->trackOutputCombo->setItemData(ui->trackOutputCombo->count() - 1, o.text, Qt::ToolTipRole);
            }
            ui->trackOutputCombo->setVisible(ui->trackOutputCombo->count() > 0);
        }
        showTrackerLabel();
        showTrack(m_session.track());
    });
    connect(&m_session, &Session::trackerAligned, this, &MainWindow::showTrackerLabel);
    connect(&m_session, &Session::trackerDisconnected, this, [this](const QString &reason) {
        ui->trackerConnectButton->setEnabled(true);
        ui->trackerConnectButton->setChecked(false);
        ui->trackerConnectButton->setText(tr("Connect"));
        ui->trackerPortCombo->setEnabled(true);
        ui->trackerLabel->setText(tr("Not connected"));
        ui->trackerLabel->setToolTip({});
        ui->trackOutputCombo->hide();
        showTrack(m_session.track());
        error(reason);
    });
    connect(&m_session, &Session::trackerTraffic, this, [this](bool sent, const QString &text) {
        if (sent && m_trackerSent.size() < 10000)
            m_trackerSent << text;
    });
}

// "tinySA Ultra ZS405, to 5.327 GHz, -430 ppb": the unit, its top frequency and its frequency
// error against the analyzer once measured (at the first tracking sweep)
void MainWindow::showTrackerLabel()
{
    const DeviceInfo &info = m_session.tracker();
    if (!m_session.isTrackerConnected() || !info.model)
        return;
    const GenLimits l = trackerLimits(info, m_session.track());
    const std::optional<double> ppb = m_session.trackerPpb();
    ui->trackerLabel->setText(QString("%1 %2, to %3%4").arg(info.model->name, info.hardware, formatHz(l.maxHz, 1e6),
                                                             ppb ? QString(", %1 ppb").arg(std::lround(*ppb)) : QString()));
    ui->trackerLabel->setToolTip(QString("%1\nOutput %2 to %3, %4 to %5 dBm\n%6")
                                     .arg(info.title(), formatHz(l.minHz, 1e3), formatHz(l.maxHz, 1e6))
                                     .arg(l.minDbm).arg(l.maxDbm)
                                     .arg(ppb ? QString("Its frequency is %1 ppb off this tinySA's: corrected").arg(*ppb, 0, 'f', 0)
                                              : QString("Its frequency error is measured at the first tracking sweep")));
}

void MainWindow::showTrack(const TrackConfig &t)
{
    QSignalBlocker b1(ui->trackCheck), b2(ui->trackLevelSpin), b3(ui->trackOutputCombo);
    ui->trackCheck->setChecked(t.on);
    ui->trackCheck->setEnabled(m_session.isTrackerConnected());
    ui->trackLevelSpin->setValue(t.levelDbm);
    ui->trackOutputCombo->setCurrentIndex(std::max(0, ui->trackOutputCombo->findData(ultraOutputKey(t.mixer, t.harmonic))));
    ui->trackOffsetEdit->setText(t.offsetHz == 0 ? QString()  // "+500 MHz"; empty = none (placeholder "Offset MHz")
                                                 : (t.offsetHz > 0 ? "+" : "") + formatMHz(t.offsetHz) + " MHz");
}

void MainWindow::connectTrackerTo(const QString &port)
{
    int at = ui->trackerPortCombo->findData(port);
    if (at < 0) {
        ui->trackerPortCombo->addItem(port, port);
        at = ui->trackerPortCombo->count() - 1;
    }
    ui->trackerPortCombo->setCurrentIndex(at);
    if (!ui->trackerConnectButton->isChecked())
        ui->trackerConnectButton->click();
}
