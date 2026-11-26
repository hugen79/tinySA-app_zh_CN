#include "simdevice.h"
#include "units.h"
#include <QHash>
#include <QMap>
#include <cmath>
#include <cstring>

static const char *kHelpUltra =
    "commands: version reset freq time dac sweep_voltage nf saveconfig clearconfig zero sweep "
    "pause restart resume wait waitscan repeat status caloutput save recall trace trigger marker "
    "line usart_cfg vbat_offset color\r\n"
    "Other commands: data dump frequencies gamma scan hop scanraw abort test touchcal touchtest "
    "channel usart capture refresh touch release vbat help info threads rbw attenuate lna mode "
    "deviceid threshold\r\n";
static const char *kHelpBasic =
    "commands: version reset freq time dac saveconfig clearconfig data frequencies scan scanraw "
    "zero sweep pause resume caloutput save recall trace trigger marker capture refresh touch "
    "release vbat vbat_offset help info color rbw attenuate mode deviceid threshold\r\n";

SimTransport::SimTransport(Kind kind, QObject *parent)
    : QIODevice(parent), rleCapture(kind == Kind::Ultra), m_kind(kind)
{
    m_timer.setInterval(2);
    connect(&m_timer, &QTimer::timeout, this, &SimTransport::scanTick);
    if (kind == Kind::Ultra)
        m_sweepStop = 800000000;
    m_lcd = kind == Kind::Ultra ? QSize(480, 320) : QSize(320, 240);
    for (int y = 0; y < m_lcd.height(); ++y)  // dark screen, grid, a yellow sine "trace"
        for (int x = 0; x < m_lcd.width(); ++x) {
            quint16 c = x % 40 == 0 || y % 40 == 0 ? 0x4208 : 0x0000;
            if (std::abs(y - m_lcd.height() / 2 - int(m_lcd.height() / 4 * std::sin(x * 0.05))) < 2)
                c = 0xFFE0;
            m_screen += char(c >> 8);
            m_screen += char(c & 0xff);
        }
    m_pushTimer.setInterval(100);
    connect(&m_pushTimer, &QTimer::timeout, this, &SimTransport::pushTick);
    bench().append(this);
}

SimTransport::~SimTransport()
{
    bench().removeAll(this);
}

// every simulated unit; one thread (the links' thread, or the test's)
QList<SimTransport *> &SimTransport::bench()
{
    static QList<SimTransport *> units;
    return units;
}

double SimTransport::dutGainDb(Dut d, double hz)
{
    if (d != Dut::LowPass)
        return 0;
    return -10 * std::log10(1 + std::pow(hz / 150e6, 10));  // |H|^2 = 1 / (1 + (f/fc)^2n), n = 5
}

// DiSlord's packbits() (NanoVNA-D common.c), byte for byte.
static int packbits(const char *source, char *dest, int size)
{
    int i = 0, rle, l, pk = 0, sz = -1;
    while (i < size) {
        l = size - i;
        if (l > 128)
            l = 128;
        const char c = source[i++];
        for (rle = 0; --l && c == source[i + rle];)
            rle++;
        if (sz >= 0 && rle < 2)
            rle = 0;
        else if (sz < 0 || rle > 0)
            sz = pk++;
        dest[pk++] = c;
        if (rle > 0) {
            i += rle;
            dest[sz] = char(-rle);
        } else if ((dest[sz] = char(pk - sz - 2)) < 127) {
            continue;
        }
        sz = -1;
    }
    return pk;
}

QByteArray SimTransport::packBits(const QByteArray &data)
{
    QByteArray out(data.size() + data.size() / 128 + 2, 0);
    out.resize(packbits(data.constData(), out.data(), int(data.size())));
    return out;
}

// NanoVNA-D capture_rle8(): header, 32-entry palette, PackBits rows of palette indices.
QByteArray SimTransport::rle8() const
{
    auto le16 = [](int v) { return QByteArray(1, char(v & 0xff)) + char((v >> 8) & 0xff); };
    const int w = m_lcd.width(), h = m_lcd.height();
    QByteArray palette(64, 0);  // MAX_PALETTE 32, as read from the LCD (big-endian RGB565)
    QHash<quint16, int> index;
    QByteArray out = QByteArray("BM") + le16(w) + le16(h) + char(8) + char(1) + le16(64);
    QList<QByteArray> rows;
    for (int y = 0; y < h; ++y) {
        QByteArray idx(w, 0);
        for (int x = 0; x < w; ++x) {
            const int at = 2 * (y * w + x);
            const quint16 c = quint16(quint8(m_screen[at]) << 8 | quint8(m_screen[at + 1]));
            if (!index.contains(c) && index.size() < 32) {
                const int i = int(index.size());
                index.insert(c, i);
                palette[2 * i] = m_screen[at];
                palette[2 * i + 1] = m_screen[at + 1];
            }
            idx[x] = char(index.value(c, 0));  // colours outside the palette -> 0, as the firmware
        }
        rows << packBits(idx);
    }
    out += palette;
    for (const QByteArray &r : rows)
        out += le16(int(r.size())) + r;
    return out;
}

void SimTransport::region(bool fill, int x, int y, int w, int h, quint16 colour)
{
    x = std::clamp(x, 0, m_lcd.width() - 1);
    y = std::clamp(y, 0, m_lcd.height() - 1);
    w = std::min(w, m_lcd.width() - x);
    h = std::min(h, m_lcd.height() - y);
    QByteArray px;
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) {
            const quint16 v = fill ? colour : quint16(colour + 0x0841 * ((r + c) % 8));
            const int at = 2 * ((y + r) * m_lcd.width() + x + c);
            m_screen[at] = char(v >> 8);
            m_screen[at + 1] = char(v & 0xff);
            px += char(v >> 8);
            px += char(v & 0xff);
        }
    if (!m_pushTimer.isActive())
        return;
    auto le16 = [](int v) { return QByteArray(1, char(v & 0xff)) + char((v >> 8) & 0xff); };
    const QByteArray head = le16(x) + le16(y) + le16(w) + le16(h);
    if (fill)
        return out("fill\r\n" + head + px.left(2) + "ch> ");
    // refresh rle, as send_region_rle8(): own palette sorted by the pixel as the MCU reads it
    // (little-endian word), raw when small, more than 256 colours or possibly larger than raw;
    // the indices as one PackBits stream, packed 480 (LCD_WIDTH) at a time
    const int n = w * h;
    QMap<quint16, int> colours;
    for (int i = 0; i < n; ++i)
        colours.insert(quint16(quint8(px[2 * i]) | quint8(px[2 * i + 1]) << 8), 0);
    if (!m_pushRle || n < 256 || colours.size() > 256 || 2 * colours.size() + n / 64 + 2 > n)
        return out("bulk\r\n" + head + px + "ch> ");
    QByteArray palette, idx(n, 0), stream;
    int k = 0;
    for (auto it = colours.begin(); it != colours.end(); ++it, ++k) {
        it.value() = k;
        palette += char(it.key() & 0xff);
        palette += char(it.key() >> 8);
    }
    for (int i = 0; i < n; ++i)
        idx[i] = char(colours.value(quint16(quint8(px[2 * i]) | quint8(px[2 * i + 1]) << 8)));
    for (int i = 0; i < n; i += 480)
        stream += packBits(idx.mid(i, 480));
    out("rle8\r\n" + head + le16(int(palette.size())) + palette + stream + "ch> ");
}

void SimTransport::scroll(int x, int y, int w, int h)
{
    const int bpl = 2 * m_lcd.width();
    char *d = m_screen.data();
    for (int r = std::min(y + h, m_lcd.height() - 1) - 1; r >= y; --r)
        std::memcpy(d + (r + 1) * bpl + 2 * x, d + r * bpl + 2 * x, size_t(2 * w));
    auto le16 = [](int v) { return QByteArray(1, char(v & 0xff)) + char((v >> 8) & 0xff); };
    if (m_pushTimer.isActive())
        out("scrl\r\n" + le16(x) + le16(y) + le16(w) + le16(h) + le16(1) + "ch> ");
}

void SimTransport::pushTick()
{
    if (m_scan)
        return;  // never inside scan data
    ++m_pushes;
    region(false, (m_pushes * 16) % (m_lcd.width() - 32), m_lcd.height() - 24, 32, 16, 0x001F);
    if (m_pushRle) {  // a waterfall as refresh rle sends it: the rows move down, then a new top row
        scroll(30, 200, 450, 80);
        region(false, 30, 200, 450, 1, quint16(0xF800 + 0x20 * (m_pushes % 32)));
    }
}

QVector<SimTransport::Carrier> SimTransport::carriers()
{
    return {{100e6, -30}, {250e6, -50}, {433.92e6, -40}, {868e6, -45}, {2.4e9, -35}, {5.8e9, -55}};
}

double SimTransport::levelAt(double hz, double rbwHz) const
{
    const double floorDbm = (m_kind == Kind::Ultra ? -105 : -100) + std::max(m_atten, 0);
    double mw = std::pow(10.0, floorDbm / 10);
    const double half = std::max(rbwHz, 100.0) / 2;
    QVector<Carrier> in;
    if (dut == Dut::None) {
        in = carriers();
    } else {  // cabled: what the other units generate, through the DUT
        for (const SimTransport *u : bench())
            if (u != this && u->m_outMode && u->m_rfOn) {
                if (dut == Dut::Mixer) {  // the difference and the sum frequency
                    in.append(Carrier{std::abs(u->outputHz() - mixerLoHz), u->m_level - kMixerLossDb});
                    in.append(Carrier{u->outputHz() + mixerLoHz, u->m_level - kMixerLossDb});
                } else
                    in.append(Carrier{u->outputHz(), u->m_level + dutGainDb(dut, u->outputHz())});
            }
    }
    for (const Carrier &c : in) {
        if (m_kind == Kind::Basic && c.hz > 960e6)
            continue;
        const double x = (hz - c.hz) / half;
        if (std::abs(x) < 40)  // -3 dB at +-rbw/2, Gaussian filter shape
            mw += std::pow(10.0, (c.dBm - 3.01 * x * x) / 10);
    }
    return 10 * std::log10(mw);
}

void SimTransport::touchScreen()
{
    if (m_scan) {
        m_scan.reset();
        m_timer.stop();
        out("}");  // aborted scanraw: no prompt
    }
    if (m_uiOff) {  // ui_process(): the touch only gives the screen back, and says so
        m_uiOff = false;
        out("ui on\r\n");
    }
    processInput();
}

qint64 SimTransport::readData(char *data, qint64 maxlen)
{
    const qint64 n = std::min<qint64>(maxlen, m_out.size());
    std::memcpy(data, m_out.constData(), size_t(n));
    m_out.remove(0, n);
    return n;
}

qint64 SimTransport::writeData(const char *data, qint64 len)
{
    m_in.append(data, len);
    processInput();
    return len;
}

void SimTransport::out(const QByteArray &bytes)
{
    m_out += bytes;
    if (!m_readyPending) {
        m_readyPending = true;
        QMetaObject::invokeMethod(this, [this] {
            m_readyPending = false;
            emit readyRead();
        }, Qt::QueuedConnection);
    }
}

void SimTransport::processInput()
{
    // the shell thread reads (and echoes) nothing while a scan runs, nor once it hangs
    while (!m_scan && !m_hung && !m_in.isEmpty()) {
        const char c = m_in.at(0);
        m_in.remove(0, 1);
        if (c == '\r') {
            out("\r\n");
            const QByteArray line = m_line;
            m_line.clear();
            execute(line);
        } else if (c >= ' ') {
            m_line += c;
            out(QByteArray(1, c));
            if (m_pushTimer.isActive() && m_line.size() == 2)
                pushTick();  // a screen push inside the echo, as the firmware's threads allow
        }
    }
}

void SimTransport::execute(const QByteArray &line)
{
    m_received << line;
    const QByteArrayList tok = line.split(' ');
    const QByteArray cmd = tok.value(0), a1 = tok.value(1), a2 = tok.value(2);
    const bool ultra = m_kind == Kind::Ultra;

    // old firmware (v1.4-220): a wait-mutex command right after a direct one that set `dirty`
    // in output mode loses its request, and the shell waits for it forever (firmware 994a567)
    static const QByteArrayList mutexCommands{"level", "sweeptime", "sweep", "mode", "vbat", "zero",
                                              "pause", "resume", "attenuate", "rbw", "lna"};
    if (oldFirmware && m_outMode && mutexCommands.contains(cmd) && m_directClock.isValid()
        && m_directClock.elapsed() < 100) {
        m_hung = true;
        return;  // no reply, no prompt, no more input read
    }
    if (oldFirmware && m_outMode && QByteArrayList{"output", "modulation", "levelchange", "ext_gain"}.contains(cmd))
        m_directClock.start();
    const QByteArray version = !ultra        ? "tinySA_v1.4-302-sim"
                               : oldFirmware ? "tinySA4_v1.4-220-sim"
                               : ctcssCommand ? "tinySA4_v1.4-341-sim"
                                              : "tinySA4_v1.4-317-sim";
    const bool uiCommand = ultra && !oldFirmware;  // `ui off|on` (v1.4-314), `ui off "text"` (v1.4-317)
    if (cmd == "version") {
        out(version + "\r\n" + (!ultra ? "" : oldFirmware ? "HW Version:V0.4.5.1.1\r\n" : "HW Version:V0.5.4 max2871\r\n"));
    } else if (cmd == "info") {
        out(!ultra ? "tinySA v0.3ZS304\r\n" : oldFirmware ? "tinySA ULTRA ZS405\r\n" : "tinySA ULTRA+ ZS407\r\n");
        out("2019-2024 Copyright @Erik Kaashoek\r\n2016-2020 Copyright @edy555\r\n"
            "SW licensed under GPL. See: https://github.com/erikkaashoek/tinySA\r\n");
        out("Version: " + version + "\r\n");
    } else if (cmd == "help") {
        QByteArray help = ultra ? kHelpUltra : kHelpBasic;
        if (uiCommand)
            help.replace("threshold\r\n", "threshold ui\r\n");
        out(help);
    } else if (cmd == "ui" && uiCommand) {
        const int quote = line.indexOf('"');  // ui off "text": up to the next quote (parse_line)
        if (a1.isEmpty()) {
            out(m_uiOff ? "ui off\r\n" : "ui on\r\n");
        } else if (a1 == "off" && quote > 0 && uiWithText) {
            m_uiOff = true;
            m_uiText = line.mid(quote + 1);
            if (const int end = int(m_uiText.indexOf('"')); end >= 0)
                m_uiText.truncate(end);
        } else if ((a1 == "off" || a1 == "on") && tok.size() == 2) {
            m_uiOff = a1 == "off";  // a plain ui off keeps the text
        } else {
            out(uiWithText ? "usage: ui [off [\"text\"]|on]\r\n" : "usage: ui off|on\r\n");
        }
    } else if (cmd == "zero") {
        out(ultra ? "usage: zero {level}\r\n174dBm\r\n" : "usage: zero {level}\r\n128dBm\r\n");
    } else if (cmd == "sweep") {
        const qint64 f = a2.toLongLong(), half = (m_sweepStop - m_sweepStart) / 2;
        if (a1 == "start")
            m_sweepStart = f;
        else if (a1 == "stop")
            m_sweepStop = f;
        else if (a1 == "cw")
            m_sweepStart = m_sweepStop = f;
        else if (a1 == "center")
            m_sweepStart = f - half, m_sweepStop = f + half;
        else if (a1 == "span")
            m_sweepStart = m_sweepStart + half - f / 2, m_sweepStop = m_sweepStart + f;
        else if (a1 == "go" || a1 == "abort")
            m_genSweep = a1 == "go";
        else if (a1.isEmpty())
            out(QByteArray::number(m_sweepStart) + " " + QByteArray::number(m_sweepStop) + " "
                + QByteArray::number(m_sweepPoints) + "\r\n");
    } else if (cmd == "vbat") {
        out("4101 mV\r\n");
    } else if (cmd == "vbat_offset") {
        out("300\r\n");
    } else if (cmd == "deviceid") {
        out("deviceid 0\r\n");
    } else if (cmd == "rbw") {
        m_rbwKHz = a1 == "auto" ? 0 : a1.toDouble();
    } else if (cmd == "attenuate") {
        if (a1.isEmpty())  // output mode: the level from the attenuator setting
            out("usage: attenuate 0..31|auto\r\n"
                + QByteArray::number(m_outMode && !m_high ? m_level : std::max(m_atten, 0), 'f', 2) + "\r\n");
        else
            m_atten = a1 == "auto" ? -1 : a1.toInt();
    } else if (cmd == "mode") {
        const bool one = ultra && tok.size() == 2;  // the Ultra's "mode input|output"
        const QByteArray lh = one ? QByteArray("low") : a1, io = one ? a1 : a2;
        if ((lh != "low" && (ultra || lh != "high")) || (io != "input" && io != "output"))
            out(ultra ? "usage: mode [low] input|output\r\n" : "usage: mode low|high input|output\r\n");
        else
            setMode(io == "output", lh == "high");
    } else if (cmd == "lna") {
        m_lna = a1 == "on";
    } else if (cmd == "pause" || cmd == "resume") {
        m_paused = cmd == "pause";
    } else if (cmd == "capture") {
        out(!a1.isEmpty() && rleCapture ? rle8() : m_screen);
    } else if (cmd == "refresh") {
        if (a1 == "rle" && !ultra) {  // firmware without __REFRESH_RLE__ (every tinySA basic)
            out("usage: refresh off|on\r\n");
        } else {
            m_pushRle = a1 == "rle";
            if (a1 == "on" || m_pushRle)
                m_pushTimer.start();
            else
                m_pushTimer.stop();
        }
    } else if (cmd == "touch") {
        region(true, a1.toInt() - 4, a2.toInt() - 4, 8, 8, 0xFFFF);  // a white dot where touched
    } else if (cmd == "scanraw") {
        const qint64 s = a1.toLongLong(), e = a2.toLongLong();
        const int n = tok.size() > 3 ? tok[3].toInt() : m_sweepPoints;
        if (m_outMode)
            ++scansWhileGenerating;
        if (tok.size() < 3 || e < s || n <= 0) {
            out("frequency range is invalid\r\n");
        } else {
            m_scan = Scan{s, e, n, 0};
            m_clock.start();
            m_timer.start();
            return;  // '}' + prompt when the scan completes
        }
    } else if (!(cmd.isEmpty() || cmd == "abort" || cmd == "release" || cmd == "reset"
                 || cmd == "saveconfig" || cmd == "marker" || cmd == "threshold"
                 || generatorCommand(tok))) {
        out(cmd + "?\r\n");
    }
    out("ch> ");
}

void SimTransport::setMode(bool output, bool high)
{
    if (output == m_outMode && high == m_high)
        return;  // set_mode(): nothing happens in the same mode
    const bool ultra = m_kind == Kind::Ultra;
    m_outMode = output;
    m_high = high;
    m_rfOn = m_genSweep = false;
    m_mixer = true;
    m_modulation = "off";
    m_modHz = 1000, m_amDepth = 80, m_fmDev = 3000;
    m_burstPeriod = 1e-3, m_burstOn = 100e-6, m_burstCount = 0;
    m_levelChange = 0;  // not ext_gain: reset_settings() keeps it (one value for both modes)
    m_sweepTime = 2;
    m_rbwKHz = 0;
    m_atten = -1;
    if (output) {  // carrier at 10 MHz (HIGH: 300 MHz), level at its maximum
        m_sweepStart = m_sweepStop = high ? 300000000 : 10000000;
        m_level = high ? 9 : ultra ? -18.5 : -7;
    } else {
        m_sweepStart = high ? 240000000 : 0;
        m_sweepStop = high ? 960000000 : ultra ? 800000000 : 350000000;
    }
}

void SimTransport::startGenerating()
{
    setMode(true, false);
    m_sweepStart = m_sweepStop = 100000000;
    m_level = -30;
    m_rfOn = true;
    m_modulation = m_kind == Kind::Ultra ? "fm" : "nfm";
}

// The firmware's generator commands (sa_cmd.c); true when `tok` is one of them.
bool SimTransport::generatorCommand(const QByteArrayList &tok)
{
    const QByteArray cmd = tok.value(0), a1 = tok.value(1), a2 = tok.value(2);
    const bool ultra = m_kind == Kind::Ultra;
    if (cmd == "output") {
        if (a1 == "on" || a1 == "off")
            m_rfOn = a1 == "on";
        else if (ultra && (a1 == "normal" || a1 == "mixer"))
            m_mixer = a1 == "mixer";
        else
            out(ultra ? "usage: output on|off|normal|mixer\r\n" : "usage: output on|off\r\n");
    } else if (cmd == "level") {
        if (a1.isEmpty() && m_outMode)  // only in output mode, and the basic's ranges
            out(m_high ? "usage: level -38..13\r\n" : "usage: level -76..-6\r\n");
        else if (!a1.isEmpty())
            m_level = std::clamp(a1.toDouble(), m_high ? -38.0 : ultra ? -115.0 : -76.0,
                                 m_high ? 9.0 : ultra ? -18.5 : -7.0);
    } else if (cmd == "levelchange") {
        if (a1.isEmpty())
            out(ultra ? "usage: levelchange -90..+90\r\n" : "usage: levelchange -70..+70\r\n");
        else
            m_levelChange = a1.toDouble();
    } else if (cmd == "sweeptime") {
        if (a1.isEmpty())
            out("usage: sweeptime 0.003..60\r\n" + QByteArray::number(m_sweepTime, 'f', 3) + "s\r\n");
        else
            m_sweepTime = a1.toDouble();
    } else if (cmd == "ext_gain") {
        if (a1.isEmpty())
            out("usage: ext_gain -100.0..+100.0\r\n" + QByteArray::number(m_extGain, 'f', 1) + "\r\n");
        else
            m_extGain = a1.toDouble();
    } else if (cmd == "modulation") {
        const bool burst = ultra && !oldFirmware;  // v1.4-305
        const bool ctcss = burst && ctcssCommand;  // v1.4-341
        const QByteArrayList types = burst ? QByteArrayList{"off", "am", "fm", "burst"}
                                     : ultra ? QByteArrayList{"off", "am", "fm"}
                                             : QByteArrayList{"off", "am", "nfm", "wfm", "extern"};
        const std::optional<double> s = parseSeconds(QString::fromLatin1(a2));  // m/u suffixes
        if (types.contains(a1))
            m_modulation = a1;
        else if (a1 == "freq" && !a2.isEmpty())
            m_modHz = std::clamp(a2.toInt(), 1, 10000);
        else if (ultra && a1 == "depth" && !a2.isEmpty())
            m_amDepth = a2.toInt();
        else if (ultra && a1 == "deviation" && !a2.isEmpty())
            m_fmDev = a2.toInt();
        else if (burst && a1 == "period" && s)  // as set_burst_period / _on_time / _count
            m_burstPeriod = std::clamp(*s, 2e-6, 89.0), m_burstOn = std::clamp(m_burstOn, 1e-6, m_burstPeriod - 1e-6);
        else if (burst && a1 == "ontime" && s)
            m_burstOn = std::clamp(*s, 1e-6, m_burstPeriod - 1e-6);
        else if (burst && a1 == "count" && !a2.isEmpty())
            m_burstCount = std::clamp(a2.toInt(), 0, 65535);
        else if (burst && a1 == "trigger")
            ++m_burstTriggers;
        else if (ctcss && a1 == "ctcss" && a2.isEmpty())  // the state, as config.wfm_1khz_harmonic
            out(ctcssTone ? "on\r\n" : "off\r\n");
        else if (ctcss && a1 == "ctcss" && (a2 == "on" || a2 == "off"))
            ctcssTone = a2 == "on";
        else if (burst)
            out("usage: modulation off|am|fm|freq|depth|deviation|burst|period|ontime|count|trigger"
                + QByteArray(ctcss ? "|ctcss" : "") + " [value]\r\n");
        else
            out("usage: modulation " + types.join('|') + "|freq 100..6000\r\n");
    } else {
        return false;
    }
    return true;
}

void SimTransport::scanTick()
{
    if (m_clock.elapsed() >= shellWaitMs && m_in.startsWith("abort\r")) {
        // shell thread: echo + prompt of the direct command, between data chunks;
        // sweep thread: sees the abort, ends the data with '}' and no prompt
        m_in.remove(0, 6);
        m_received << "abort";
        out("abort\r\nch> }");
        m_scan.reset();
        m_timer.stop();
        processInput();
        return;
    }
    Scan &s = *m_scan;
    const int due = std::min<int>(s.points, int(m_clock.elapsed() * pointsPerSecond / 1000) + 1);
    const double step = double(s.stop - s.start) / s.points;  // as firmware: /points
    // one point at one frequency (`scanraw f f 1`): measured as a sweep point with a 300 kHz
    // step when the RBW is auto (v1.4-316)
    const double autoStep = step == 0 && s.points == 1 ? 300e3 : step;
    const double rbw = m_rbwKHz > 0 ? m_rbwKHz * 1e3 : std::max(autoStep * 1.5, 1e3);
    const int zero = m_kind == Kind::Ultra ? 174 : 128;
    while (s.next + 20 <= due || (s.next < due && due == s.points)) {
        QByteArray chunk = s.next == 0 ? "{" : "";
        for (int k = 0; k < 20 && s.next < due; ++k, ++s.next) {
            const double hz = s.start + step * s.next;
            // deterministic +-1.5 dB noise at the noise floor, different every sweep; a signal
            // well above it is steady (0.05 dB 40 dB up)
            const unsigned h = (unsigned(s.next) * 2654435761u) ^ (m_sweeps * 40503u);
            const double level = levelAt(hz, rbw), floor = (m_kind == Kind::Ultra ? -105 : -100) + std::max(m_atten, 0);
            const double noise = (((h >> 8) % 3000) / 1000.0 - 1.5) * std::clamp(1 - (level - floor) / 40, 0.03, 1.0);
            // reported at the device under test: minus the external gain (correct_RSSI)
            const int v = int(std::lround((level + noise - m_extGain + zero) * 32));
            chunk += 'x';
            chunk += char(v & 0xff);
            chunk += char((v >> 8) & 0xff);
        }
        out(chunk);
    }
    if (s.next >= s.points) {
        m_scan.reset();
        m_timer.stop();
        ++m_sweeps;
        out("}ch> ");
        processInput();
    }
}
