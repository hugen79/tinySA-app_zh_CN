#include "link.h"
#include "simdevice.h"
#include <QPointer>
#include <QRegularExpression>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <cmath>
#include <utility>

DeviceLink::DeviceLink(QObject *parent)
    : QObject(parent), m_timeout(this), m_battery(this), m_drain(this), m_settle(this), m_trackSettle(this),
      m_screenTimer(this), m_touchTimer(this)
{
    m_settle.setSingleShot(true);
    connect(&m_settle, &QTimer::timeout, this, &DeviceLink::pump);
    m_trackSettle.setSingleShot(true);
    m_trackSettle.setTimerType(Qt::PreciseTimer);  // ms waits: not on the Windows tick (15.6 ms)
    connect(&m_trackSettle, &QTimer::timeout, this, &DeviceLink::measureTrackPoint);
    m_touchTimer.setInterval(30);
    connect(&m_touchTimer, &QTimer::timeout, this, [this] {
        if (m_pendingTouch.isEmpty())
            m_touchTimer.stop();
        flushTouch();
    });
    m_screenTimer.setSingleShot(true);
    connect(&m_screenTimer, &QTimer::timeout, this, [this] { emit screen(m_screen); });
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, &DeviceLink::onTimeout);
    m_battery.setInterval(6000);
    connect(&m_battery, &QTimer::timeout, this, [this] {
        m_batteryDue = true;
        pump();
    });
    m_drain.setSingleShot(true);
    connect(&m_drain, &QTimer::timeout, this, [this] {
        m_draining = false;
        m_rx.reset();
        pump();
    });
}

DeviceLink::~DeviceLink()
{
    shutdown({});
}

bool DeviceLink::isTinySAUsbId(quint16 vid, quint16 pid)
{
    return (vid == 0x0483 && pid == 0x5740)     // ST virtual COM port: tinySA, older Ultra firmware
        || (vid == 0x1781 && pid == 0x0A44);    // newer tinySA Ultra firmware
}

QString DeviceLink::findPort()
{
    for (const auto &pi : QSerialPortInfo::availablePorts())
        if (isTinySAUsbId(pi.vendorIdentifier(), pi.productIdentifier()))
            return pi.portName();
    return {};
}

void DeviceLink::open(const QString &port)
{
    shutdown({});
    m_info = DeviceInfo{};
    m_info.port = port;
    m_have = AppliedConfig{};
    m_genHave = AppliedGen{};
    m_activity = Analyzer;
    m_uiText.clear();
    m_uiTextOk = true;  // until the firmware answers ui off "text" with its usage
    // sim:basic, sim:ultra; ":old" = v1.4-220 ZS405, ":gen" = left generating; ":thru" / ":lpf" =
    // input cabled to the other simulated unit's output (directly / through a 150 MHz low-pass);
    // ":ppb=N" = its output frequency N ppb off; ":ctcss" = its CTCSS tone on, ":noctcss" =
    // firmware without `modulation ctcss` (v1.4-317); ":extgain=N" = its ext_gain N dB;
    // ":mixer=M" = input through a mixer with its LO at M MHz (sees |f - LO| and f + LO, 7 dB loss)
    if (port.startsWith("sim:")) {
        auto *sim = new SimTransport(port.contains("basic") ? SimTransport::Kind::Basic
                                                            : SimTransport::Kind::Ultra, this);
        sim->oldFirmware = port.contains(":old");
        sim->ctcssCommand = !port.contains(":noctcss");  // ":noctcss": firmware before v1.4-341
        sim->ctcssTone = port.contains(":ctcss");        // ":ctcss": its CTCSS + 1 kHz tone is on
        static const QRegularExpression extGain(R"(:extgain=(-?\d+(?:\.\d+)?))");  // its ext_gain
        if (const auto m = extGain.match(port); m.hasMatch())
            sim->setExtGain(m.captured(1).toDouble());
        sim->dut = port.contains(":lpf") ? SimTransport::Dut::LowPass
                   : port.contains(":thru") ? SimTransport::Dut::Thru : SimTransport::Dut::None;
        static const QRegularExpression mixer(R"(:mixer=(\d+(?:\.\d+)?))");  // through a mixer, LO in MHz
        if (const auto m = mixer.match(port); m.hasMatch()) {
            sim->dut = SimTransport::Dut::Mixer;
            sim->mixerLoHz = m.captured(1).toDouble() * 1e6;
        }
        static const QRegularExpression ppb(R"(:ppb=(-?\d+))");  // its generator's frequency error
        if (const auto m = ppb.match(port); m.hasMatch())
            sim->ppb = m.captured(1).toDouble();
        if (port.contains(":gen"))
            sim->startGenerating();
        sim->open(QIODevice::ReadWrite | QIODevice::Unbuffered);
        m_io = sim;
    } else {
        auto *sp = new QSerialPort(port, this);
        sp->setBaudRate(576000);
        if (!sp->open(QIODevice::ReadWrite)) {
            const QString err = sp->errorString();
            delete sp;
            emit closed(QString("Cannot open %1: %2").arg(port, err));
            return;
        }
        // Queued: a failing write reports its error from inside write() or waitForBytesWritten(),
        // where closing the port would pull it from under the caller.
        connect(sp, &QSerialPort::errorOccurred, this, [this, port = QPointer<QSerialPort>(sp)](QSerialPort::SerialPortError e) {
            if ((e == QSerialPort::ResourceError || e == QSerialPort::PermissionError) && port && m_io == port)
                fail("Connection lost");
        }, Qt::QueuedConnection);
        sp->clear(QSerialPort::Input);
        m_io = sp;
    }
    connect(m_io, &QIODevice::readyRead, this, &DeviceLink::onReadyRead);
    // A scan left running by an earlier session: stop it and drop its output.
    m_io->write("abort\r");
    resync();
    m_battery.start();
    handshake();
}

void DeviceLink::handshake()
{
    enqueue({"version", {}, 0, 3000, [this](bool ok, const QStringList &lines) {
        for (const QString &l : lines) {
            const int at = l.indexOf("tinySA");  // skip junk left in the line
            if (at >= 0 && m_info.version.isEmpty())
                m_info.version = l.mid(at).trimmed();
            if (l.startsWith("HW Version:"))
                m_info.hardware = l.mid(11).trimmed();
        }
        if (!ok)
            fail("No reply from " + m_info.port);
    }});
    enqueue({"info", {}, 0, 3000, [this](bool ok, const QStringList &lines) {
        m_info.info = lines.join('\n');
        m_info.model = ok ? detectModel(m_info.info, m_info.version) : nullptr;
        if (!m_info.model) {
            fail(QString("No tinySA on %1 %2").arg(m_info.port, m_info.version));
            return;
        }
        static const QRegularExpression zs(R"(ZS\d{3})");
        const auto m = zs.match(lines.value(0));
        if (m.hasMatch())
            m_info.hardware = m.captured(0);
    }});
    enqueue({"help", {}, 0, 3000, [this](bool, const QStringList &lines) {
        m_info.commands = parseHelp(lines);
    }});
    enqueue({"zero", {}, 0, 3000, [this](bool, const QStringList &lines) {
        m_info.zeroDbm = parseZero(lines, m_info.model->defaultZero);
    }});
    enqueue({"sweep", {}, 0, 3000, [this](bool ok, const QStringList &lines) {
        const QStringList v = lines.value(0).split(' ', Qt::SkipEmptyParts);  // "start stop points"
        if (ok && v.size() >= 3)
            m_info.deviceGrid = {v[0].toDouble(), v[1].toDouble(), v[2].toInt()};
    }});
    enqueue(extGainRead());
    // mode probe: `level` without a value prints its usage only in output mode
    enqueue({"level", {}, 0, 3000, [this](bool ok, const QStringList &lines) {
        if (ok)
            probeGenerator(lines);
    }});
    // burst modulation (Ultra firmware from v1.4-305): `modulation` alone lists it in its usage
    enqueue({"modulation", {}, 0, 3000, [this](bool ok, const QStringList &lines) {
        m_info.hasBurst = ok && lines.join(' ').contains("burst");
        // the CTCSS + 1 kHz tone (Ultra firmware from v1.4-341): the unit's state, before the
        // rest of the handshake
        if (ok && lines.join(' ').contains("ctcss"))
            m_queue.prepend(Cmd{"modulation ctcss", {}, 0, 3000, [this](bool ok, const QStringList &lines) {
                const QString state = lines.value(0).trimmed();
                if (ok && (state == "on" || state == "off"))
                    m_info.ctcssTone = state == "on";
            }});
    }});
    enqueue({"vbat_offset", {}, 0, 3000, [this](bool ok, const QStringList &lines) {
        bool num = false;
        const int v = lines.value(0).trimmed().toInt(&num);
        if (ok && num)
            m_info.vbatOffset = v;
    }});
    enqueue({"", {}, 0, 0, [this](bool, const QStringList &) {
        m_want = clampConfig(m_want, *m_info.model);
        m_gen = clampGen(m_gen, m_info);
        m_gen.ctcss = m_genHave.ctcss = m_info.ctcssTone;  // the unit's, not the last unit's
        // the app sends its own external gain: say so when the unit had another one (a unit
        // that generates is followed: the app takes its value)
        if (!m_isTracker && !m_info.gen.active && m_info.extGainDb && std::abs(*m_info.extGainDb - m_want.extGainDb) >= 0.05)
            emit message(QString("The tinySA's external gain was %1 dB: the app sets its own, %2 dB (Ext. gain)")
                             .arg(QString::asprintf("%+.1f", *m_info.extGainDb), QString::asprintf("%+.1f", m_want.extGainDb)));
        // left off by an app that did not close properly (no-op when on); a tracking generator's
        // goes off right away
        if (m_info.has("ui") && !m_isTracker)
            m_queue.prepend(Cmd{"ui on"});
        m_ready = true;
        m_batteryDue = true;
        emit opened(m_info);
        adoptGenerator();
        if (m_isTracker) {  // its role: output mode, RF off until the analyzer scans
            m_gen = trackerGen(m_gen, m_info, m_track, m_gen.freqHz, m_gen.levelDbm, m_trackRf);
            if (m_activity != Generator)
                setActivity(Generator);
        }
    }});
}

void DeviceLink::close()
{
    shutdown("Disconnected");
}

void DeviceLink::fail(const QString &reason)
{
    shutdown(reason);
}

void DeviceLink::shutdown(const QString &reason)
{
    m_timeout.stop();
    m_battery.stop();
    m_drain.stop();
    m_settle.stop();
    const bool wasOpen = m_io != nullptr;
    if (QIODevice *io = std::exchange(m_io, nullptr)) {
        io->disconnect(this);  // nothing from the port gets back in here while it closes
        auto *sp = qobject_cast<QSerialPort *>(io);
        const bool gone = sp && (sp->error() == QSerialPort::ResourceError
                                 || sp->error() == QSerialPort::PermissionError);
        // Leave the unit sweeping on its own: stop our scan (the shell reads the abort
        // within 5 s) and undo our pause. Both wait in the unit's input buffer if needed.
        // Not when the unit has left USB (unplugged, restarting in DFU mode).
        if (!gone) {
            if (m_cur && m_cur->body == RxParser::Body::ScanRaw && !m_scanDone && !m_aborting)
                io->write("abort\r");
            if (m_remote)
                io->write("refresh off\r");
            if (m_trackRf && m_ready)  // tracking generator: no signal into the DUT after us
                io->write("output off\r");
            if (m_uiOff && m_ready)
                io->write("ui on\r");
            if (m_paused && m_ready)
                io->write("resume\r");
            if (sp)
                sp->waitForBytesWritten(200);
        }
        io->close();
        io->deleteLater();
    }
    m_queue.clear();
    m_cur.reset();
    m_rx.reset();
    m_ready = m_paused = m_draining = m_remote = m_uiOff = m_trackRf = false;
    m_sweepPiece = -1;
    stopTracking(true);  // analyzer: the generator's RF off
    m_touchTimer.stop();
    m_pendingTouch.clear();
    if (m_mode != Off) {
        m_mode = Off;
        emit scanChanged(Off);
    }
    updateBusy();
    if (wasOpen && !reason.isEmpty())  // once: the port poll may ask again after a lost connection
        emit closed(reason);
}

void DeviceLink::setConfig(const SweepConfig &c)
{
    m_want = m_info.model ? clampConfig(c, *m_info.model) : c;
    pump();
}

void DeviceLink::setScan(int mode)
{
    setMode(mode);
}

void DeviceLink::setMode(int mode)
{
    if (mode == m_mode || (mode != Off && m_activity == Generator))  // a generating unit cannot scan
        return;
    m_mode = mode;
    emit scanChanged(mode);
    auto dropQueued = [this](const char *line) {  // "ui off" also drops `ui off "text"`
        return m_queue.removeIf([line](const Cmd &c) { return c.line.startsWith(line); }) > 0;
    };
    if (mode == Off) {
        // Stop now rather than after a possibly minutes-long sweep. The shell reads the
        // abort when the scan ends or within 5 s; its echo inside the data is tolerated.
        if (m_cur && m_cur->body == RxParser::Body::ScanRaw && !m_scanDone && !m_aborting) {
            m_aborting = true;
            m_io->write("abort\r");
        }
        stopTracking(true);
        m_sweepPiece = -1;
        dropQueued("pause");
        dropQueued("ui off");
        if (m_uiOff)  // the unit's screen and its own sweeping back
            enqueue({"ui on"});
        if (m_paused)
            enqueue({"resume"});
    } else {
        dropQueued("resume");  // scanning again before the resume went out
        dropQueued("ui on");
    }
    pump();
}

void DeviceLink::setScreenOffWhileScanning(bool on)
{
    m_screenOffWanted = on;
    pump();  // while scanning: switched at once, between two sweeps
}

void DeviceLink::send(const QString &command)
{
    Cmd c{command.toLatin1()};
    if (command.trimmed() == "capture" && m_info.model)
        return capture();
    c.done = [this, command](bool, const QStringList &lines) { emit reply(command, lines); };
    c.user = true;
    enqueue(c);
}

void DeviceLink::capture()
{
    if (!m_info.model)
        return;
    // RLE8 (DiSlord) when the firmware has it; older firmware ignores the argument and
    // sends raw RGB565, which the parser recognises by the missing RLE8 header.
    const QSize lcd = m_info.model->lcd;
    enqueue({"capture rle", RxParser::Body::Capture, lcd.width() * lcd.height() * 2, 5000,
             [this, lcd](bool ok, const QStringList &) {
        if (!ok)
            return;
        QImage img(lcd, QImage::Format_RGB16);
        if (m_pixelsRle8) {
            if (!decodeRle8(m_pixels, img) || img.size() != lcd)
                return emit message("Screen capture: bad RLE8 data");
        } else if (m_pixels.size() == lcd.width() * lcd.height() * 2) {
            blitRgb565BE(img, img.rect(), m_pixels);
        } else {
            return;
        }
        emit traffic(false, QString("<screen %1x%2, %3 %4 bytes, %5 ms>").arg(lcd.width()).arg(lcd.height())
                                .arg(m_pixelsRle8 ? "RLE8" : "raw").arg(m_pixels.size()).arg(m_sentClock.elapsed()),
                     false);
        m_screen = img;
        emit screen(m_screen);
    }});
}

// While the unit streams screen pushes it still echoes typed commands, but from another
// thread: a push can land inside the echo. So in remote mode touches and `refresh off`
// are sent without waiting for echo and prompt, and input is drained afterwards.
void DeviceLink::setRemote(bool on)
{
    if (!m_ready || on == m_remote)
        return;
    if (on) {
        m_remote = true;
        m_modeBeforeRemote = m_mode;
        setMode(Off);  // resumes the unit's own sweeping and display
        capture();     // full picture first, then only the changes
        // compressed pushes and a scrolling waterfall; firmware without them answers with usage
        enqueue({"refresh rle", {}, 0, 3000, [this](bool, const QStringList &lines) {
            if (m_remote && lines.join(' ').contains("usage"))
                enqueue({"refresh on"});
        }});
    } else {
        enqueue({"", {}, 0, 0, [this](bool, const QStringList &) {  // after anything in flight
            flushTouch();
            m_touchTimer.stop();
            writeDirect("refresh off");
            m_remote = false;
            resync();  // pushes still in the pipe; their echo too
        }});
        setMode(m_modeBeforeRemote);
    }
}

void DeviceLink::writeDirect(const QByteArray &line)
{
    m_io->write(line + "\r");
    emit traffic(true, QString::fromLatin1(line), false);
}

void DeviceLink::flushTouch()
{
    if (!m_pendingTouch.isEmpty() && m_io)
        writeDirect(std::exchange(m_pendingTouch, {}));
}

void DeviceLink::touch(QPoint lcd, bool down)
{
    if (!m_ready)
        return;
    const QByteArray line = down ? "touch " + QByteArray::number(lcd.x()) + " " + QByteArray::number(lcd.y())
                                 : QByteArray("release");
    if (m_remote) {  // live screen: no echo wait; drags at most every 30 ms
        if (!down) {
            flushTouch();
            writeDirect(line);
        } else if (!m_touchTimer.isActive()) {
            writeDirect(line);
            m_touchTimer.start();
        } else {
            m_pendingTouch = line;
        }
        return;
    }
    if (down)  // dragging: update a touch that is still waiting instead of queueing more
        for (Cmd &c : m_queue)
            if (c.line.startsWith("touch ")) {
                c.line = line;
                return;
            }
    enqueue({line});
}

void DeviceLink::simTouch()
{
    if (auto *sim = qobject_cast<SimTransport *>(m_io))
        sim->touchScreen();
}

void DeviceLink::enqueue(Cmd c)
{
    m_queue.append(std::move(c));
    pump();
}

void DeviceLink::pump()
{
    pumpStep();
    updateBusy();
}

void DeviceLink::updateBusy()
{
    const bool busy = m_cur || !m_queue.isEmpty() || m_draining || m_settle.isActive();
    if (busy != m_busy) {
        m_busy = busy;
        emit busyChanged(busy);
    }
}

void DeviceLink::pumpStep()
{
    if (!m_io || m_cur || m_draining || m_settle.isActive())
        return;
    if (!m_queue.isEmpty())
        return transmit(m_queue.takeFirst());
    if (!m_ready)
        return;
    // no polls while the screen streams, nor while generating anything but a carrier:
    // `vbat` restarts the unit's sweep and interrupts its modulation
    const bool quietGenerator = m_activity == Generator && m_gen.type != GenType::CW;
    if (m_batteryDue && !m_remote && !quietGenerator) {
        m_batteryDue = false;
        return enqueue({"vbat", {}, 0, 3000, [this](bool ok, const QStringList &lines) {
            static const QRegularExpression re(R"((\d+)\s*mV)");
            const auto m = re.match(lines.value(0));
            if (ok && m.hasMatch())
                emit battery(m.captured(1).toInt());
        }});
    }
    if (m_activity == Generator)
        return pumpGenerator();
    if (m_mode == Off)
        return;
    if (!m_paused)  // the unit's own sweeping would compete with scanraw
        return enqueue({"pause"});
    // screen off: no redraw after every command (firmware with `ui`, tinySA Ultra v1.4-314)
    const bool uiOff = m_screenOffWanted && m_info.has("ui");
    if (uiOff != m_uiOff || (uiOff && uiTextDue()))
        return enqueue(uiCommand(uiOff));
    SweepConfig want = m_want;
    if (tracking() && want.rbwKHz <= 0)  // the firmware's auto RBW misreads single points
        want.rbwKHz = kTrackAutoRbwKHz;
    const QByteArrayList cmds = configCommands(want, m_have, *m_info.model);
    if (!cmds.isEmpty()) {
        m_sweepPiece = -1;    // a sweep starts again with the new settings
        stopTracking(false);  // a tracking sweep too
        return queueChecked(cmds);
    }
    if (m_trackWait || m_aligning)  // the generator unit is being tuned or measured
        return;
    if (tracking())
        return pumpTracking();
    stopTracking(true);  // tracking wanted but its unit is gone: ordinary sweeps
    startSweep();
}

void DeviceLink::queueChecked(const QByteArrayList &lines, bool generator)
{
    for (const QByteArray &c : lines) {
        Cmd cmd{c, {}, 0, 3000, [this, c, generator](bool ok, const QStringList &reply) {
            const QString l = reply.value(0);
            if (l.endsWith('?') || l.startsWith("usage"))  // unknown command / bad value
                emit message(QString("tinySA refused '%1': %2").arg(QString::fromLatin1(c), l));
            else if (!ok && generator && m_activity == Generator)
                forgetGenCommand(c, m_genHave);
        }};
        cmd.generator = generator;
        m_queue.append(cmd);
    }
    pump();
}

// A sweep is one `scanraw`, a log sweep one per linear piece (ScanGrid): the pieces fill one
// frame in turn, with queued commands (not settings: they start the sweep again) in between.
void DeviceLink::startSweep()
{
    m_aborting = false;
    ScanGrid g = m_want.grid;
    if (g.log)
        g.pieces = logPieces(g.startHz, g.stopHz, g.points);
    if (m_sweepPiece < 0 || !(m_frame.grid == g) || m_frame.complete) {  // a new sweep
        m_frame = SweepFrame{m_frame.seq + 1, g, QVector<double>(g.points, qQNaN())};
        m_sweepPiece = 0;
        m_sweepClock.start();
        m_emitClock.start();
    }
    const int k = m_sweepPiece;
    const ScanGrid p = g.piece(k);
    m_frame.received = g.pieceStart(k);  // its values land from here on
    Cmd c;
    c.line = QString("scanraw %1 %2 %3").arg(std::llround(p.startHz)).arg(p.scanrawStop())
                 .arg(p.points).toLatin1();
    c.body = RxParser::Body::ScanRaw;
    c.points = p.points;
    c.sweep = true;
    c.timeoutMs = 15000;  // inactivity watchdog, restarted by every data chunk
    c.done = [this, k, seq = m_frame.seq](bool, const QStringList &lines) {
        if (seq != m_frame.seq || m_sweepPiece != k)
            return;
        const ScanGrid &g = m_frame.grid;
        if (m_frame.received == g.pieceStart(k) && !lines.isEmpty()) {  // refused, e.g. range invalid
            m_sweepPiece = -1;
            emit message("Scan failed: " + lines.join(' '));
            setMode(Off);
        } else if (m_frame.received < g.pieceStart(k + 1)) {  // stopped or lost: from the start again
            m_sweepPiece = -1;
        } else if (k + 1 < g.pieceCount()) {  // the next piece
            m_sweepPiece = k + 1;
            emit frame(m_frame);
        } else {
            m_sweepPiece = -1;
            if (m_mode == Once)
                setMode(Off);
        }
    };
    transmit(c);
}

void DeviceLink::transmit(Cmd c)
{
    if (c.line.isEmpty()) {  // local step in a command sequence
        if (c.done)
            c.done(true, {});
        return pump();
    }
    m_echoSeen = m_scanDone = false;
    m_lines.clear();
    m_pixels.clear();
    m_rx.expect(c.line, c.body, c.bytes);
    m_io->write(c.line + "\r");
    m_sentClock.start();
    if (c.line == "pause" || c.line == "resume")  // the unit's state from now on
        m_paused = c.line == "pause";
    if (c.line.startsWith("ui off") || c.line == "ui on") {
        m_uiOff = c.line != "ui on";
        if (c.line.startsWith("ui off \""))  // ui off "text"; a plain ui off keeps the text
            m_uiText = c.line.mid(8).chopped(1);
    }
    if (c.body == RxParser::Body::ScanRaw) {
        m_scanExpect = c.points;
        m_alignScan = c.align;
        m_alignValues.clear();
        m_aborting = false;
    } else {
        emit traffic(true, QString::fromLatin1(c.line), c.user);
    }
    m_timeout.start(c.timeoutMs);
    m_cur = std::move(c);
}

void DeviceLink::onReadyRead()
{
    if (!m_io)
        return;
    const QByteArray bytes = m_io->readAll();
    if (m_draining) {
        if (m_drainClock.elapsed() < 3000)
            m_drain.start(150);  // drain until the line has been quiet for a while
        return;
    }
    for (const RxEvent &e : m_rx.feed(bytes)) {
        handle(e);
        if (!m_io)
            return;
    }
}

void DeviceLink::handle(const RxEvent &e)
{
    const bool scanning = m_cur && m_cur->body == RxParser::Body::ScanRaw;
    switch (e.type) {
    case RxEvent::Echo:
        m_echoSeen = true;
        break;
    case RxEvent::Line:
        // "ui on" that is no reply to our own ui command: a touch or the jog on the unit gave
        // its screen back, the user takes over (as a touch that ends a scan)
        if (e.data == "ui on" && m_uiOff && !(m_cur && m_cur->line.startsWith("ui"))) {
            m_uiOff = false;
            m_queue.removeIf([](const Cmd &c) { return c.line == "ui on"; });
            if (m_mode != Off) {
                emit message("Scan stopped on the tinySA");
                setMode(Off);
            }
            if (m_isTracker) {  // the tracking generator: the user's until the next scan
                m_trackTouched = true;
                if (m_trackRf) {  // the analyzer stops, its RF goes off
                    emit trackerStopped();
                    if (m_trackRf)
                        setTrackOutput(false, m_gen.levelDbm, m_gen.freqHz);
                }
            }
        }
        emit traffic(false, QString::fromLatin1(e.data), m_cur && m_cur->user && m_echoSeen);
        if (m_cur && m_echoSeen)
            m_lines << QString::fromLatin1(e.data);
        break;
    case RxEvent::Prompt:
        if (m_cur && m_echoSeen)  // prompts before our echo are stale
            finish(true);
        break;
    case RxEvent::ScanData:
        if (!scanning)
            break;
        m_timeout.start(m_cur->timeoutMs);
        if (m_alignScan) {  // not a sweep: no frame
            for (qint16 v : e.values)
                m_alignValues << v / 32.0 - m_info.zeroDbm;
            break;
        }
        for (qint16 v : e.values)
            if (m_frame.received < m_frame.dBm.size())
                m_frame.dBm[m_frame.received++] = v / 32.0 - m_info.zeroDbm;
        if (m_emitClock.elapsed() > 50) {  // live progress for slow sweeps
            m_emitClock.restart();
            emit frame(m_frame);
        }
        break;
    case RxEvent::ScanEnd:
        if (!scanning)
            break;
        if (e.count < m_scanExpect) {  // our abort, or touch/jog: no prompt follows
            m_scanDone = true;
            if (!m_aborting) {
                emit message("Scan stopped on the tinySA");
                setMode(Off);
            }
            finish(true);
        } else {
            if (m_cur->sweep && m_frame.received >= m_frame.grid.points) {  // a whole sweep (its last piece)
                m_frame.complete = true;
                m_frame.seconds = m_sweepClock.elapsed() / 1000.0;
                emit frame(m_frame);
            }
            m_scanDone = true;
            m_timeout.start(500);  // the prompt follows at once unless aborted late
        }
        break;
    case RxEvent::Pixels:
        m_pixels = e.data;
        m_pixelsRle8 = e.rle8;
        break;
    case RxEvent::Region:  // pushed screen change (refresh on / rle)
        if (m_info.model && m_screen.size() != m_info.model->lcd) {
            m_screen = QImage(m_info.model->lcd, QImage::Format_RGB16);
            m_screen.fill(Qt::black);
        }
        if (e.scroll)
            scrollRgb16(m_screen, e.rect, qint16(quint8(e.data[0]) | quint8(e.data[1]) << 8));
        else if (e.fill)
            fillRgb565BE(m_screen, e.rect, e.data);
        else if (e.rle8)
            blitRle8(m_screen, e.rect, e.data);
        else
            blitRgb565BE(m_screen, e.rect, e.data);
        if (!m_screenTimer.isActive())
            m_screenTimer.start(40);
        break;
    }
}

void DeviceLink::finish(bool ok)
{
    m_timeout.stop();
    Cmd c = std::move(*m_cur);
    m_cur.reset();
    if (c.done)
        c.done(ok, m_lines);
    // Old firmware: after these commands the unit's sweep thread handles a fake lever event and
    // wipes the request of a command that arrives meanwhile, which then hangs (fixed in
    // v1.4-266, firmware commit 994a567). Give the sweep thread time to handle it first.
    static const QByteArrayList racy{"output", "modulation", "levelchange", "ext_gain"};
    if (m_info.model && m_info.directCommandRace() && racy.contains(c.line.split(' ').value(0)))
        m_settle.start(300);
    pump();
}

void DeviceLink::onTimeout()
{
    if (!m_cur)
        return;
    if (m_scanDone)  // complete sweep whose prompt never came
        return finish(true);
    if (!m_ready)
        return finish(false);  // handshake step: its callback decides
    // No reply: drop the command, drain whatever is still arriving, then carry on.
    emit message(QString("No reply to '%1' - resynchronising").arg(QString::fromLatin1(m_cur->line)));
    Cmd c = std::move(*m_cur);
    m_cur.reset();
    resync();
    if (c.done)
        c.done(false, m_lines);
}

void DeviceLink::resync()
{
    m_rx.reset();
    m_draining = true;
    m_drainClock.start();
    m_drain.start(150);
}
