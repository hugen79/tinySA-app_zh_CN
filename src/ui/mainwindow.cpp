// MainWindow: construction, wiring, connection and sweep configuration.
// Traces/files: mainfiles.cpp. Test hooks: maintest.cpp.
#include "mainwindow.h"
#include "batterywindow.h"
#include "consolewindow.h"
#include "screenwindow.h"
#include "ui_mainwindow.h"
#include <QCloseEvent>
#include <QLabel>
#include <QMessageBox>
#include <QSerialPortInfo>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <cmath>

MainWindow::MainWindow(bool simulators, const QString &settingsPath, QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_settingsPath(settingsPath),
      m_simulators(simulators)
{
    ui->setupUi(this);
    ui->splitter->setStretchFactor(1, 1);

    m_stepLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_stepLabel);
    m_sweepsLabel = new QLabel(this);
    m_avgLabel = new QLabel(this);
    m_avgLabel->setObjectName("avgCountLabel");
    m_avgLabel->setToolTip("Sweeps in the average so far (restart with the button next to Avg)");
    m_rateLabel = new QLabel(this);
    m_batteryLabel = new QLabel(this);
    for (QLabel *l : {m_sweepsLabel, m_avgLabel, m_rateLabel, m_batteryLabel})
        statusBar()->addPermanentWidget(l);

    // --- sweep and device widgets -> session ---
    connect(ui->connectButton, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(ui->scanButton, &QPushButton::clicked, this, [this](bool on) {
        m_session.setScan(on ? DeviceLink::Continuous : DeviceLink::Off);
    });
    connect(ui->onceButton, &QPushButton::clicked, this, [this] { m_session.setScan(DeviceLink::Once); });
    for (QLineEdit *e : {ui->startEdit, ui->stopEdit, ui->centerEdit, ui->spanEdit})
        connect(e, &QLineEdit::editingFinished, this, [this, e] { editRange(e); });
    connect(ui->pointsCombo, &QComboBox::activated, this, [this] {
        editConfig([this](SweepConfig &c) { c.grid.points = ui->pointsCombo->currentText().toInt(); });
    });
    connect(ui->rbwCombo, &QComboBox::activated, this, [this] {
        editConfig([this](SweepConfig &c) { c.rbwKHz = ui->rbwCombo->currentData().toDouble(); });
    });
    connect(ui->attenCombo, &QComboBox::activated, this, [this] {
        editConfig([this](SweepConfig &c) { c.atten = ui->attenCombo->currentData().toInt(); });
    });
    connect(ui->inputCombo, &QComboBox::activated, this, [this](int i) {
        editConfig([this, i](SweepConfig &c) {
            c.input = i ? InputMode::High : InputMode::Low;
            if (const ModelDesc *m = m_session.device().model) {  // full range of that input
                c.grid.startHz = m->lowLimit(c.input);
                c.grid.stopHz = m->highLimit(c.input);
            }
        });
    });
    connect(ui->lnaCheck, &QCheckBox::clicked, this, [this](bool on) {
        editConfig([on](SweepConfig &c) { c.lna = on; });
    });
    connect(ui->analyzerGainSpin, &QDoubleSpinBox::valueChanged, this, [this](double db) {
        editConfig([db](SweepConfig &c) { c.extGainDb = db; });
    });
    connect(ui->deviceRangeButton, &QPushButton::clicked, this, [this] {
        const ScanGrid g = m_session.device().deviceGrid;
        if (g.points > 1)
            editConfig([g](SweepConfig &c) { c.grid = {g.startHz, g.stopHz, g.points, c.grid.log}; });
    });
    connect(ui->logCheck, &QCheckBox::clicked, this, [this](bool on) {
        editConfig([on](SweepConfig &c) { c.grid.log = on; });
    });

    // --- trace and file widgets ---
    connect(ui->calcCombo, &QComboBox::activated, this, [this](int i) {
        m_session.traces().setHold(Hold(i));
        renderTraces(false);
    });
    connect(ui->avgCombo, &QComboBox::activated, this, [this](int i) {
        m_session.traces().setAverage(1 << i);
        renderTraces(false);
        showAverage();
    });
    connect(ui->avgRestartButton, &QToolButton::clicked, this, [this] {
        m_session.traces().restartAverage();
        showAverage();
    });
    ui->scaleCombo->addItems(levelUnitNames());  // in LevelUnit order
    connect(ui->scaleCombo, &QComboBox::activated, this, [this](int i) {
        ui->spectrum->setUnit(LevelUnit(i));
        renderMarkers();  // their readouts in the new unit
    });
    connect(ui->liveButton, &QToolButton::toggled, this, [this](bool on) { ui->spectrum->setShown(0, on); });
    setupMemoryButton(0, ui->m1Button);
    setupMemoryButton(1, ui->m2Button);
    setupMemoryButton(2, ui->m3Button);
    connect(ui->normButton, &QPushButton::toggled, this, [this](bool on) {
        m_session.traces().setNormalize(on);
        ui->spectrum->setNormalized(on);
        renderTraces(true);
        ui->spectrum->autoScale();
    });
    connect(ui->autoScaleButton, &QPushButton::clicked, ui->spectrum, &SpectrumView::autoScale);
    connect(ui->sweepNameEdit, &QLineEdit::textChanged, this, [this](const QString &t) { m_settings.sweepName = t; });
    connect(ui->actionSaveCsv, &QAction::triggered, this, [this] { saveCsv({}); });
    connect(ui->actionSaveImage, &QAction::triggered, this, [this] { saveImage({}); });
    connect(ui->actionCopyImage, &QAction::triggered, this, [this] { saveImage("clipboard:"); });
    connect(ui->actionRecord, &QAction::toggled, this, &MainWindow::setRecording);
    connect(ui->actionExit, &QAction::triggered, this, &QWidget::close);
    connect(ui->actionAbout, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("About tinySA-App"),
                           QString("<b>tinySA-App %1</b><p>PC software for the tinySA spectrum analyzers.</p>"
                                   "<p>GPL-3.0-or-later. Built with Qt %2 and QCustomPlot.</p>")
                               .arg(QCoreApplication::applicationVersion(), qVersion()));
    });

    setupMarkers();
    setupTools();
    setupGenerator();
    setupTracking();

    // --- graph intents ---
    connect(ui->spectrum, &SpectrumView::rangeRequested, this, &MainWindow::applyRangeOp);
    connect(ui->spectrum, &SpectrumView::saveImageRequested, this, [this] { saveImage({}); });
    connect(ui->spectrum, &SpectrumView::copyImageRequested, this, [this] { saveImage("clipboard:"); });
    connect(ui->spectrum, &SpectrumView::stopRequested, this, [this] { m_session.setScan(DeviceLink::Off); });
    auto shortcut = [this](QKeySequence k, auto fn) {
        auto *s = new QShortcut(k, this);
        connect(s, &QShortcut::activated, this, fn);
        return s;
    };
    const auto zoom = [this](double f) {
        const SweepConfig c = m_session.config();  // about the middle of the axis
        applyRangeOp(SpectrumView::RangeOp::Zoom, f,
                     c.grid.log ? std::sqrt(c.grid.startHz * c.grid.stopHz) : (c.grid.startHz + c.grid.stopHz) / 2);
    };
    m_analyzerShortcuts = {shortcut(Qt::Key_PageUp, [zoom] { zoom(1 / 1.5); }),
                           shortcut(Qt::Key_PageDown, [zoom] { zoom(1.5); })};
    shortcut(Qt::Key_Escape, [this] { escape(); });

    // --- session -> render ---
    connect(&m_session, &Session::connected, this, &MainWindow::onConnected);
    connect(&m_session, &Session::disconnected, this, &MainWindow::onDisconnected);
    connect(&m_session, &Session::configChanged, this, &MainWindow::showConfig);
    connect(&m_session, &Session::frame, this, &MainWindow::onFrame);
    connect(&m_session, &Session::tracesChanged, this, [this] { renderTraces(false); });
    connect(&m_session, &Session::scanChanged, this, &MainWindow::onScanChanged);
    connect(&m_session, &Session::message, this, [this](const QString &m) {
        statusBar()->showMessage(m, 5000);
        m_messages << m;
    });
    connect(&m_session, &Session::battery, this, [this](int mV) {
        m_batteryLabel->setText(QString("Battery %1 V").arg(mV / 1000.0, 0, 'f', 2));
    });
    connect(&m_session, &Session::traffic, this, [this](bool sent, const QString &text) {
        if (sent && m_sent.size() < 10000)
            m_sent << text;
    });

    // --- restore ---
    QSettings qs(m_settingsPath, QSettings::IniFormat);
    m_settings.config.grid = {0, 350e6, 450};
    m_settings.load(qs);
    restoreGeometry(qs.value("window/geometry").toByteArray());
    ui->splitter->restoreState(qs.value("window/splitter").toByteArray());
    ui->spectrum->setView(m_settings.view);
    ui->scaleCombo->setCurrentIndex(int(m_settings.view.unit));
    ui->liveButton->setChecked(m_settings.view.show[0]);
    ui->m1Button->setChecked(m_settings.view.show[1]);
    ui->m2Button->setChecked(m_settings.view.show[2]);
    ui->m3Button->setChecked(m_settings.view.show[3]);
    m_session.traces().setHold(Hold(std::clamp(m_settings.hold, 0, 2)));
    m_session.traces().setAverage(m_settings.average);
    ui->calcCombo->setCurrentIndex(std::clamp(m_settings.hold, 0, 2));
    ui->avgCombo->setCurrentIndex(std::clamp(int(std::log2(std::max(1, m_settings.average))), 0, 6));
    showAverage();
    ui->sweepNameEdit->setText(m_settings.sweepName);
    m_screen->setInvert(m_settings.screenInvert);
    m_console->setMacros(m_settings.macroNames, m_settings.macroCommands);
    m_console->setHistory(m_settings.history);
    applyAppearance();
    restoreState(qs.value("window/state").toByteArray());
    ui->graphSplitter->restoreState(qs.value("window/graph_splitter").toByteArray());
    ui->actionShowMarkers->setChecked(qs.value("window/show_markers", true).toBool());
    m_session.markers().deserialize(m_settings.markers);
    for (int m = 0; m < TraceSet::kMemories; ++m)
        if (QFile::exists(memoryFile(m)))
            loadMemory(m, memoryFile(m));
    applyModel(nullptr);
    m_session.setScreenOffWhileScanning(m_settings.screenOffScanning);
    m_session.setTrack(m_settings.track);
    showTrack(m_session.track());
    m_session.setConfig(m_settings.config);
    showConfig(m_session.config());
    onScanChanged(DeviceLink::Off);
    m_session.setGenerator(m_settings.gen);
    ui->generatorPanel->showConfig(m_session.generator());
    showActivity(DeviceLink::Analyzer);

    refreshPorts();
    connect(&m_portPoll, &QTimer::timeout, this, &MainWindow::refreshPorts);
    m_portPoll.start(1000);  // hot-plug: cheap enough to poll
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent *e)
{
    setRecording(false);
    QSettings qs(m_settingsPath, QSettings::IniFormat);
    m_settings.config = m_session.config();
    m_settings.gen = m_session.generator();
    m_settings.track = m_session.track();
    m_settings.view = ui->spectrum->view();
    m_settings.hold = int(m_session.traces().hold());
    m_settings.average = m_session.traces().average();
    m_settings.markers = m_session.markers().serialize();
    m_settings.macroNames = m_console->macroNames();
    m_settings.macroCommands = m_console->macroCommands();
    m_settings.history = m_console->history();
    m_settings.save(qs);
    qs.setValue("window/geometry", saveGeometry());
    qs.setValue("window/splitter", ui->splitter->saveState());
    qs.setValue("window/state", saveState());
    qs.setValue("window/graph_splitter", ui->graphSplitter->saveState());
    qs.setValue("window/show_markers", ui->actionShowMarkers->isChecked());
    saveTools(qs);
    QString err;
    for (int m = 0; m < TraceSet::kMemories; ++m) {
        const Trace &t = m_session.traces().memory(m);
        if (t.isEmpty())
            QFile::remove(memoryFile(m));
        else
            writeTraceCsv(memoryFile(m), t, CsvFormat{}, &err, QString("M%1").arg(m + 1));
    }
    e->accept();
}

void MainWindow::refreshPorts()
{
    const auto ports = QSerialPortInfo::availablePorts();
    QStringList names;
    for (const auto &p : ports)
        names << p.portName();
    const QString connectedPort = m_session.device().port, trackerPort = m_session.tracker().port;
    if (m_session.isConnected() && !connectedPort.startsWith("sim:") && !names.contains(connectedPort)) {
        m_session.disconnectDevice();  // unplugged; the serial error may come later or never
        statusBar()->showMessage("tinySA unplugged", 5000);
    }
    if (m_session.isTrackerConnected() && !trackerPort.startsWith("sim:") && !names.contains(trackerPort))
        m_session.disconnectTracker();
    if (names == m_portNames && ui->portCombo->count())
        return;
    m_portNames = names;

    QStringList tinySAs;
    for (const auto &p : ports)
        if (DeviceLink::isTinySAUsbId(p.vendorIdentifier(), p.productIdentifier()))
            tinySAs << p.portName();
    // keep a tinySA selection; otherwise pick a (newly plugged) tinySA, not the other one's
    auto fill = [&](QComboBox *combo, const QString &saved, bool connected, const QString &other) {
        const QString current = combo->count() ? combo->currentData().toString() : saved;
        QSignalBlocker block(combo);
        combo->clear();
        for (const auto &p : ports)
            combo->addItem(tinySAs.contains(p.portName()) ? p.portName() + " - tinySA" : p.portName(), p.portName());
        if (m_simulators) {
            combo->addItem("Simulated tinySA", "sim:basic");
            combo->addItem("Simulated tinySA Ultra", "sim:ultra");
        }
        int at = combo->findData(current);
        const bool keep = at >= 0 && (tinySAs.contains(current) || current.startsWith("sim:"));
        QStringList free = tinySAs;
        free.removeAll(other);
        if (!keep && !free.isEmpty() && !connected)
            at = combo->findData(free.first());
        combo->setCurrentIndex(std::max(at, 0));
    };
    fill(ui->portCombo, m_settings.port, m_session.isConnected(), trackerPort);
    fill(ui->trackerPortCombo, m_settings.trackerPort, m_session.isTrackerConnected(),
         ui->portCombo->currentData().toString());
}

void MainWindow::connectTo(const QString &port)
{
    int at = ui->portCombo->findData(port);
    if (at < 0) {
        ui->portCombo->addItem(port, port);
        at = ui->portCombo->count() - 1;
    }
    ui->portCombo->setCurrentIndex(at);
    if (!ui->connectButton->isChecked())
        ui->connectButton->click();
}

void MainWindow::onConnectClicked(bool checked)
{
    if (checked) {
        ui->connectButton->setEnabled(false);
        ui->connectButton->setText(tr("Connecting..."));
        ui->deviceLabel->setText(tr("Connecting to ") + ui->portCombo->currentText());
        m_session.connectDevice(ui->portCombo->currentData().toString());
    } else {
        m_session.disconnectDevice();
    }
}

void MainWindow::onConnected(const DeviceInfo &info)
{
    m_settings.port = info.port;
    ui->connectButton->setEnabled(true);
    ui->connectButton->setChecked(true);
    ui->connectButton->setText(tr("Disconnect"));
    ui->portCombo->setEnabled(false);
    ui->deviceLabel->setText(info.title());
    setWindowTitle("tinySA-App - " + info.title());
    ui->deviceRangeButton->setEnabled(info.deviceGrid.points > 1);
    applyModel(info.model);
    onScanChanged(m_session.scanMode());
    ui->generatorPanel->setDevice(&info);
    ui->analyzerButton->setEnabled(true);
    ui->generatorButton->setEnabled(true);
    ui->firmwareWarningLabel->setVisible(info.directCommandRace());
}

void MainWindow::onDisconnected(const QString &reason)
{
    ui->connectButton->setEnabled(true);
    ui->connectButton->setChecked(false);
    ui->connectButton->setText(tr("Connect"));
    ui->portCombo->setEnabled(true);
    ui->deviceLabel->setText(tr("Not connected"));
    setWindowTitle("tinySA-App");
    ui->deviceRangeButton->setEnabled(false);
    m_batteryLabel->clear();
    onScanChanged(DeviceLink::Off);
    ui->generatorPanel->setDevice(nullptr);  // the page stays (disabled) until the unit is back
    ui->analyzerButton->setEnabled(false);
    ui->generatorButton->setEnabled(false);
    ui->firmwareWarningLabel->hide();
    statusBar()->showMessage(reason, 5000);
    m_messages << reason;
}

void MainWindow::applyModel(const ModelDesc *m)
{
    // before connecting, offer the widest lists; the session clamps on connect
    const QVector<double> rbw = m ? m->rbwKHz : QVector<double>{0.2, 1, 3, 10, 30, 100, 300, 600, 850};
    ui->rbwCombo->clear();
    ui->rbwCombo->addItem("Auto", 0.0);
    for (double k : rbw)
        ui->rbwCombo->addItem(k < 1 ? QString("%1 Hz").arg(k * 1000) : QString("%1 kHz").arg(k), k);
    ui->attenCombo->clear();
    ui->attenCombo->addItem("Auto", -1);
    for (int db = 0; db <= (m ? m->maxAtten : 30); ++db)
        ui->attenCombo->addItem(QString("%1 dB").arg(db), db);
    ui->inputLabel->setVisible(m && m->hasInputMode);
    ui->inputCombo->setVisible(m && m->hasInputMode);
    ui->lnaCheck->setVisible(m && m->hasLna);
    showConfig(m_session.config());
}

void MainWindow::onScanChanged(int mode)
{
    const bool on = m_session.isConnected();
    ui->scanButton->setChecked(mode == DeviceLink::Continuous);
    ui->scanButton->setText(mode == DeviceLink::Continuous ? "Stop" : "Scan");
    ui->scanButton->setEnabled(on);
    ui->onceButton->setEnabled(on && mode == DeviceLink::Off);
}

void MainWindow::onFrame(const SweepFrame &f)
{
    if (!f.complete)
        return;
    ++m_sweeps;
    m_sweepMs = f.seconds * 1000;
    if (m_cycle.isValid())
        m_cycleMs = m_cycle.restart();  // sweep-to-sweep time, incl. commands in between
    else
        m_cycle.start();
    m_sweepsLabel->setText(QString("Sweeps %1").arg(m_sweeps));
    showAverage();
    if (f.seconds > 0)
        m_rateLabel->setText(QString("%1 points/s").arg(std::lround(f.grid.points / f.seconds)));
    QString err;
    if (m_recorder.active() && !m_recorder.add(m_session.traces().live(), &err))
        error("Recording stopped: " + err);
}

void MainWindow::showConfig(const SweepConfig &c)
{
    ui->startEdit->setText(formatMHz(c.grid.startHz));
    ui->stopEdit->setText(formatMHz(c.grid.stopHz));
    ui->centerEdit->setText(formatMHz((c.grid.startHz + c.grid.stopHz) / 2));
    ui->spanEdit->setText(formatMHz(c.grid.stopHz - c.grid.startHz));
    const QString points = QString::number(c.grid.points);
    if (ui->pointsCombo->findText(points) < 0)  // e.g. the unit's own 450 points
        ui->pointsCombo->addItem(points);
    ui->pointsCombo->setCurrentText(points);
    ui->rbwCombo->setCurrentIndex(std::max(0, ui->rbwCombo->findData(c.rbwKHz)));
    ui->attenCombo->setCurrentIndex(std::max(0, ui->attenCombo->findData(c.atten)));
    ui->inputCombo->setCurrentIndex(c.input == InputMode::High ? 1 : 0);
    ui->lnaCheck->setChecked(c.lna);
    {
        const QSignalBlocker block(ui->analyzerGainSpin);
        ui->analyzerGainSpin->setValue(c.extGainDb);
    }
    ui->spectrum->setExtGain(c.extGainDb);
    ui->logCheck->setChecked(c.grid.log);
    ui->spectrum->setSpan(c.grid.startHz, c.grid.stopHz, c.grid.log);
    auto step = [](double hz) { return formatHz(hz, std::max(1.0, hz / 1000)); };
    m_stepLabel->setText(c.grid.log && c.grid.points > 2  // the first and the last step
                             ? "Step " + step(c.grid.step()) + " to " + step(c.grid.stepAt(c.grid.points - 2))
                             : "Step " + step(c.grid.step()));
}

void MainWindow::editConfig(const std::function<void(SweepConfig &)> &change)
{
    SweepConfig c = m_session.config();
    change(c);
    m_session.setConfig(c);
    showConfig(m_session.config());
}

void MainWindow::editRange(QLineEdit *edit)
{
    const auto hz = parseHz(edit->text(), 1e6);
    if (!hz)
        return showConfig(m_session.config());  // revert invalid input
    SpectrumView::RangeOp op = edit == ui->startEdit ? SpectrumView::RangeOp::Start
                               : edit == ui->stopEdit ? SpectrumView::RangeOp::Stop
                               : edit == ui->centerEdit ? SpectrumView::RangeOp::Center
                                                        : SpectrumView::RangeOp::Zoom;
    const SweepConfig now = m_session.config();
    if (op == SpectrumView::RangeOp::Zoom && !now.grid.log) {  // span: zoom about the center to the typed span
        const double span = now.grid.stopHz - now.grid.startHz;
        return applyRangeOp(op, span > 0 ? *hz / span : 1, (now.grid.startHz + now.grid.stopHz) / 2);
    }
    editConfig([&](SweepConfig &c) {  // typed values are taken as they are, no rounding
        FreqRange r = rangeOf(c);  // (center and span as shown: stop - start, about their mean)
        if (op == SpectrumView::RangeOp::Start) r.setStart(*hz);
        else if (op == SpectrumView::RangeOp::Stop) r.setStop(*hz);
        else if (op == SpectrumView::RangeOp::Zoom) r.setSpan(*hz);
        else r.setCenter(*hz);
        c.grid.startHz = r.start();
        c.grid.stopHz = r.stop();
    });
}

// All graph and keyboard range changes: clamped by FreqRange, rounded to a resolution
// that fits the span (so zooming does not produce 433.9173 MHz edges).
void MainWindow::applyRangeOp(SpectrumView::RangeOp op, double a, double b)
{
    editConfig([&](SweepConfig &c) {
        FreqRange r = rangeOf(c);
        const bool log = c.grid.log;  // log axis: zoom and centre in ratios, as drawn
        switch (op) {
        case SpectrumView::RangeOp::Set: r.set(a, b); break;
        case SpectrumView::RangeOp::Start: r.setStart(a); break;
        case SpectrumView::RangeOp::Stop: r.setStop(a); break;
        case SpectrumView::RangeOp::Center: log ? r.setCenterLog(a) : r.setCenter(a); break;
        case SpectrumView::RangeOp::Zoom: log ? r.zoomLog(a, b) : r.zoom(a, b); break;
        }
        if (log) {  // each end to a resolution of its own (10 kHz must not become 0)
            r.set(roundTo(r.start(), niceStep(r.start() / 1000)), roundTo(r.stop(), niceStep(r.stop() / 1000)));
        } else {
            const double res = niceStep(r.span() / 1000);
            r.set(roundTo(r.start(), res), roundTo(r.stop(), res));
        }
        c.grid.startHz = r.start();
        c.grid.stopHz = r.stop();
    });
}

// The sweep's range with the unit's limits (a log sweep starts at kLogMinHz at the lowest)
FreqRange MainWindow::rangeOf(const SweepConfig &c) const
{
    FreqRange r(c.grid.startHz, c.grid.stopHz);
    const ModelDesc *m = m_session.device().model;
    const double low = m ? m->lowLimit(c.input) : 0;
    r.setLimits(c.grid.log ? std::max(kLogMinHz, low) : low, m ? m->highLimit(c.input) : 100e9);
    return r;
}

void MainWindow::error(const QString &what)
{
    statusBar()->showMessage(what, 8000);
    m_messages << what;
}
