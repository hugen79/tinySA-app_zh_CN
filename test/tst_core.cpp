// Unit tests for src/core. One executable, one test class per module.
#include "appsettings.h"
#include "csvio.h"
#include "dfufile.h"
#include "dfuse.h"
#include "link.h"
#include "markers.h"
#include "model.h"
#include "protocol.h"
#include "session.h"
#include "simdevice.h"
#include "sweepconfig.h"
#include "traces.h"
#include "units.h"
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QPointer>
#include <QRandomGenerator>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>

class TstUnits : public QObject {
    Q_OBJECT
private slots:
    void rangeSettersKeepTheirAnchor()
    {
        FreqRange r(100e6, 200e6);
        r.setLimits(0, 1e9);
        r.setStart(120e6);
        QCOMPARE(r.stop(), 200e6);
        r.setCenter(500e6);
        QCOMPARE(r.span(), 80e6);
        QCOMPARE(r.center(), 500e6);
        r.setSpan(20e6);
        QCOMPARE(r.center(), 500e6);
        QCOMPARE(r.span(), 20e6);
    }
    void rangeClampsToLimitsAndMinSpan()
    {
        FreqRange r(100e6, 200e6);
        r.setLimits(0, 350e6, 10e3);
        r.setStop(900e6);
        QCOMPARE(r.stop(), 350e6);
        r.setStart(350e6);  // would collapse the span
        QCOMPARE(r.span(), 10e3);
        QCOMPARE(r.stop(), 350e6);
        r.set(-5, 5);
        QCOMPARE(r.start(), 0.0);
        QCOMPARE(r.span(), 10e3);
        r.setCenter(349.999e6);  // shifted back inside, span kept
        QCOMPARE(r.stop(), 350e6);
        QCOMPARE(r.span(), 10e3);
    }
    void zoomKeepsAnchorAndPanKeepsSpan()
    {
        FreqRange r(100e6, 200e6);
        r.setLimits(0, 1e9);
        r.zoom(0.5, 125e6);
        QCOMPARE(r.start(), 112.5e6);
        QCOMPARE(r.stop(), 162.5e6);
        r.pan(-500e6);
        QCOMPARE(r.start(), 0.0);
        QCOMPARE(r.span(), 50e6);
        r.zoom(1e6, 0);
        QCOMPARE(r.span(), 1e9);
    }
    void scanGridMatchesFirmwareStep()
    {
        ScanGrid g{100e6, 200e6, 101};
        QCOMPARE(g.step(), 1e6);
        // firmware step = (stop'-start)/points must equal our step
        QCOMPARE((g.scanrawStop() - 100e6) / 101, 1e6);
        QCOMPARE(g.nearest(150.4e6), 50);
        QCOMPARE(g.nearest(1e12), 100);
        QCOMPARE(g.pieceCount(), 1);
        QCOMPARE(g.piece(0), g);
    }
    void logGridInLinearPieces()
    {
        // 10 kHz - 1 GHz: 17 octaves, 101 points -> 17 pieces, starting at exact log points
        QCOMPARE(logPieces(10e3, 1e9, 101), 17);
        QCOMPARE(logPieces(10e3, 1e9, 11), 5);   // at least 2 points each
        QCOMPARE(logPieces(100e6, 150e6, 101), 1);
        const ScanGrid exact{10e3, 1e9, 101, true, 0};
        QVERIFY(std::abs(exact.freq(20) - 100e3) < 1e-3);  // a decade every 20 points
        QVERIFY(std::abs(exact.freq(60) - 10e6) < 1e-2);
        QCOMPARE(exact.freq(100), 1e9);
        QVERIFY(std::abs(exact.index(1e6) - 40) < 1e-9);
        QCOMPARE(exact.nearest(1.05e6), 40);
        QCOMPARE(exact.nearest(1.11e6), 41);  // 1.122 MHz is closer than 1 MHz
        QVERIFY(exact.step() < exact.stepAt(99));

        const ScanGrid g{10e3, 1e9, 101, true, logPieces(10e3, 1e9, 101)};
        double prev = 0;
        for (int i = 0; i < g.points; ++i) {  // rising, and freqAt / index are inverses
            QVERIFY(g.freq(i) > prev);
            prev = g.freq(i);
            QVERIFY(std::abs(g.index(g.freq(i)) - i) < 1e-6);
            QCOMPARE(g.nearest(g.freq(i) * 1.0001), i);
        }
        QVERIFY(std::abs(g.index(g.freqAt(37.3)) - 37.3) < 1e-6);
        int covered = 0;
        for (int k = 0; k < g.pieceCount(); ++k) {
            const ScanGrid p = g.piece(k);  // one scanraw: its points are the grid's
            QCOMPARE(g.pieceStart(k), covered);
            QVERIFY(!p.log && p.points >= 2);
            QVERIFY(std::abs(g.freq(covered) - exact.freq(covered)) < 1e-3);  // starts on the log grid
            for (int i = 0; i < p.points; ++i)
                QVERIFY(std::abs(p.freq(i) - g.freq(covered + i)) < 1e-3);
            if (k + 1 < g.pieceCount())  // and scanraw's stop is where the next piece starts
                QVERIFY(std::abs(p.scanrawStop() - g.freq(g.pieceStart(k + 1))) <= 1);
            covered += p.points;
        }
        QCOMPARE(covered, 101);
        QCOMPARE(g.freq(100), 1e9);
        // a piece is linear: within one octave the step stays, the next octave's is about twice
        QVERIFY(std::abs(g.stepAt(g.pieceStart(5)) / g.stepAt(g.pieceStart(4)) - 2) < 0.5);
    }
    void logZoomAndCentre()
    {
        FreqRange r(1e6, 100e6);
        r.setLimits(10e3, 6e9);
        r.zoomLog(0.5, 10e6);  // the ratio 100 becomes 10, about 10 MHz
        QVERIFY(std::abs(r.start() - std::sqrt(10.0) * 1e6) < 1);
        QVERIFY(std::abs(r.stop() - std::sqrt(10.0) * 10e6) < 1);
        r.setCenterLog(100e6);  // 100 MHz in the middle: x / sqrt(10) .. x sqrt(10)
        QVERIFY(std::abs(r.start() - 100e6 / std::sqrt(10.0)) < 1);
        QVERIFY(std::abs(r.stop() - 100e6 * std::sqrt(10.0)) < 1);
        r.zoomLog(10, 100e6);  // as wide as the limits allow
        QCOMPARE(r.start(), 10e3);
        QCOMPARE(r.stop(), 6e9);
    }
    void niceSteps()
    {
        QCOMPARE(niceStep(0.9), 1.0);
        QCOMPARE(niceStep(1.1), 2.0);
        QCOMPARE(niceStep(2.2), 2.5);
        QCOMPARE(niceStep(3), 5.0);
        QCOMPARE(niceStep(6e6), 10e6);
        QCOMPARE(niceStep(2e6), 2e6);
    }
    void formatAndParse()
    {
        QCOMPARE(formatHz(433.92e6, 1e3), QString("433.920 MHz"));
        QCOMPARE(formatHz(2.4e9, 1e6), QString("2.400 GHz"));
        QCOMPARE(formatHz(500, 1), QString("500 Hz"));
        QCOMPARE(parseHz("433.92").value(), 433.92e6);
        QCOMPARE(parseHz("433,92").value(), 433.92e6);
        QCOMPARE(parseHz("100k").value(), 100e3);
        QCOMPARE(parseHz("1.2 GHz").value(), 1.2e9);
        QCOMPARE(parseHz("50 Hz").value(), 50.0);
        QVERIFY(!parseHz("abc"));
        QVERIFY(!parseHz(""));
    }
};

// ---------------------------------------------------------------------------

static QByteArray point(int raw)
{
    return QByteArray("x") + char(raw & 0xff) + char((raw >> 8) & 0xff);
}

// Plays a conversation: for each command, expect() is armed (as DeviceLink does
// when it transmits) and the bytes the unit sends back are fed in random chunks.
struct Step {
    QByteArray cmd;
    RxParser::Body body;
    int arg;
    QByteArray reply;
};
static QVector<RxEvent> play(const QVector<Step> &steps, int maxChunk)
{
    RxParser p;
    QVector<RxEvent> all;
    for (const Step &st : steps) {
        p.expect(st.cmd, st.body, st.arg);
        for (int at = 0; at < st.reply.size();) {
            const int n = maxChunk <= 0 ? int(st.reply.size())
                                        : 1 + QRandomGenerator::global()->bounded(maxChunk);
            for (const RxEvent &e : p.feed(st.reply.mid(at, n))) {
                if (e.type == RxEvent::ScanData && !all.isEmpty()
                    && all.last().type == RxEvent::ScanData)
                    all.last().values += e.values;  // batching depends on chunking
                else
                    all.append(e);
            }
            at += n;
        }
    }
    return all;
}

static QString describe(const QVector<RxEvent> &ev)
{
    QStringList s;
    for (const RxEvent &e : ev) {
        switch (e.type) {
        case RxEvent::Line: s << "L:" + QString::fromLatin1(e.data); break;
        case RxEvent::Echo: s << "E:" + QString::fromLatin1(e.data); break;
        case RxEvent::Prompt: s << "P"; break;
        case RxEvent::ScanData: {
            QStringList v;
            for (qint16 x : e.values) v << QString::number(x);
            s << "D:" + v.join(',');
            break;
        }
        case RxEvent::ScanEnd: s << QString("}%1").arg(e.count); break;
        case RxEvent::Pixels: s << QString("PX:%1").arg(e.data.size()); break;
        case RxEvent::Region:
            s << QString("R:%1,%2,%3,%4,%5,%6%7").arg(e.rect.x()).arg(e.rect.y())
                     .arg(e.rect.width()).arg(e.rect.height()).arg(e.fill).arg(e.data.size())
                     .arg(e.rle8 ? ",rle8" : e.scroll ? ",scrl" : "");
            break;
        }
    }
    return s.join(' ');
}

class TstProtocol : public QObject {
    Q_OBJECT
private slots:
    void textReplyIgnoresStaleOutput()
    {
        const auto ev = play({{"version", RxParser::Body::Text, 0,
                               "abort\r\nch> version\r\ntinySA4_v1.4\r\nHW Version:V0.5.4\r\nch> "}},
                             0);
        QCOMPARE(describe(ev), QString("L:abort P E:version L:tinySA4_v1.4 L:HW Version:V0.5.4 P"));
    }
    void scanrawCaptureAndPushesAreChunkingInvariant()
    {
        const QVector<Step> steps{
            {"scanraw 0 1000 4", RxParser::Body::ScanRaw, 4,
             "scanraw 0 1000 4\r\n{" + point(100) + point(-32) + point(0x1234) + point(7) + "}ch> "},
            {"capture", RxParser::Body::Capture, 24, "capture\r\n" + QByteArray(24, 'U') + "ch> "},
            {"vbat", RxParser::Body::Text, 0,
             "bulk\r\n" + QByteArray("\x02\x00\x03\x00\x02\x00\x01\x00", 8) + "ABCD" + "ch> "
                 + "fill\r\n" + QByteArray("\x00\x00\x00\x00\x04\x00\x04\x00", 8)
                 + QByteArray("\xf8\x00", 2) + "ch> vbat\r\n4101 mV\r\nch> "}};
        const QString want = "E:scanraw 0 1000 4 D:100,-32,4660,7 }4 P E:capture PX:24 P "
                             "R:2,3,2,1,0,4 R:0,0,4,4,1,2 E:vbat L:4101 mV P";
        QCOMPARE(describe(play(steps, 0)), want);
        for (int i = 0; i < 200; ++i)
            QCOMPARE(describe(play(steps, 1 + i % 64)), want);
    }
    void abortedScanHasNoPrompt()
    {
        const auto ev = play({{"scanraw 0 1000 10", RxParser::Body::ScanRaw, 10,
                               "scanraw 0 1000 10\r\n{" + point(1) + point(2) + "}"},
                              {"resume", RxParser::Body::Text, 0, "resume\r\nch> "}},
                             3);
        QCOMPARE(describe(ev), QString("E:scanraw 0 1000 10 D:1,2 }2 E:resume P"));
    }
    void outOfBandTextInsideScan()
    {
        const auto ev = play({{"scanraw 0 1 2", RxParser::Body::ScanRaw, 2,
                               "scanraw 0 1 2\r\n{" + point(5) + "Command timeout\r\nch> "
                                   + point(6) + "}ch> "}},
                             0);
        QCOMPARE(describe(ev), QString("E:scanraw 0 1 2 D:5 L:Command timeout D:6 }2 P"));
    }
    void refusedScanFallsBackToText()
    {
        const auto ev = play({{"scanraw 5 1 2", RxParser::Body::ScanRaw, 2,
                               "scanraw 5 1 2\r\nfrequency range is invalid\r\nch> "}},
                             0);
        QCOMPARE(describe(ev), QString("E:scanraw 5 1 2 L:frequency range is invalid P"));
    }
    void packBitsRoundTrip()
    {
        QList<QByteArray> cases{QByteArray(480, 0), QByteArray(1, 7), QByteArray("ab"),
                                QByteArray(128, 5), QByteArray(129, 5), QByteArray(257, 9)};
        QByteArray mixed;
        for (int i = 0; i < 480; ++i)  // runs of 1..9 of changing values
            mixed += char((i / (1 + i % 9)) & 0x1f);
        cases << mixed;
        QByteArray noise;
        for (int i = 0; i < 480; ++i)
            noise += char(QRandomGenerator::global()->bounded(256));
        cases << noise;
        for (const QByteArray &c : cases) {
            const QByteArray packed = SimTransport::packBits(c);
            QCOMPARE(unpackBits(packed.constData(), int(packed.size()), int(c.size())), c);
        }
        QVERIFY(SimTransport::packBits(QByteArray(480, 0)).size() <= 8);  // 4 runs of 128/96
    }
    void captureRle8OrRawIsDetected()
    {
        // 2x2 image, palette {black, red}: rows "0 1" / "1 1"
        auto le16 = [](int v) { return QByteArray(1, char(v & 0xff)) + char((v >> 8) & 0xff); };
        QByteArray rle = QByteArray("BM") + le16(2) + le16(2) + char(8) + char(1) + le16(4)
                         + QByteArray("\x00\x00\xf8\x00", 4);
        rle += le16(3) + QByteArray("\x01\x00\x01", 3);  // literal 2: 0,1
        rle += le16(2) + QByteArray("\xff\x01", 2);      // repeat 2: 1
        for (int chunk : {0, 1, 3, 7}) {
            const auto ev = play({{"capture rle", RxParser::Body::Capture, 8,
                                   "capture rle\r\n" + rle + "ch> "}}, chunk);
            QCOMPARE(describe(ev), QString("E:capture rle PX:%1 P").arg(rle.size()));
            QVERIFY(ev[1].rle8);
            QImage img;
            QVERIFY(decodeRle8(ev[1].data, img));
            QCOMPARE(img.size(), QSize(2, 2));
            QCOMPARE(img.pixelColor(0, 0), QColor(Qt::black));
            QCOMPARE(img.pixelColor(1, 0).red(), 255);
            QCOMPARE(img.pixelColor(0, 1).red(), 255);
        }
        // old firmware ignores "rle" and sends 2x2 raw pixels
        const auto raw = play({{"capture rle", RxParser::Body::Capture, 8,
                                "capture rle\r\n" + QByteArray(8, '\x11') + "ch> "}}, 3);
        QCOMPARE(describe(raw), QString("E:capture rle PX:8 P"));
        QVERIFY(!raw[1].rle8);
    }
    void pushInsideAnEcho()
    {
        const QByteArray push = "bulk\r\n" + QByteArray("\x00\x00\x00\x00\x01\x00\x01\x00", 8) + "AB" + "ch> ";
        for (int chunk : {0, 1, 5}) {
            const auto ev = play({{"refresh off", RxParser::Body::Text, 0,
                                   "ref" + push + "resh off\r\nch> "}}, chunk);
            QCOMPARE(describe(ev), QString("R:0,0,1,1,0,2 E:refresh off P"));
        }
    }
    void refreshRlePushes()
    {
        // "rle8": 2x2 at 3,4, palette {black, red}, indices 0 1 1 1 as one PackBits stream
        // (literal 1: 0, then 3 x 1); "scrl": rows 0..9 of x 0..1 down by 1
        auto le16 = [](int v) { return QByteArray(1, char(v & 0xff)) + char((v >> 8) & 0xff); };
        const QByteArray rle8 = "rle8\r\n" + le16(3) + le16(4) + le16(2) + le16(2) + le16(4)
                                + QByteArray("\x00\x00\xf8\x00", 4) + QByteArray("\x00\x00\xfe\x01", 4) + "ch> ";
        const QByteArray scrl = "scrl\r\n" + le16(0) + le16(0) + le16(2) + le16(10) + le16(1) + "ch> ";
        QCOMPARE(SimTransport::packBits(QByteArray("\x00\x01\x01\x01", 4)), QByteArray("\x00\x00\xfe\x01", 4));
        for (int chunk : {0, 1, 3, 7}) {
            const auto ev = play({{"refresh rle", RxParser::Body::Text, 0,
                                   "refr" + rle8 + "esh rle\r\n" + scrl + "ch> "}}, chunk);
            QCOMPARE(describe(ev), QString("R:3,4,2,2,0,10,rle8 E:refresh rle R:0,0,2,10,0,2,scrl P"));
            QImage img(6, 12, QImage::Format_RGB16);
            img.fill(Qt::white);
            QVERIFY(blitRle8(img, ev[0].rect, ev[0].data));
            QCOMPARE(img.pixelColor(3, 4), QColor(Qt::black));
            QCOMPARE(img.pixelColor(4, 4).red(), 255);
            QCOMPARE(img.pixelColor(3, 5).red(), 255);
            QCOMPARE(img.pixelColor(2, 4), QColor(Qt::white));  // outside the region
        }
    }
    void scrollMovesRowsDown()
    {
        QImage img(3, 4, QImage::Format_RGB16);
        auto px = [&](int x, int y) -> quint16 & { return reinterpret_cast<quint16 *>(img.scanLine(y))[x]; };
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 3; ++x)
                px(x, y) = quint16(10 * y + x);
        scrollRgb16(img, QRect(0, 1, 2, 3), 1);  // rows 1..3 of x 0..1 down by 1: row 3 drops off
        QCOMPARE(px(0, 0), quint16(0));
        QCOMPARE(px(0, 1), quint16(10));  // left as it was: the caller draws the new top row
        QCOMPARE(px(0, 2), quint16(10));
        QCOMPARE(px(1, 3), quint16(21));
        QCOMPARE(px(2, 3), quint16(32));  // outside the rectangle
    }
    void rgb565IsBigEndian()
    {
        QImage img(2, 1, QImage::Format_RGB16);
        blitRgb565BE(img, img.rect(), QByteArray("\xf8\x00\x07\xe0", 4));
        QCOMPARE(img.pixelColor(0, 0).red(), 255);
        QCOMPARE(img.pixelColor(1, 0).green(), 255);
    }
};

// ---------------------------------------------------------------------------

class TstModel : public QObject {
    Q_OBJECT
private slots:
    void detectFromInfo()
    {
        QCOMPARE(detectModel("tinySA v0.3ZS304\n2019-2024 Copyright", "")->name, QString("tinySA"));
        QCOMPARE(detectModel("tinySA ULTRA ZS405\n", "")->name, QString("tinySA Ultra"));
        QCOMPARE(detectModel("tinySA ULTRA+ ZS407\n", "")->name, QString("tinySA Ultra+"));
        QCOMPARE(detectModel("", "tinySA4_v1.4-100")->name, QString("tinySA Ultra"));
        QCOMPARE(detectModel("", "tinySA_v1.4-100")->name, QString("tinySA"));
        QVERIFY(!detectModel("NanoVNA-H 4\n", "1.2.00"));
    }
    void firmwareWithTheCommandRace()
    {
        auto race = [](const char *version) {
            DeviceInfo d;
            d.version = version;
            return d.directCommandRace();
        };
        QVERIFY(race("tinySA4_v1.4-220-gbc09086"));  // ZS405 that hung in generator mode
        QVERIFY(race("tinySA4_v1.4-265-g1234567"));
        QVERIFY(!race("tinySA4_v1.4-266-g1234567"));  // fixed (changelog V1.4266)
        QVERIFY(!race("tinySA_v1.4-302-gab5ecd5-dirty"));
        QVERIFY(race("tinySA_v1.3-500-gaaaaaaa"));
        QVERIFY(race("something else"));  // cannot tell: wait, to be safe
    }
    void helpAndZero()
    {
        const QStringList cmds = parseHelp({"commands: version zero", "Other commands: scanraw vbat"});
        QCOMPARE(cmds, QStringList({"version", "zero", "scanraw", "vbat"}));
        QCOMPARE(parseZero({"usage: zero {level}", "174dBm"}, 0), 174);
        QCOMPARE(parseZero({"zero?"}, 128), 128);
    }
    void configDiffSendsOnlyChanges()
    {
        const ModelDesc &basic = *detectModel("tinySA v0.3ZS304", "");
        AppliedConfig have;
        SweepConfig c;
        c.grid = {100e6, 200e6, 450};
        QCOMPARE(configCommands(c, have, basic),
                 QByteArrayList({"mode low input", "attenuate auto", "rbw auto", "ext_gain 0",
                                 "sweep start 100000000", "sweep stop 200000000"}));
        QVERIFY(configCommands(c, have, basic).isEmpty());
        c.rbwKHz = 30;
        QCOMPARE(configCommands(c, have, basic), QByteArrayList({"rbw 30"}));
        c.extGainDb = 20;  // an amplifier in front of the input
        QCOMPARE(configCommands(c, have, basic), QByteArrayList({"ext_gain 20"}));
        c.extGainDb = -6.5;
        QCOMPARE(configCommands(c, have, basic), QByteArrayList({"ext_gain -6.5"}));
        c.input = InputMode::High;  // mode switch reloads rbw/atten/range on the unit; it keeps
        // the external gain (only the generator sets its own: the link forgets it then)
        QCOMPARE(configCommands(c, have, basic),
                 QByteArrayList({"mode high input", "attenuate auto", "rbw 30",
                                 "sweep start 100000000", "sweep stop 200000000"}));
        have.extGainDb.reset();
        QCOMPARE(configCommands(c, have, basic), QByteArrayList({"ext_gain -6.5"}));
        c.extGainDb = 150.04;  // `ext_gain -100.0..+100.0`, 0.1 dB
        QCOMPARE(clampConfig(c, basic).extGainDb, 100.0);
        c.extGainDb = -12.34;
        QCOMPARE(clampConfig(c, basic).extGainDb, -12.3);
        c.extGainDb = -6.5;
        const ModelDesc &ultra = *detectModel("tinySA ULTRA ZS405", "");
        AppliedConfig u;
        c.lna = true;
        const QByteArrayList first = configCommands(clampConfig(c, ultra), u, ultra);
        QCOMPARE(first.value(0), QByteArray("mode low input"));  // a unit left generating: never scanraw
        QVERIFY(first.contains("lna on"));
        QVERIFY(configCommands(clampConfig(c, ultra), u, ultra).isEmpty());
    }
    void clampToModel()
    {
        const ModelDesc &basic = *detectModel("tinySA v0.3ZS304", "");
        SweepConfig c;
        c.grid = {100e6, 2e9, 100000};
        c.rbwKHz = 850;
        c.lna = true;
        c = clampConfig(c, basic);
        QCOMPARE(c.grid.stopHz, 350e6);
        QCOMPARE(c.grid.points, 30000);
        QCOMPARE(c.rbwKHz, 600.0);
        QVERIFY(!c.lna);
    }
};

// ---------------------------------------------------------------------------

class TstLink : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<SweepFrame>(); qRegisterMetaType<DeviceInfo>(); }

    void usbIdsOfBothFirmwares()
    {
        QVERIFY(DeviceLink::isTinySAUsbId(0x0483, 0x5740));
        QVERIFY(DeviceLink::isTinySAUsbId(0x1781, 0x0A44));
        QVERIFY(!DeviceLink::isTinySAUsbId(0x0483, 0x5741));  // tinyGTC
        QVERIFY(!DeviceLink::isTinySAUsbId(0x0483, 0xDF11));  // DFU bootloader
        QVERIFY(!DeviceLink::isTinySAUsbId(0x1781, 0x5740));
        QVERIFY(!DeviceLink::isTinySAUsbId(0, 0));            // port without USB IDs
    }

    void handshakeDetectsSimUltra()
    {
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened);
        link.open("sim:ultra");
        QVERIFY(opened.wait(3000));
        const auto info = opened.first().first().value<DeviceInfo>();
        QCOMPARE(info.model->name, QString("tinySA Ultra+"));
        QCOMPARE(info.hardware, QString("ZS407"));
        QCOMPARE(info.zeroDbm, 174);
        QVERIFY(info.has("scanraw"));
        QCOMPARE(info.version, QString("tinySA4_v1.4-341-sim"));
        QCOMPARE(info.firmware(), 1004341);
        QVERIFY(info.has("ui"));  // screen off while scanning
        QVERIFY(!info.directCommandRace());
        QVERIFY(info.hasBurst);  // `modulation` lists burst
        QCOMPARE(info.ctcssTone, std::optional(false));  // `modulation ctcss` read: off
    }

    void singleSweepLandsOnTheGrid()
    {
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened), frames(&link, &DeviceLink::frame),
            scan(&link, &DeviceLink::scanChanged);
        link.open("sim:basic");
        QVERIFY(opened.wait(3000));
        SweepConfig c;
        c.grid = {50e6, 150e6, 101};
        link.setConfig(c);
        link.setScan(DeviceLink::Once);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty() && frames.last().first().value<SweepFrame>().complete, 5000);
        const auto f = frames.last().first().value<SweepFrame>();
        QCOMPARE(f.received, 101);
        QCOMPARE(f.grid.freq(50), 100e6);
        QVERIFY(std::abs(f.dBm[50] - -30) < 2);  // 100 MHz carrier exactly on point 50
        QVERIFY(f.dBm[10] < -90);
        QTRY_COMPARE_WITH_TIMEOUT(scan.last().first().toInt(), int(DeviceLink::Off), 2000);

        auto *sim = qobject_cast<SimTransport *>(link.transport());
        QTRY_VERIFY_WITH_TIMEOUT(sim->received().contains("resume"), 2000);
        const QByteArrayList rx = sim->received();
        const int scanAt = rx.indexOf("scanraw 50000000 151000000 101");
        QVERIFY(scanAt > 0);
        QVERIFY(rx.indexOf("pause") < scanAt);
        QVERIFY(rx.indexOf("rbw auto") < scanAt);
        QVERIFY(rx.indexOf("resume") > scanAt);
        QVERIFY(std::none_of(rx.begin(), rx.end(), [](const QByteArray &l) { return l.startsWith("ui "); }));  // no `ui`
    }

    void logSweepInPieces()
    {
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened), frames(&link, &DeviceLink::frame),
            scan(&link, &DeviceLink::scanChanged);
        link.open("sim:ultra");
        QVERIFY(opened.wait(3000));
        SweepConfig c;
        c.grid = {10e6, 1e9, 51, true};  // 7 octaves: 7 pieces, one scanraw each
        link.setConfig(c);
        link.setScan(DeviceLink::Once);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty() && frames.last().first().value<SweepFrame>().complete, 5000);
        const auto f = frames.last().first().value<SweepFrame>();
        QCOMPARE(f.received, 51);
        QVERIFY(f.grid.log);
        QCOMPARE(f.grid.pieces, 7);
        QCOMPARE(std::count_if(frames.begin(), frames.end(), [](const QList<QVariant> &a) {
                     return a.first().value<SweepFrame>().complete; }), 1);  // one sweep, not 7
        auto *sim = qobject_cast<SimTransport *>(link.transport());
        QByteArrayList scans;
        for (const QByteArray &l : sim->received())
            if (l.startsWith("scanraw"))
                scans << l;
        QCOMPARE(scans.size(), 7);
        for (int k = 0; k < 7; ++k) {
            const ScanGrid p = f.grid.piece(k);
            QCOMPARE(scans[k], QString("scanraw %1 %2 %3").arg(std::llround(p.startHz)).arg(p.scanrawStop())
                                   .arg(p.points).toLatin1());
        }
        QCOMPARE(scans.first().split(' ').value(1), QByteArray("10000000"));
        const int at = f.grid.nearest(100e6);  // the sim's 100 MHz carrier, near a point
        QVERIFY(std::abs(f.grid.freq(at) - 100e6) < f.grid.stepAt(at));
        QVERIFY(*std::max_element(f.dBm.begin() + at - 1, f.dBm.begin() + at + 2) > -45);
        QVERIFY(f.dBm[2] < -80);
        QTRY_COMPARE_WITH_TIMEOUT(scan.last().first().toInt(), int(DeviceLink::Off), 2000);
    }

    void screenOffWhileScanning()
    {
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened), frames(&link, &DeviceLink::frame);
        link.open("sim:ultra");
        QVERIFY(opened.wait(3000));
        auto *sim = qobject_cast<SimTransport *>(link.transport());
        link.setScan(DeviceLink::Continuous);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 3000);
        QVERIFY(sim->uiOff());
        QCOMPARE(sim->uiText(), QByteArray());  // ui off "": no text from an earlier session
        QByteArrayList rx = sim->received();
        const auto firstScan = std::find_if(rx.begin(), rx.end(), [](const QByteArray &l) { return l.startsWith("scanraw"); });
        QVERIFY(rx.indexOf("pause") < rx.indexOf("ui off \"\""));
        QVERIFY(rx.indexOf("ui off \"\"") < int(firstScan - rx.begin()));
        link.setScreenOffWhileScanning(false);  // switched between two sweeps
        QTRY_VERIFY_WITH_TIMEOUT(!sim->uiOff(), 3000);
        link.setScreenOffWhileScanning(true);
        QTRY_VERIFY_WITH_TIMEOUT(sim->uiOff(), 3000);
        link.setScan(DeviceLink::Off);  // the screen back, then the unit's own sweeping
        QTRY_VERIFY_WITH_TIMEOUT(!sim->received().isEmpty() && sim->received().last() == "resume", 3000);
        QVERIFY(!sim->uiOff());
        rx = sim->received();
        QVERIFY(rx.lastIndexOf("ui on") < rx.lastIndexOf("resume"));
    }

    void olderFirmwareGetsAPlainUiOff()
    {
        // v1.4-314..316: ui off|on only; ui off "text" answers with its usage, the screen stays on
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened), frames(&link, &DeviceLink::frame),
            msg(&link, &DeviceLink::message);
        link.open("sim:ultra");
        QVERIFY(opened.wait(3000));
        auto *sim = qobject_cast<SimTransport *>(link.transport());
        sim->uiWithText = false;
        link.setScan(DeviceLink::Continuous);
        QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= 3 && sim->uiOff(), 3000);
        const QByteArrayList rx = sim->received();
        QCOMPARE(rx.count("ui off \"\""), 1);  // tried once
        QVERIFY(rx.indexOf("ui off \"\"") < rx.indexOf("ui off"));
        QCOMPARE(rx.count("ui off"), 1);
        QVERIFY(msg.isEmpty());
        link.setScan(DeviceLink::Off);
        QTRY_VERIFY_WITH_TIMEOUT(!sim->uiOff(), 3000);
    }

    void touchGivesTheScreenBack()
    {
        // between two sweeps a touch only gives the screen back; its "ui on" stops our scanning
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened), scan(&link, &DeviceLink::scanChanged),
            msg(&link, &DeviceLink::message);
        link.open("sim:ultra");
        QVERIFY(opened.wait(3000));
        auto *sim = qobject_cast<SimTransport *>(link.transport());
        bool touched = false;
        connect(&link, &DeviceLink::frame, this, [&](const SweepFrame &f) {
            if (f.complete && !touched) {  // the sim finished the scan already: no scan to abort
                touched = true;
                QVERIFY(sim->uiOff());
                sim->touchScreen();
            }
        });
        link.setScan(DeviceLink::Continuous);
        QTRY_VERIFY_WITH_TIMEOUT(touched && scan.last().first().toInt() == DeviceLink::Off, 3000);
        QVERIFY(msg.size() >= 1);
        QCOMPARE(msg.last().first().toString(), QString("Scan stopped on the tinySA"));
        QVERIFY(!sim->uiOff());
        QTest::qWait(300);
        QCOMPARE(sim->received().count("ui on"), 1);  // the handshake's; the unit is on already
    }

    void closingWhileScanningGivesTheScreenBack()
    {
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened), frames(&link, &DeviceLink::frame);
        link.open("sim:ultra");
        QVERIFY(opened.wait(3000));
        QPointer<SimTransport> sim = qobject_cast<SimTransport *>(link.transport());
        sim->pointsPerSecond = 500;
        SweepConfig c;
        c.grid = {1e6, 300e6, 3000};
        link.setConfig(c);
        link.setScan(DeviceLink::Continuous);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 3000);  // a long scan in flight
        link.close();
        QVERIFY(sim);
        QCOMPARE(sim->pendingInput(), QByteArray("abort\rui on\rresume\r"));
    }

    void touchOnTheUnitStopsScanning()
    {
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened), frames(&link, &DeviceLink::frame),
            scan(&link, &DeviceLink::scanChanged), msg(&link, &DeviceLink::message);
        link.open("sim:ultra");
        QVERIFY(opened.wait(3000));
        auto *sim = qobject_cast<SimTransport *>(link.transport());
        sim->pointsPerSecond = 2000;
        SweepConfig c;
        c.grid = {1e6, 900e6, 3000};
        link.setConfig(c);
        link.setScan(DeviceLink::Continuous);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 3000);  // mid-sweep progress frame
        link.simTouch();
        QTRY_COMPARE_WITH_TIMEOUT(scan.last().first().toInt(), int(DeviceLink::Off), 2000);
        QVERIFY(!msg.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(sim->received().contains("resume"), 2000);
        // the link is still usable afterwards
        QSignalSpy reply(&link, &DeviceLink::reply);
        link.send("vbat");
        QVERIFY(reply.wait(2000));
        QCOMPARE(reply.first().at(1).toStringList(), QStringList({"4101 mV"}));
    }

    void captureDecodesScreen_data()
    {
        QTest::addColumn<QString>("port");
        QTest::addColumn<QSize>("size");
        QTest::addColumn<bool>("rle");
        QTest::newRow("ultra, RLE8") << "sim:ultra" << QSize(480, 320) << true;
        QTest::newRow("basic, raw (old firmware)") << "sim:basic" << QSize(320, 240) << false;
    }
    void captureDecodesScreen()
    {
        QFETCH(QString, port);
        QFETCH(QSize, size);
        QFETCH(bool, rle);
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened), screen(&link, &DeviceLink::screen),
            traffic(&link, &DeviceLink::traffic);
        link.open(port);
        QVERIFY(opened.wait(3000));
        auto *sim = qobject_cast<SimTransport *>(link.transport());
        link.capture();
        QVERIFY(screen.wait(3000));
        const QImage img = screen.first().first().value<QImage>();
        QCOMPARE(img.size(), size);
        QImage want(size, QImage::Format_RGB16);
        blitRgb565BE(want, want.rect(), sim->screenBytes());
        QCOMPARE(img, want);  // lossless: every sim colour is in the palette
        bool sawRle = false;
        for (const auto &t : traffic)
            sawRle |= t.at(1).toString().contains("RLE8");
        QCOMPARE(sawRle, rle);
        QVERIFY(sim->received().contains("capture rle"));
    }

    void remoteMirrorsScreen_data()
    {
        QTest::addColumn<QString>("port");
        QTest::addColumn<bool>("rle");
        QTest::newRow("ultra, refresh rle") << "sim:ultra" << true;
        QTest::newRow("basic, falls back to refresh on") << "sim:basic" << false;
    }
    void remoteMirrorsScreen()
    {
        QFETCH(QString, port);
        QFETCH(bool, rle);
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened), screen(&link, &DeviceLink::screen);
        link.open(port);
        QVERIFY(opened.wait(3000));
        auto *sim = qobject_cast<SimTransport *>(link.transport());
        link.setRemote(true);
        QTRY_VERIFY_WITH_TIMEOUT(screen.size() >= 6, 5000);  // the capture, then pushes (rle: scrolls too)
        sim->stopPushes();
        QImage want(sim->screenBytes().size() == 480 * 320 * 2 ? QSize(480, 320) : QSize(320, 240),
                    QImage::Format_RGB16);
        blitRgb565BE(want, want.rect(), sim->screenBytes());
        QTRY_COMPARE_WITH_TIMEOUT(screen.last().first().value<QImage>(), want, 2000);  // pixel exact
        const QByteArrayList rx = sim->received();
        QVERIFY(rx.contains("refresh rle"));
        QCOMPARE(rx.contains("refresh on"), !rle);
        link.setRemote(false);
        QTRY_VERIFY_WITH_TIMEOUT(sim->received().contains("refresh off"), 3000);
    }

    void stopAbortsALongScan()
    {
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened), frames(&link, &DeviceLink::frame),
            busy(&link, &DeviceLink::busyChanged), msg(&link, &DeviceLink::message);
        link.open("sim:ultra");
        QVERIFY(opened.wait(3000));
        auto *sim = qobject_cast<SimTransport *>(link.transport());
        sim->pointsPerSecond = 500;  // 3000 points take 6 s
        sim->shellWaitMs = 300;
        SweepConfig c;
        c.grid = {1e6, 900e6, 3000};
        link.setConfig(c);
        link.setScan(DeviceLink::Continuous);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 3000);
        QElapsedTimer t;
        t.start();
        link.setScan(DeviceLink::Off);
        QTRY_VERIFY_WITH_TIMEOUT(!busy.isEmpty() && !busy.last().first().toBool(), 3000);
        QVERIFY(t.elapsed() < 2000);  // not the rest of the 6 s sweep
        QVERIFY(msg.isEmpty());       // our own abort is not reported as a touch
        QVERIFY(sim->received().contains("abort"));
        QCOMPARE(sim->received().last(), QByteArray("resume"));
    }

    void quitRightAfterStopStillResumesTheUnit()
    {
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened), frames(&link, &DeviceLink::frame);
        link.open("sim:basic");
        QVERIFY(opened.wait(3000));
        QPointer<SimTransport> sim = qobject_cast<SimTransport *>(link.transport());
        sim->pointsPerSecond = 500;
        SweepConfig c;
        c.grid = {1e6, 300e6, 3000};
        link.setConfig(c);
        link.setScan(DeviceLink::Continuous);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 3000);
        link.setScan(DeviceLink::Off);  // resume queued behind the running scan...
        link.close();                   // ...and the app quits at once
        QVERIFY(sim);                   // deleted later
        QCOMPARE(sim->pendingInput(), QByteArray("abort\rresume\r"));
    }

    void notATinySA()
    {
        QCOMPARE(detectModel("NanoVNA-H", "1.0"), nullptr);
    }
};

// ---------------------------------------------------------------------------

static DeviceInfo unit(const QString &info)  // as the handshake fills it in
{
    DeviceInfo d;
    d.model = detectModel(info, "");
    d.hardware = QRegularExpression(R"(ZS\d{3})").match(info).captured(0);
    return d;
}

static GenConfig carrier(double hz = 433.92e6, double dBm = -40)
{
    GenConfig c;
    c.freqHz = hz;
    c.levelDbm = dBm;
    c.on = false;
    c.type = GenType::CW;
    return c;
}

class TstGenerator : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<SweepFrame>(); qRegisterMetaType<DeviceInfo>(); qRegisterMetaType<GenConfig>(); }

    void ultraCommandsInFirmwareOrder()
    {
        const ModelDesc &m = *unit("tinySA ULTRA+ ZS407").model;
        AppliedGen h;
        GenConfig c = carrier();
        // the mode switch mutes the output and resets modulation: only what differs follows
        QCOMPARE(genCommands(c, h, m),
                 QByteArrayList({"mode low output", "ext_gain 0", "sweep cw 433920000", "level -40"}));
        QVERIFY(genCommands(c, h, m).isEmpty());
        c.on = true;
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"output on"}));
        c.freqHz = 100e6;  // the Ultra may lower the level at a new frequency: level again
        c.freqHz = 101e6;  // edits between two diffs: one command per field
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"sweep cw 101000000", "level -40"}));
        c.type = GenType::Sweep;
        c.spanHz = 10e6;
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"sweep center 101000000", "sweep span 10000000",
                                                       "level -40", "sweeptime 2", "sweep go"}));
        c.type = GenType::FM;  // never sweep and modulate: the sweep stops first
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"sweep abort", "sweep cw 101000000", "level -40",
                                                       "modulation fm"}));
        c.fmDevHz = 5000;
        c.modHz = 400;
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"modulation freq 400", "modulation deviation 5000"}));
        c.mixer = false;
        c.extGainDb = 10;  // the level is at the DUT; the unit gets the port level
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"output normal", "ext_gain 10", "level -50"}));
        c.on = false;
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"output off"}));
    }

    void levelBeforeOutputOn()
    {
        const ModelDesc &m = *unit("tinySA v0.3ZS304").model;
        AppliedGen h;
        GenConfig c = carrier(400e6, -10);
        c.path = GenPath::High;  // the unit starts at +9 dBm here
        c.on = true;
        QCOMPARE(genCommands(c, h, m),
                 QByteArrayList({"mode high output", "ext_gain 0", "sweep cw 400000000", "level -10", "output on"}));
        c.type = GenType::Sweep;  // the basic sweeps without `sweep go`
        c.spanHz = 20e6;
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"sweep center 400000000", "sweep span 20000000",
                                                       "level -10", "sweeptime 2"}));
        c.path = GenPath::Low;  // other connector: mode switch, RF off again
        c.type = GenType::NFM;
        c.freqHz = 100e6;
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"mode low output", "ext_gain 0", "sweep cw 100000000",
                                                       "level -10", "modulation nfm", "output on"}));
    }

    void clampToTheUnit()
    {
        const DeviceInfo basic = unit("tinySA v0.3ZS304");
        GenConfig c = carrier(100e6);
        c.path = GenPath::High;
        c.type = GenType::AM;  // no AM on the HIGH output
        c.levelChangeDb = 10;
        c = clampGen(c, basic);
        QCOMPARE(c.freqHz, 240e6);
        QCOMPARE(c.type, std::optional(GenType::CW));
        QCOMPARE(c.levelChangeDb, 0.0);
        c.type = GenType::FM;  // the Ultra's FM on a basic: the nearest FM
        QCOMPARE(clampGen(c, basic).type, std::optional(GenType::NFM));

        GenConfig u = carrier(6e9, -200);
        QCOMPARE(clampGen(u, unit("tinySA ULTRA ZS405")).freqHz, 5.327e9);  // mixer path
        u.mixer = false;
        QCOMPARE(clampGen(u, unit("tinySA ULTRA ZS405")).freqHz, 4.35e9);   // direct PLL path
        QCOMPARE(clampGen(u, unit("tinySA ULTRA+ ZS407")).freqHz, 6e9);     // MAX2871 reaches 6.3 GHz
        QCOMPARE(clampGen(u, unit("tinySA ULTRA+ ZS407")).levelDbm, -115.0);
        u.extGainDb = 10;
        u.levelDbm = 0;
        QCOMPARE(clampGen(u, unit("tinySA ULTRA+ ZS407")).levelDbm, -8.5);  // -18.5 + 10 dB amplifier
        u.type = GenType::Sweep;  // the whole sweep stays inside the limits
        u.freqHz = 1e6;
        u.spanHz = 10e6;
        QCOMPARE(clampGen(u, unit("tinySA ULTRA+ ZS407")).freqHz, 10e3 + 5e6);
        QCOMPARE(outputPathText(carrier(433.92e6), unit("tinySA ULTRA ZS405")), QString("Sine"));
        QCOMPARE(outputPathText(carrier(2e9), unit("tinySA ULTRA ZS405")), QString("Square wave, Mixer"));
    }

    void adoptKeepsTheUnitAsItIs()
    {
        DeviceInfo d = unit("tinySA ULTRA ZS405");
        d.gen.active = true;
        d.gen.extGainDb = 0;
        d.deviceGrid = {100e6, 100e6, 450};
        AppliedGen h;
        GenConfig c = adoptUnitGenerator(carrier(), d, &h);
        QCOMPARE(c.freqHz, 100e6);
        QVERIFY(!c.on && !c.type && !c.levelKnown);  // cannot be read
        QVERIFY(genCommands(c, h, *d.model).isEmpty());  // nothing changes on the unit
        c.on = true;
        QCOMPARE(genCommands(c, h, *d.model), QByteArrayList({"output on"}));
        c.type = GenType::CW;  // now the app knows: no sweep, no modulation, no level change
        QCOMPARE(genCommands(c, h, *d.model), QByteArrayList({"sweep abort", "modulation off", "levelchange 0"}));
        c.freqHz = 101e6;  // the level stays the unit's until it is set
        QCOMPARE(genCommands(c, h, *d.model), QByteArrayList({"sweep cw 101000000"}));
        c.levelKnown = true;
        c.levelDbm = -50;
        QCOMPARE(genCommands(c, h, *d.model), QByteArrayList({"level -50"}));
        c.freqHz = 2e9;  // the unit's path (mixer or not) was not known: set where it matters
        QCOMPARE(genCommands(c, h, *d.model), QByteArrayList({"output mixer", "sweep cw 2000000000", "level -50"}));

        d.deviceGrid = {90e6, 110e6, 450};  // a span: the unit sweeps (its menus never add modulation)
        d.gen.sweepS = 5;
        c = adoptUnitGenerator(carrier(), d, &h);
        QCOMPARE(c.type, std::optional(GenType::Sweep));
        QCOMPARE(c.spanHz, 20e6);
        QCOMPARE(c.sweepS, 5.0);
        QVERIFY(genCommands(c, h, *d.model).isEmpty());

        d.deviceGrid = {6e9, 6e9, 450};  // the LO's harmonic: an Ultra output mode, kept
        d.gen.extGainDb.reset();         // not read (no reply): the app's own goes out
        GenConfig stored = carrier();
        stored.extGainDb = 10;
        c = adoptUnitGenerator(stored, d, &h);
        QCOMPARE(c.freqHz, 6e9);
        QVERIFY(c.harmonic && c.mixer);
        QCOMPARE(genCommands(c, h, *d.model), QByteArrayList({"output mixer", "ext_gain 10"}));

        d.deviceGrid = {13e9, 13e9, 450};  // above even that (harmonic 4 or 5 on the unit): moved inside
        c = adoptUnitGenerator(stored, d, &h);
        QCOMPARE(c.freqHz, 12.0726e9);
        QCOMPARE(genCommands(c, h, *d.model),
                 QByteArrayList({"output mixer", "ext_gain 10", "sweep cw 12072600000"}));
    }

    void ultraOutputModes()
    {
        const DeviceInfo zs405 = unit("tinySA ULTRA ZS405");
        const DeviceInfo zs407 = unit("tinySA ULTRA+ ZS407");
        const QVector<UltraOutput> modes = ultraOutputs(zs405);
        QCOMPARE(modes.size(), 4);
        QCOMPARE(modes[0].text, QString("Accurate, max 5.33 GHz"));
        QCOMPARE(modes[1].text, QString("Cleanest, max 4.35 GHz"));
        QCOMPARE(modes[2].text, QString("Ultra accurate, max 12.07 GHz"));  // 3 x 4.35 GHz - 977.4 MHz
        QCOMPARE(modes[3].text, QString("Ultra cleanest, max 13.05 GHz"));  // 3 x 4.35 GHz
        QCOMPARE(modes[2].key, QString("ultra mixer"));
        QVERIFY(ultraOutputs(unit("tinySA v0.3ZS304")).isEmpty());  // the basic: LOW / HIGH port

        GenConfig u = carrier(20e9, -40);
        u.harmonic = true;
        QCOMPARE(clampGen(u, zs405).freqHz, 12.0726e9);
        QCOMPARE(clampGen(u, zs407).freqHz, 17.9226e9);  // MAX2871: LO up to 6.3 GHz
        u.mixer = false;
        QCOMPARE(clampGen(u, zs407).freqHz, 18.9e9);
        u.mixer = true;
        u.freqHz = 8e9;  // no command for it: the unit takes those frequencies after its mode switch
        AppliedGen h;
        QCOMPARE(genCommands(u, h, *zs405.model),
                 QByteArrayList({"mode low output", "ext_gain 0", "sweep cw 8000000000", "level -40"}));
        QCOMPARE(outputPathText(u, zs405), QString("Square wave, Mixer, LO harmonic"));
        QCOMPARE(outputPathText(carrier(5e9), zs405), QString("Square wave, Mixer"));
        GenConfig basic = u;  // the basic has none
        QVERIFY(!clampGen(basic, unit("tinySA v0.3ZS304")).harmonic);

        TrackConfig t;
        QCOMPARE(trackerLimits(zs405, t).maxHz, 5.327e9);
        t.harmonic = true;
        QCOMPARE(trackerLimits(zs405, t).maxHz, 12.0726e9);
        QCOMPARE(trackerPath(zs405, t, 5e9), 2);
        QCOMPARE(trackerPath(zs405, t, 6e9), 3);  // the LO's harmonic: another path, settles longer
        t.mixer = false;
        QCOMPARE(trackerPath(zs405, t, 5e9), 3);  // above the direct path's 4.35 GHz
        const GenConfig g = trackerGen(carrier(), zs405, t, 13e9, -30, true);
        QVERIFY(!g.mixer && g.harmonic);
        QCOMPARE(g.freqHz, 13e9);
        QVERIFY(clampTrack(t, unit("tinySA v0.3ZS304")).mixer);  // the basic: one LOW output
        QVERIFY(!clampTrack(t, unit("tinySA v0.3ZS304")).harmonic);
        SweepConfig c;
        c.grid = {1e9, 9e9, 101};
        QCOMPARE(clampToTracker(c, *zs405.model, zs405, t).grid.stopHz, 9e9);
        t.harmonic = false;
        QCOMPARE(clampToTracker(c, *zs405.model, zs405, t).grid.stopHz, 4.35e9);
    }

    void burstCommands()
    {
        DeviceInfo d = unit("tinySA ULTRA+ ZS407");
        d.hasBurst = true;
        AppliedGen h;
        GenConfig c = carrier(100e6, -40);
        genCommands(c, h, *d.model);
        c.type = GenType::Burst;  // the unit's defaults after the mode switch: 1 ms, 100 us, continuous
        QCOMPARE(genCommands(c, h, *d.model), QByteArrayList({"modulation burst"}));
        c.burstPeriodS = 2e-3;
        c.burstOnS = 500e-6;
        c.burstCount = 5;  // counted bursts wait for a trigger (not a setting: not in the diff)
        QCOMPARE(genCommands(c, h, *d.model),
                 QByteArrayList({"modulation period 2m", "modulation ontime 500u", "modulation count 5"}));
        c.burstPeriodS = 2;
        c.burstOnS = 1.5;
        QCOMPARE(genCommands(c, h, *d.model), QByteArrayList({"modulation period 2", "modulation ontime 1.5"}));
        c.type = GenType::CW;
        QCOMPARE(genCommands(c, h, *d.model), QByteArrayList({"modulation off"}));
        forgetGenCommand("modulation trigger", h);  // no setting: nothing to send again
        QVERIFY(genCommands(c, h, *d.model).isEmpty());

        GenConfig k = c;  // as set_burst_period / set_burst_on_time / set_burst_count
        k.type = GenType::Burst;
        k.burstPeriodS = 1e-3;
        k.burstOnS = 5e-3;
        k.burstCount = 100000;
        k = clampGen(k, d);
        QCOMPARE(k.burstOnS, 1e-3 - 1e-6);
        QCOMPARE(k.burstCount, 65535);
        k.burstPeriodS = 1e-7;
        QCOMPARE(clampGen(k, d).burstPeriodS, 2e-6);
        d.hasBurst = false;  // firmware before v1.4-305
        QCOMPARE(clampGen(k, d).type, std::optional(GenType::CW));
        QVERIFY(burstPossible(k));
        k.freqHz = 2e9;
        k.mixer = false;  // the direct path above 1.13 GHz: "MOD: BURST N/A"
        QVERIFY(!burstPossible(k));
    }

    void secondsInAndOut()
    {
        QCOMPARE(parseSeconds("100u"), std::optional(100e-6));
        QCOMPARE(parseSeconds(QString::fromUtf8("100 µs")), std::optional(100e-6));
        QCOMPARE(parseSeconds("1,5m"), std::optional(1.5e-3));
        QCOMPARE(parseSeconds("2 s"), std::optional(2.0));
        QCOMPARE(parseSeconds("2"), std::optional(2.0));
        QVERIFY(!parseSeconds("2 Hz"));
        QCOMPARE(formatSeconds(100e-6), QString::fromUtf8("100 µs"));
        QCOMPARE(formatSeconds(1.5e-3), QString("1.5 ms"));
        QCOMPARE(formatSeconds(89), QString("89 s"));
        QCOMPARE(parseSeconds(formatSeconds(0.000999)), std::optional(0.000999));
    }

    void burstDetectedAndTriggered()
    {
        for (const char *port : {"sim:ultra:old", "sim:basic"}) {  // no burst there
            DeviceLink old;
            QSignalSpy opened(&old, &DeviceLink::opened);
            old.open(port);
            QVERIFY(opened.wait(3000));
            QVERIFY2(!opened.first().first().value<DeviceInfo>().hasBurst, port);
        }
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened);
        link.open("sim:ultra");
        QVERIFY(opened.wait(3000));
        QVERIFY(opened.first().first().value<DeviceInfo>().hasBurst);
        auto *sim = qobject_cast<SimTransport *>(link.transport());
        GenConfig g = carrier(100e6, -50);
        link.setGenerator(g);
        link.setActivity(DeviceLink::Generator);
        link.triggerBurst();  // not bursting: nothing to trigger
        g.type = GenType::Burst;
        g.burstPeriodS = 4e-3;
        g.burstOnS = 1e-3;
        g.burstCount = 3;
        link.setGenerator(g);
        link.triggerBurst();  // after the settings it follows
        QTRY_COMPARE_WITH_TIMEOUT(sim->burstTriggers(), 1, 3000);
        QCOMPARE(sim->modulation(), QByteArray("burst"));
        QCOMPARE(sim->burstPeriod(), 4e-3);
        QCOMPARE(sim->burstOnTime(), 1e-3);
        QCOMPARE(sim->burstCount(), 3);
        const QByteArrayList rx = sim->received();
        QVERIFY(rx.indexOf("modulation count 3") < rx.indexOf("modulation trigger"));
        QCOMPARE(rx.count("modulation trigger"), 1);
    }

    void ctcssToneCommands()
    {
        DeviceInfo d = unit("tinySA ULTRA+ ZS407");
        d.ctcssTone = false;  // read at connect
        const ModelDesc &m = *d.model;
        GenConfig c = clampGen(carrier(10e6, -20), d);
        c.ctcss = false;
        c.type = GenType::FM;
        c.modHz = 100;
        AppliedGen h;
        h.ctcss = false;
        QByteArrayList cmds = genCommands(c, h, m);  // the unit's state already: nothing about it
        QCOMPARE(cmds.first(), QByteArray("mode low output"));
        QVERIFY(cmds.contains("modulation fm"));
        QVERIFY(!cmds.join(' ').contains("ctcss"));
        QVERIFY(!ctcssTone(c));

        c.ctcss = true;
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"modulation ctcss on"}));
        auto tone = ctcssTone(c);  // as sa_core.c: 10 x 100 Hz at 3 kHz, the rate at 3 kHz / 7
        QVERIFY(tone);
        QCOMPARE(tone->toneHz, 1000);
        QCOMPARE(tone->toneDevHz, 3000);
        QCOMPARE(tone->rateDevHz, 400);
        c.modHz = 67;  // the lowest CTCSS tone: 15 x 67 Hz
        c.fmDevHz = 1000;
        QCOMPARE(ctcssTone(c)->toneHz, 1005);
        QCOMPARE(ctcssTone(c)->rateDevHz, 100);
        c.modHz = 240;  // 4 x 240 Hz
        QCOMPARE(ctcssTone(c)->toneHz, 960);
        c.modHz = 255;  // from MAX_CTCSS_FREQ on: no tone
        QVERIFY(!ctcssTone(c));
        genCommands(c, h, m);

        h.mode.reset();  // a mode switch keeps the unit's config
        cmds = genCommands(c, h, m);
        QCOMPARE(cmds.first(), QByteArray("mode low output"));
        QVERIFY(!cmds.join(' ').contains("ctcss"));
        const AppliedGen k = keepUnitConfig(h);
        QCOMPARE(k.ctcss, std::optional(true));
        QVERIFY(!k.mode && !k.freq && !k.modulation);

        forgetGenCommand("modulation ctcss on", h);  // no reply: sent again
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"modulation ctcss on"}));
        forgetGenCommand("modulation fm", h);  // the type, not the tone
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"modulation fm"}));
        c.type = GenType::CW;
        c.ctcss = false;
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"modulation off", "modulation ctcss off"}));

        d.ctcssTone.reset();  // firmware before v1.4-341: unknown, never sent
        c = clampGen(c, d);
        QVERIFY(!c.ctcss);
        h = AppliedGen{};
        QVERIFY(!genCommands(c, h, m).join(' ').contains("ctcss"));
        d.ctcssTone = true;  // a generating unit taken over: its tone with it
        d.gen.active = true;
        QCOMPARE(adoptUnitGenerator(carrier(), d, &h).ctcss, std::optional(true));
        QCOMPARE(h.ctcss, std::optional(true));
    }

    void ctcssReadAtConnectAndSet()
    {
        for (const char *port : {"sim:ultra:noctcss", "sim:ultra:old", "sim:basic"}) {  // no command
            DeviceLink old;
            QSignalSpy opened(&old, &DeviceLink::opened);
            old.open(port);
            QVERIFY(opened.wait(3000));
            QVERIFY2(!opened.first().first().value<DeviceInfo>().ctcssTone, port);
        }
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened);
        link.open("sim:ultra:ctcss");  // its tone is on
        QVERIFY(opened.wait(3000));
        QCOMPARE(opened.first().first().value<DeviceInfo>().ctcssTone, std::optional(true));
        auto *sim = qobject_cast<SimTransport *>(link.transport());
        GenConfig g = carrier(10e6, -20);
        g.type = GenType::FM;
        g.modHz = 100;
        g.ctcss = true;  // as the session takes it from the unit
        link.setGenerator(g);
        link.setActivity(DeviceLink::Generator);
        QTRY_COMPARE_WITH_TIMEOUT(sim->modulation(), QByteArray("fm"), 3000);
        QCOMPARE(sim->modulationHz(), 100);
        QVERIFY(!sim->received().contains("modulation ctcss on"));  // the unit had it already
        g.ctcss = false;
        link.setGenerator(g);
        QTRY_VERIFY_WITH_TIMEOUT(!sim->ctcssTone, 3000);
        link.setActivity(DeviceLink::Analyzer);  // back and forth: the mode switches keep it
        link.setActivity(DeviceLink::Generator);
        QTRY_COMPARE_WITH_TIMEOUT(sim->received().count("modulation fm"), 2, 3000);  // the FM set up again
        const QByteArrayList rx = sim->received();
        QCOMPARE(rx.count("modulation ctcss off"), 1);
        QCOMPARE(rx.count("modulation ctcss"), 1);  // the read at connect
    }

    void extGainOneValue()
    {
        // the unit has one ext_gain for both modes, so has the app (the session keeps the
        // generator's equal to the analyzer's); the link sends it again after mode switches
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened), message(&link, &DeviceLink::message),
            frames(&link, &DeviceLink::frame);
        SweepConfig cfg;
        cfg.grid = {99e6, 101e6, 101};
        cfg.extGainDb = 20;  // the app's, from its ini: replaces the unit's 0
        link.setConfig(cfg);
        link.open("sim:ultra");
        QVERIFY(opened.wait(3000));
        QCOMPARE(opened.first().first().value<DeviceInfo>().extGainDb, std::optional(0.0));
        QVERIFY(std::any_of(message.begin(), message.end(), [](const QList<QVariant> &m) {
            return m.first().toString().contains("external gain was +0.0 dB: the app sets its own, +20.0 dB");
        }));
        auto *sim = qobject_cast<SimTransport *>(link.transport());
        auto peak = [&] {  // of the last complete sweep: the sim's 100 MHz carrier is -30 dBm
            frames.clear();
            link.setScan(DeviceLink::Once);
            const bool ok = QTest::qWaitFor([&] {
                return std::any_of(frames.begin(), frames.end(), [](const QList<QVariant> &f) {
                    return f.first().value<SweepFrame>().complete;
                });
            }, 5000);
            const SweepFrame f = frames.last().first().value<SweepFrame>();
            return ok ? *std::max_element(f.dBm.begin(), f.dBm.end()) : 0.0;
        };
        QCOMPARE(std::round(peak()), -50.0);  // at the device under test: 20 dB less
        QCOMPARE(sim->extGain(), 20.0);

        GenConfig g = carrier(100e6, -40);
        g.extGainDb = 20;
        link.setGenerator(g);
        link.setActivity(DeviceLink::Generator);
        QTRY_COMPARE_WITH_TIMEOUT(sim->modulation(), QByteArray("off"), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(sim->generating(), true, 3000);
        g.extGainDb = cfg.extGainDb = -6;  // edited on the generator page: both, as the session does
        link.setGenerator(g);
        link.setConfig(cfg);
        QTRY_COMPARE_WITH_TIMEOUT(sim->extGain(), -6.0, 3000);
        link.setActivity(DeviceLink::Analyzer);
        QCOMPARE(std::round(peak()), -24.0);  // an attenuator: levels 6 dB higher at the DUT
        QCOMPARE(sim->extGain(), -6.0);
        cfg.extGainDb = 0;
        link.setConfig(cfg);
        QCOMPARE(std::round(peak()), -30.0);

        // a generating unit is followed: its external gain is taken, without a message
        DeviceLink gen;
        QSignalSpy genOpened(&gen, &DeviceLink::opened), adopted(&gen, &DeviceLink::generatorAdopted),
            genMessage(&gen, &DeviceLink::message);
        gen.setConfig(cfg);  // the app's 0
        gen.open("sim:ultra:gen:extgain=10");
        QVERIFY(genOpened.wait(3000));
        QCOMPARE(genOpened.first().first().value<DeviceInfo>().extGainDb, std::optional(10.0));
        QCOMPARE(adopted.count(), 1);
        QCOMPARE(adopted.first().first().value<GenConfig>().extGainDb, 10.0);
        QVERIFY(std::none_of(genMessage.begin(), genMessage.end(), [](const QList<QVariant> &m) {
            return m.first().toString().contains("external gain");
        }));
    }

    void levelUnits()
    {
        auto near = [](double a, double b) { return std::abs(a - b) < 1e-6 * std::max(1.0, std::abs(b)); };
        QVERIFY(near(levelFromDbm(-30, LevelUnit::dBmV), -30 + 46.9897));  // 1 mW in 50 ohm = 223.6 mV
        QVERIFY(near(levelFromDbm(-30, LevelUnit::dBuV), -30 + 106.9897));
        QVERIFY(near(levelFromDbm(-30, LevelUnit::dBW), -60));
        QVERIFY(near(levelFromDbm(-30, LevelUnit::W), 1e-6));
        QVERIFY(near(levelFromDbm(-30, LevelUnit::mW), 1e-3));
        QVERIFY(near(levelFromDbm(0, LevelUnit::V), std::sqrt(0.05)));
        QVERIFY(near(levelFromDbm(-30, LevelUnit::mV), std::sqrt(50e-6) * 1e3));
        QVERIFY(near(levelFromDbm(6, LevelUnit::V, true), std::pow(10, 0.3)));  // a voltage ratio
        QVERIFY(near(levelFromDbm(10, LevelUnit::W, true), 10));                // a power ratio
        QVERIFY(near(levelFromDbm(3, LevelUnit::dBmV, true), 3));               // dB stays dB
        QCOMPARE(formatLevel(-30, LevelUnit::dBm), QString("-30.0 dBm"));
        QCOMPARE(formatLevel(-30, LevelUnit::dBmV), QString("17.0 dBmV"));
        QCOMPARE(formatLevel(-30, LevelUnit::dBuV), QString::fromUtf8("77.0 dBµV"));
        QCOMPARE(formatLevel(-30, LevelUnit::W), QString::fromUtf8("1 µW"));
        QCOMPARE(formatLevel(-27, LevelUnit::W), QString::fromUtf8("2 µW"));
        QCOMPARE(formatLevel(-30, LevelUnit::mW), QString("0.001 mW"));
        QCOMPARE(formatLevel(-30, LevelUnit::V), QString("7.07 mV"));
        QCOMPARE(formatLevel(20, LevelUnit::V), QString("2.24 V"));
        QCOMPARE(formatLevel(-30, LevelUnit::mV), QString("7.07 mV"));
        QCOMPARE(formatLevel(0, LevelUnit::V, true), QString("x 1"));
        QCOMPARE(formatLevel(qQNaN(), LevelUnit::dBmV), QString("-"));
        QCOMPARE(levelUnitFromName("dBuV"), LevelUnit::dBuV);
        QCOMPARE(levelUnitFromName(QString::fromUtf8("dBµV")), LevelUnit::dBuV);
        QCOMPARE(levelUnitFromName("nonsense"), LevelUnit::dBm);

        QTemporaryDir dir;  // ini: view/unit; older files only have view/linear (W)
        const QString path = dir.filePath("u.ini");
        {
            QSettings old(path, QSettings::IniFormat);
            old.setValue("view/linear", true);
        }
        AppSettings a;
        {
            QSettings s(path, QSettings::IniFormat);
            a.load(s);
        }
        QCOMPARE(a.view.unit, LevelUnit::W);
        a.view.unit = LevelUnit::dBuV;
        {
            QSettings s(path, QSettings::IniFormat);
            a.save(s);
        }
        QSettings s(path, QSettings::IniFormat);
        QVERIFY(!s.contains("view/linear"));
        AppSettings b;
        b.load(s);
        QCOMPARE(b.view.unit, LevelUnit::dBuV);
    }

    void extGainOneKeyInTheIni()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("t.ini");
        {
            QSettings old(path, QSettings::IniFormat);  // from before: only the generator's
            old.setValue("generator/ext_gain_db", 7.5);
        }
        AppSettings a;
        {
            QSettings s(path, QSettings::IniFormat);
            a.load(s);
        }
        QCOMPARE(a.config.extGainDb, 7.5);
        QCOMPARE(a.gen.extGainDb, 7.5);
        a.config.extGainDb = a.gen.extGainDb = -3;
        {
            QSettings s(path, QSettings::IniFormat);
            a.save(s);
        }
        QSettings s(path, QSettings::IniFormat);
        QCOMPARE(s.value("device/ext_gain_db").toDouble(), -3.0);
        QVERIFY(!s.contains("generator/ext_gain_db"));
        AppSettings b;
        b.load(s);
        QCOMPARE(b.gen.extGainDb, -3.0);
    }

    void lostCommandIsSentAgain()
    {
        const ModelDesc &m = *unit("tinySA ULTRA ZS405").model;
        AppliedGen h;
        GenConfig c = carrier();
        c.on = true;
        genCommands(c, h, m);
        c.on = false;  // RF off ...
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"output off"}));
        forgetGenCommand("output off", h);  // ... got no reply: it goes out again
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"output off"}));
        forgetGenCommand("level -40", h);
        forgetGenCommand("modulation off", h);
        QCOMPARE(genCommands(c, h, m), QByteArrayList({"modulation off", "level -40"}));
    }

    void modulationStopsBeforeASweepStarts()
    {
        const ModelDesc &basic = *unit("tinySA v0.3ZS304").model;
        AppliedGen h;
        GenConfig c = carrier(100e6);
        c.type = GenType::NFM;
        genCommands(c, h, basic);
        c.type = GenType::Sweep;  // the basic sweeps as soon as it has a span
        const QByteArrayList cmds = genCommands(c, h, basic);
        QCOMPARE(cmds.value(0), QByteArray("modulation off"));
        QVERIFY(cmds.indexOf("sweep span 10000000") > 0);
    }

    void oldFirmwareDoesNotHang()
    {
        // v1.4-220: a command right after output/modulation/levelchange/ext_gain hangs the shell
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened);
        link.open("sim:ultra:old");
        QVERIFY(opened.wait(3000));
        QVERIFY(opened.first().first().value<DeviceInfo>().directCommandRace());
        auto *sim = qobject_cast<SimTransport *>(link.transport());
        GenConfig g = carrier(100e6, -50);
        link.setGenerator(g);
        link.setActivity(DeviceLink::Generator);
        g.on = true;
        link.setGenerator(g);  // output on, then (racing) the level and sweep commands below
        g.type = GenType::AM;
        g.levelDbm = -45;
        g.freqHz = 101e6;
        link.setGenerator(g);
        g.type = GenType::Sweep;
        g.levelChangeDb = -10;
        link.setGenerator(g);
        QTRY_VERIFY_WITH_TIMEOUT(sim->received().contains("sweep go"), 8000);
        QVERIFY(!sim->hung());
        QVERIFY(sim->rfOn());
        QCOMPARE(sim->outputLevel(), -45.0);
        link.send("vbat");  // a console command after `modulation off`, `levelchange -10` ...
        QSignalSpy reply(&link, &DeviceLink::reply);
        QVERIFY(reply.wait(3000));
        QVERIFY(!sim->hung());
        // the emulation itself: without the wait it hangs, as the real unit did
        sim->write("output off\rlevel -30\r");
        QVERIFY(sim->hung());
    }

    void portSwitchSendsTheLevel()
    {
        // a taken-over unit (level unknown) switched to the other port: the mode switch sets the
        // unit's level to its maximum, so the app's level must go out before RF can go on
        Session session;
        QSignalSpy activity(&session, &Session::activityChanged), traffic(&session, &Session::traffic);
        session.connectDevice("sim:basic:gen");
        QTRY_VERIFY_WITH_TIMEOUT(!activity.isEmpty(), 5000);
        GenConfig g = session.generator();
        QVERIFY(!g.levelKnown);
        g.path = GenPath::High;
        session.setGenerator(g);
        QVERIFY(session.generator().levelKnown);
        auto sent = [&](const QString &prefix) {
            for (const auto &t : traffic)
                if (t.at(0).toBool() && t.at(1).toString().startsWith(prefix))
                    return true;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(sent("mode high output") && sent("level "), 3000);
        session.disconnectDevice();
    }

    void generatorNeverScans()
    {
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened), frames(&link, &DeviceLink::frame),
            activity(&link, &DeviceLink::activityChanged);
        link.open("sim:ultra");
        QVERIFY(opened.wait(3000));
        QTRY_VERIFY_WITH_TIMEOUT(!activity.isEmpty(), 1000);
        QCOMPARE(activity.last().first().toInt(), int(DeviceLink::Analyzer));
        auto *sim = qobject_cast<SimTransport *>(link.transport());
        link.setScan(DeviceLink::Continuous);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 3000);
        GenConfig g = carrier(100e6, -50);
        link.setGenerator(g);
        link.setActivity(DeviceLink::Generator);  // stops the scan, resumes the unit
        QTRY_VERIFY_WITH_TIMEOUT(sim->generating(), 3000);
        QCOMPARE(activity.last().first().toInt(), int(DeviceLink::Generator));
        g.on = true;
        link.setGenerator(g);
        QTRY_VERIFY_WITH_TIMEOUT(sim->rfOn(), 2000);
        QCOMPARE(sim->outputLevel(), -50.0);
        link.setScan(DeviceLink::Once);  // ignored
        QTest::qWait(300);
        QCOMPARE(sim->scansWhileGenerating, 0);
        const QByteArrayList rx = sim->received();
        for (const char *c : {"resume", "mode low output", "level -50", "output on"})
            QVERIFY2(rx.contains(c), c);
        QVERIFY(rx.indexOf("resume") < rx.indexOf("mode low output"));
        QVERIFY(rx.indexOf("level -50") < rx.indexOf("output on"));
        link.setActivity(DeviceLink::Analyzer);  // back to the scan from before
        QTRY_VERIFY_WITH_TIMEOUT(!sim->generating(), 2000);
        const int before = int(frames.size());
        QTRY_VERIFY_WITH_TIMEOUT(frames.size() > before + 2, 5000);
        QCOMPARE(sim->scansWhileGenerating, 0);
    }

    void followsAGeneratingUnit_data()
    {
        QTest::addColumn<QString>("port");
        QTest::addColumn<QByteArray>("modulation");
        QTest::newRow("ultra") << "sim:ultra:gen" << QByteArray("fm");
        QTest::newRow("basic") << "sim:basic:gen" << QByteArray("nfm");
    }
    void followsAGeneratingUnit()
    {
        QFETCH(QString, port);
        QFETCH(QByteArray, modulation);
        DeviceLink link;
        QSignalSpy opened(&link, &DeviceLink::opened), adopted(&link, &DeviceLink::generatorAdopted),
            activity(&link, &DeviceLink::activityChanged);
        link.setGenerator(carrier(433.92e6, -60));  // the app's stored settings
        link.open(port);
        QVERIFY(opened.wait(3000));
        QTRY_VERIFY_WITH_TIMEOUT(!activity.isEmpty(), 1000);
        QCOMPARE(activity.last().first().toInt(), int(DeviceLink::Generator));
        QCOMPARE(adopted.size(), 1);
        const GenConfig g = adopted.first().first().value<GenConfig>();
        QCOMPARE(g.freqHz, 100e6);
        QVERIFY(!g.on && !g.type && !g.levelKnown);
        auto *sim = qobject_cast<SimTransport *>(link.transport());
        QTest::qWait(300);  // nothing changed on the unit
        QVERIFY(sim->generating() && sim->rfOn());
        QCOMPARE(sim->modulation(), modulation);
        QCOMPARE(sim->outputLevel(), -30.0);
        link.setScan(DeviceLink::Continuous);  // a generating unit is never scanned
        QTest::qWait(200);
        QCOMPARE(sim->scansWhileGenerating, 0);
        GenConfig off = g;  // RF off: only that is sent
        off.on = false;
        link.setGenerator(off);
        QTRY_VERIFY_WITH_TIMEOUT(!sim->rfOn(), 2000);
        QCOMPARE(sim->modulation(), modulation);
    }
};

// ---------------------------------------------------------------------------

static SweepFrame frame(const ScanGrid &g, QVector<double> dBm, bool complete = true, int received = -1)
{
    SweepFrame f{1, g, dBm};
    f.received = received < 0 ? int(dBm.size()) : received;
    f.complete = complete;
    return f;
}

class TstTraces : public QObject {
    Q_OBJECT
private slots:
    void averagesPower()
    {
        const ScanGrid g{0, 100, 2};
        TraceSet t;
        t.setAverage(2);
        t.add(frame(g, {-40, -40}));
        t.add(frame(g, {-30, -30}));
        QVERIFY(std::abs(t.live().dBm[0] - 10 * std::log10((1e-4 + 1e-3) / 2)) < 1e-9);
        t.add(frame(g, {-30, -30}));  // window of 2: the -40 sweep has left
        QVERIFY(std::abs(t.live().dBm[0] - -30) < 1e-9);
    }
    void countsAndRestartsTheAverage()
    {
        const ScanGrid g{0, 100, 2};
        TraceSet t;
        t.setAverage(4);
        QCOMPARE(t.averaged(), 0);
        for (int i = 1; i <= 6; ++i) {
            t.add(frame(g, {-40, -40}));
            QCOMPARE(t.averaged(), std::min(i, 4));  // 1/4, 2/4, 3/4, 4/4, then stays 4/4
        }
        t.add(frame(g, {-10, qQNaN()}, false, 1));  // a partial sweep does not count
        QCOMPARE(t.averaged(), 4);
        t.setHold(Hold::Max);
        t.add(frame(g, {-20, -20}));
        t.restartAverage();                         // average and max hold forget the -40 / -20 sweeps
        QCOMPARE(t.averaged(), 0);
        t.add(frame(g, {-60, -60}));
        QCOMPARE(t.averaged(), 1);
        QVERIFY(std::abs(t.live().dBm[0] - -60) < 1e-9);
        t.setAverage(8);                            // a new length starts over too
        QCOMPARE(t.averaged(), 0);
    }
    void holdsAndRestarts()
    {
        const ScanGrid g{0, 100, 3};
        TraceSet t;
        t.setHold(Hold::Max);
        t.add(frame(g, {-50, -20, -60}));
        t.add(frame(g, {-40, -30, -70}));
        QCOMPARE(t.live().dBm, QVector<double>({-40, -20, -60}));
        t.setHold(Hold::Min);
        t.add(frame(g, {-40, -30, -70}));
        t.add(frame(g, {-45, -10, -65}));
        QCOMPARE(t.live().dBm, QVector<double>({-45, -30, -70}));
        t.add(frame({0, 200, 3}, {-1, -2, -3}));  // new range: history restarts
        QCOMPARE(t.live().dBm, QVector<double>({-1, -2, -3}));
    }
    void partialFramesKeepThePreviousSweep()
    {
        const ScanGrid g{0, 100, 3};
        TraceSet t;
        t.add(frame(g, {-50, -50, -50}));
        t.add(frame(g, {-10, qQNaN(), qQNaN()}, false, 1));  // sweep cursor at point 1
        QCOMPARE(t.live().dBm, QVector<double>({-10, -50, -50}));
    }
    void normalizesAgainstTheReference()
    {
        TraceSet t;
        t.add(frame({0, 100, 3}, {-50, -40, -30}));
        t.setNormalize(true);
        QCOMPARE(t.normalizedView(t.live()).dBm, QVector<double>({0, 0, 0}));
        const Trace other{{0, 50, 3}, {-50, -45, -40}};  // reference interpolated: -50 -45 -40
        QCOMPARE(t.normalizedView(other).dBm, QVector<double>({0, 0, 0}));
        t.setNormalize(false);
        QCOMPARE(t.normalizedView(other).dBm, other.dBm);
        QVERIFY(std::isnan(other.at(60)));
        QCOMPARE(other.at(37.5), -42.5);
    }
    void memoriesAreCopies()
    {
        TraceSet t;
        t.add(frame({0, 100, 2}, {-50, -40}));
        t.store(1);
        t.add(frame({0, 100, 2}, {-20, -20}));
        QCOMPARE(t.memory(1).dBm, QVector<double>({-50, -40}));
        QVERIFY(t.memory(0).isEmpty());
    }
};

class TstCsv : public QObject {
    Q_OBJECT
private slots:
    void roundTripBothFormats()
    {
        const Trace t{{100e6, 200e6, 3}, {-45.25, qQNaN(), -30}};
        for (const CsvFormat fmt : {CsvFormat{';', true}, CsvFormat{',', false}, CsvFormat{'\t', false}}) {
            const QString path = QDir::tempPath() + "/tst_trace.csv";
            QString err;
            QVERIFY(writeTraceCsv(path, t, fmt, &err, "title"));
            const auto back = readTraceCsv(path, &err);
            QVERIFY2(back.has_value(), qPrintable(err));
            QCOMPARE(back->grid, t.grid);
            QCOMPARE(back->dBm[0], -45.25);
            QVERIFY(std::isnan(back->dBm[1]));
            QFile::remove(path);
        }
    }
    void mhzForWirelessManagers()
    {
        const Trace t{{470e6, 470.5e6, 3}, {-85.25, qQNaN(), -30}};
        const QString path = QDir::tempPath() + "/tst_mhz.csv";
        QString err;
        QVERIFY(writeTraceCsv(path, t, CsvFormat{';', true, true}, &err, "title"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(f.readLine(), QByteArray("470.000000,-85.25\n"));  // no title or header, comma and point
        f.close();
        const auto back = readTraceCsv(path, &err);
        QVERIFY2(back.has_value(), qPrintable(err));
        QCOMPARE(back->grid, t.grid);
        QCOMPARE(back->dBm[0], -85.25);
        QVERIFY(std::isnan(back->dBm[1]));
        QFile::remove(path);
    }
    void logGridsRoundTrip()
    {
        for (const int pieces : {logPieces(10e3, 1e9, 101), 0}) {  // an analyzer's / a tracking sweep
            Trace t{{10e3, 1e9, 101, true, pieces}, QVector<double>(101, -50.5)};
            const QString path = QDir::tempPath() + "/tst_log.csv";
            QString err;
            QVERIFY(writeTraceCsv(path, t, CsvFormat{';', false}, &err, "title"));
            const auto back = readTraceCsv(path, &err);
            QVERIFY2(back.has_value(), qPrintable(err));
            QCOMPARE(back->grid, t.grid);
            QFile::remove(path);
        }
    }
    void rejectsUnevenFrequencies()
    {
        const QString path = QDir::tempPath() + "/tst_uneven.csv";
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("1000;-10\n2000;-11\n3500;-12\n");
        f.close();
        QString err;
        QVERIFY(!readTraceCsv(path, &err));
        QVERIFY(err.contains("evenly"));
        QFile::remove(path);
    }
};

class TstMarkers : public QObject {
    Q_OBJECT
private slots:
    void parabolicPeakFindsTheVertex()
    {
        // samples of -20 - (f - 1003.3)^2 / 50 at 1000..1010: vertex between points
        Trace t{{1000, 1010, 11}, {}};
        for (int i = 0; i < 11; ++i)
            t.dBm << -20 - std::pow(1000 + i - 1003.3, 2) / 50;
        const QPointF max = tracePeak(t, true);
        QVERIFY(std::abs(max.x() - 1003.3) < 1e-6);
        QVERIFY(std::abs(max.y() - -20) < 1e-9);
        QCOMPARE(tracePeak(t, false).x(), 1010.0);  // the minimum sits at the edge
        QVERIFY(std::isnan(tracePeak(Trace{}, true).x()));
    }
    void peakRobustToQuantisedLevels()
    {
        // Gaussian RBW response (-3 dB at +-15 kHz) sampled every 10 kHz and rounded to the
        // unit's 0.5 dB steps, for carriers at several offsets between two points
        for (double offset : {0.0, 1.5, 3.0, 4.2, 5.0, 6.8}) {
            const double f0 = 110e6 + offset * 1e3;
            Trace t{{109.9e6, 110.1e6, 21}, {}};
            for (int i = 0; i < 21; ++i) {
                const double x = (t.grid.freq(i) - f0) / 15e3;
                t.dBm << std::round((-56 - 3.01 * x * x) * 2) / 2;
            }
            const double err = tracePeak(t, true).x() - f0;
            QVERIFY2(std::abs(err) < 1000, qPrintable(QString("offset %1 kHz: error %2 Hz").arg(offset).arg(err)));
        }
    }
    void deltaMarkersReferToTheFirstNormalMarker()
    {
        const Trace t{{0, 100, 11}, {-10, -20, -30, -40, -50, -60, -70, -80, -90, -100, -110}};
        MarkerSet ms;
        ms.add(20);
        ms.add(52);  // reads the nearest point, 50
        ms.setDelta(1, true);
        const MarkerReading r = ms.read(1, t);
        QCOMPARE(r.name, QString("D2"));
        QCOMPARE(r.hz, 50.0);
        QCOMPARE(r.dHz, 30.0);
        QCOMPARE(r.dDb, -30.0);
        QVERIFY(std::isnan(ms.read(0, t).dDb));  // normal markers have no delta
        ms.remove(0);  // no reference left: the delta marker reads absolute only
        QVERIFY(std::isnan(ms.read(0, t).dDb));
        QCOMPARE(ms.selected(), 0);
    }
    void selectionFollowsRemovals()
    {
        MarkerSet ms;
        ms.add(1);
        ms.add(2);
        ms.add(3);
        ms.select(2);
        ms.remove(0);
        QCOMPARE(ms.selected(), 1);
        ms.remove(1);
        QCOMPARE(ms.selected(), -1);
        QCOMPARE(ms.near(1.5, 1), 0);
        QCOMPARE(ms.near(10, 1), -1);
    }
    void serializeRoundTrip()
    {
        MarkerSet ms;
        ms.add(100e6);
        ms.add(433.92e6);
        ms.setDelta(1, true);
        MarkerSet back;
        back.deserialize(ms.serialize());
        QCOMPARE(back.count(), 2);
        QCOMPARE(back.all()[1].hz, 433.92e6);
        QVERIFY(back.all()[1].delta);
    }
};

// A fake STM32 ROM bootloader: DfuSe commands on a flash array. Programming
// only works on erased (0xFF) bytes, like the real chip.
class FakeBootloader : public DfuTransport {
public:
    FakeBootloader(int pages) : flash(pages * 2048, char(0x5A)) {}
    QByteArray flash;
    quint32 base = 0x08000000, pointer = 0;
    int state = 2, status = 0, busy = 0, erased = 0;
    QString layoutText = "@Internal Flash  /0x08000000/064*0002Kg";

    bool out(quint8 request, quint16 value, const QByteArray &data) override
    {
        if (request == 4 || request == 6) {  // CLRSTATUS, ABORT
            state = 2, status = 0;
            return true;
        }
        if (request != 1)
            return false;
        if (data.isEmpty()) {  // leave
            state = 7;
            return true;
        }
        if (value == 0) {  // DfuSe command
            const quint32 a = data.size() >= 5 ? quint32(quint8(data[1])) | quint32(quint8(data[2])) << 8
                                                   | quint32(quint8(data[3])) << 16 | quint32(quint8(data[4])) << 24
                                               : 0;
            if (quint8(data[0]) == 0x21)
                pointer = a;
            else if (quint8(data[0]) == 0x41) {
                if (a < base || a >= base + quint32(flash.size())) {
                    status = 8, state = 10;  // errADDRESS
                    return true;
                }
                flash.replace(int((a - base) / 2048 * 2048), 2048, QByteArray(2048, char(0xFF)));
                ++erased;
            }
        } else {  // program at pointer + (block - 2) * length
            const quint32 a = pointer + quint32(value - 2) * quint32(data.size());
            for (int i = 0; i < data.size(); ++i) {
                const qsizetype at = qsizetype(a - base) + i;
                if (at < 0 || at >= flash.size() || flash[at] != char(0xFF)) {
                    status = 6, state = 10;  // errPROG
                    return true;
                }
                flash[at] = data[i];
            }
        }
        state = 3, busy = 1;
        return true;
    }
    bool in(quint8 request, quint16 value, int length, QByteArray *data) override
    {
        if (request == 3) {  // GETSTATUS: DNLOAD-SYNC -> DNBUSY -> DNLOAD-IDLE
            if (state == 3)
                state = busy-- > 0 ? 4 : 5;
            else if (state == 4)
                state = 5;
            *data = QByteArray(1, char(status)) + QByteArray("\x01\x00\x00", 3) + char(state) + char(0);
            return true;
        }
        if (request == 2 && value >= 2) {  // UPLOAD
            const qsizetype at = qsizetype(pointer - base) + qsizetype(value - 2) * length;
            *data = flash.mid(at, length);
            state = 9;
            return true;
        }
        return false;
    }
    QString layout() override { return layoutText; }
};

class TstDfu : public QObject {
    Q_OBJECT
private slots:
    void intelHex()
    {
        FirmwareImage img;
        QString err;
        const QByteArray hex = ":020000040800F2\n"
                               ":10000000000102030405060708090A0B0C0D0E0F78\n"
                               ":10001000101112131415161718191A1B1C1D1E1F68\n"
                               ":00000001FF\n";
        QVERIFY2(parseIntelHex(hex, &img, &err), qPrintable(err));
        QCOMPARE(img.segments.size(), 1);
        QCOMPARE(img.segments[0].address, 0x08000000u);
        QCOMPARE(img.segments[0].data.size(), 32);
        QCOMPARE(quint8(img.segments[0].data[31]), quint8(0x1F));
        FirmwareImage bad;
        QVERIFY(!parseIntelHex(":10000000000102030405060708090A0B0C0D0E0F79\n", &bad, &err));
        QVERIFY(err.contains("checksum"));
    }
    void dfuSeFile()
    {
        auto le32 = [](quint32 v) { return QByteArray(1, char(v)) + char(v >> 8) + char(v >> 16) + char(v >> 24); };
        const QByteArray payload(1000, char(0x42));
        QByteArray target = QByteArray("Target") + char(0) + le32(0) + QByteArray(255, 0)
                            + le32(8 + payload.size()) + le32(1) + le32(0x08004000) + le32(payload.size()) + payload;
        QByteArray file = QByteArray("DfuSe") + char(1) + le32(0) + char(1) + target;
        file += QByteArray("\xff\xff\x11\xdf\x83\x04\x1a\x01", 8) + "UFD" + char(16);
        file += le32(0);
        const quint32 crc = dfuCrc(file);
        file.replace(file.size() - 4, 4, le32(crc));
        FirmwareImage img;
        QString err;
        QVERIFY2(parseDfuSe(file, &img, &err), qPrintable(err));
        QCOMPARE(img.segments[0].address, 0x08004000u);
        QCOMPARE(img.segments[0].data, payload);
        file[20] = 'x';  // corrupt: CRC no longer matches
        QVERIFY(!parseDfuSe(file, &img, &err));
    }
    void flashLayout()
    {
        bool ok = false;
        FlashLayout f = parseFlashLayout("@Internal Flash  /0x08000000/128*0002Kg", &ok);
        QVERIFY(ok);
        QCOMPARE(f.size(), 256 * 1024);
        QCOMPARE(f.pageStart(0x08000FFF), 0x08000800u);
        f = parseFlashLayout("@Internal Flash  /0x08000000/04*016Kg,01*064Kg,07*128Kg", &ok);
        QCOMPARE(f.pageSize(0x08010000), 64 * 1024);
        QCOMPARE(f.pageSize(0x08100000), 0);
        parseFlashLayout("garbage", &ok);
        QVERIFY(!ok);
    }
    void writeVerifyKeepsTheConfigPage()
    {
        FakeBootloader boot(64);
        DfuSe dfu(boot);
        QCOMPARE(dfu.flash().size(), 128 * 1024);
        FirmwareImage img;
        QByteArray fw(5000, 0);
        for (int i = 0; i < fw.size(); ++i)
            fw[i] = char(i * 7);
        img.segments.append(FirmwareSegment{0x08000000, fw});
        img.segments.append(FirmwareSegment{0x08010100, QByteArray(300, char(0x33))});
        int lastPercent = 0;
        DfuSe writer(boot, [&](int p, const QString &) { lastPercent = p; });
        QVERIFY2(writer.write(img, true), qPrintable(writer.error()));
        QCOMPARE(lastPercent, 100);
        QCOMPARE(boot.erased, 4);  // 3 pages for 5000 bytes + 1 for the second segment
        QCOMPARE(boot.flash.left(5000), fw);
        QCOMPARE(boot.flash.mid(0x10100, 300), QByteArray(300, char(0x33)));
        QCOMPARE(boot.flash.right(2048), QByteArray(2048, char(0x5A)));  // "config" page untouched
        QByteArray all;
        QVERIFY(dfu.readFlash(&all));
        QCOMPARE(all, boot.flash);
        QVERIFY(dfu.leave());
        QCOMPARE(boot.state, 7);
    }
    void errorsAreReported()
    {
        FakeBootloader boot(64);
        DfuSe dfu(boot);
        FirmwareImage tooBig;
        tooBig.segments.append(FirmwareSegment{0x08000000, QByteArray(200 * 1024, 1)});
        QVERIFY(!dfu.write(tooBig, false));
        QVERIFY(dfu.error().contains("does not fit"));
        // a bootloader error (erase outside its flash) is reported, not ignored
        boot.base = 0x08010000;
        FirmwareImage img;
        img.segments.append(FirmwareSegment{0x08000000, QByteArray(10, 1)});
        QVERIFY(!dfu.write(img, false));
        QVERIFY2(dfu.error().contains("address out of range"), qPrintable(dfu.error()));
        // unknown layout: the smallest tinySA flash (64 x 2 KB) is assumed
        boot.layoutText.clear();
        QCOMPARE(DfuSe(boot).flash().size(), 128 * 1024);
    }
};

// Tracking generator: two simulated units on one bench, the analyzer's input cabled to the
// generator's output (":thru") or through a 150 MHz low-pass (":lpf").
class TstTracking : public QObject {
    Q_OBJECT
    struct Bench {
        DeviceLink analyzer, tg;
        QSignalSpy frames{&analyzer, &DeviceLink::frame}, scan{&analyzer, &DeviceLink::scanChanged},
            msg{&analyzer, &DeviceLink::message};
        SimTransport *a = nullptr, *g = nullptr;
        bool open(const QString &analyzerPort, const QString &tgPort)
        {
            analyzer.setTracker(&tg);
            QSignalSpy ao(&analyzer, &DeviceLink::opened), go(&tg, &DeviceLink::opened);
            analyzer.open(analyzerPort);
            if (!tgPort.isEmpty())
                tg.open(tgPort);
            if (!ao.wait(3000) || (!tgPort.isEmpty() && go.isEmpty() && !go.wait(3000)))
                return false;
            a = qobject_cast<SimTransport *>(analyzer.transport());
            g = qobject_cast<SimTransport *>(tg.transport());
            return true;
        }
        SweepFrame last() const { return frames.last().first().value<SweepFrame>(); }
        bool complete() const { return !frames.isEmpty() && last().complete; }
    };

private slots:
    void initTestCase() { qRegisterMetaType<SweepFrame>(); qRegisterMetaType<DeviceInfo>(); }

    void generatorUnitGoesToOutputMode()
    {
        Bench b;
        QVERIFY(b.open("sim:ultra:thru", "sim:ultra"));
        QTRY_VERIFY_WITH_TIMEOUT(b.g->generating(), 3000);  // its role: output mode, RF off
        QVERIFY(!b.g->rfOn());
        QTRY_VERIFY_WITH_TIMEOUT(b.g->uiOff(), 3000);  // screen off from the start (no redraws)
        QCOMPARE(b.g->uiText(), QByteArray("Tracking generator"));  // which tinySA is which
        const QByteArrayList g = b.g->received();
        QVERIFY(g.indexOf("ui off \"Tracking generator\"") < g.indexOf("mode low output"));
        QVERIFY(!g.contains("ui on"));
    }

    void screenTextsTellTheUnitsApart()
    {
        Bench b;
        QVERIFY(b.open("sim:ultra:thru", "sim:ultra"));
        b.analyzer.setScan(DeviceLink::Continuous);  // an ordinary sweep (Track off)
        QTRY_VERIFY_WITH_TIMEOUT(b.a->uiOff() && b.a->uiText() == "Spectrum analyzer", 3000);
        QCOMPARE(b.g->uiText(), QByteArray("Tracking generator"));
        b.tg.close();  // its screen back; the analyzer's text goes, its screen stays off
        QTRY_VERIFY_WITH_TIMEOUT(b.a->uiText().isEmpty(), 3000);
        QVERIFY(b.a->uiOff());
        b.analyzer.setScan(DeviceLink::Off);
        QTRY_VERIFY_WITH_TIMEOUT(!b.a->uiOff(), 3000);
    }

    void pathChangeIsWaitedFor()
    {
        // the cleanest path above 1.13 GHz: the unit resets its Si4468 after the prompt of the
        // `sweep cw` (180-190 ms on a ZS407); a bare `sweep` is answered once that is done
        Bench b;
        QVERIFY(b.open("sim:ultra:thru", "sim:ultra"));
        SweepConfig c;
        c.grid = {1.1e9, 1.2e9, 11};
        b.analyzer.setConfig(c);
        TrackConfig t{true, -30, -1};
        t.mixer = false;
        b.analyzer.setTracking(t);
        b.analyzer.setScan(DeviceLink::Once);
        QTRY_VERIFY_WITH_TIMEOUT(b.complete(), 10000);
        QByteArrayList g;
        for (const QByteArray &l : b.g->received())  // the sweep's own commands
            if (l.startsWith("sweep") || l.startsWith("output"))
                g << l;
        QVERIFY(g.contains("output normal"));
        auto hz = [](const QByteArray &l) { return l.startsWith("sweep cw ") ? l.mid(9).toLongLong() : 0; };
        // the sweep: from its first point (after the frequency error was measured at 3 GHz)
        g = g.mid(int(std::find_if(g.begin(), g.end(), [&](const QByteArray &l) { return hz(l) > 1.09e9 && hz(l) < 1.2e9; }) - g.begin()));
        const int at = int(std::find_if(g.begin(), g.end(), [&](const QByteArray &l) { return hz(l) > 1130000000; }) - g.begin());
        QVERIFY(at < g.size() - 1);
        QCOMPARE(g[at + 1], QByteArray("sweep"));  // into the leakage path: waited for
        QCOMPARE(g[1], QByteArray("sweep"));       // the first point (from 3 GHz, another path) too
        QCOMPARE(g.count("sweep"), 2);             // not every step
    }

    void offsetForMixers()
    {
        DeviceInfo ultra;
        ultra.model = detectModel("tinySA ULTRA+ ZS407", {});
        ultra.hardware = "ZS407";
        TrackConfig t{true, -30, -1};
        t.offsetHz = 500e6;  // the generator 500 MHz above the analyzer
        SweepConfig c;
        c.grid = {1e6, 9e9, 101};
        QCOMPARE(clampToTracker(c, *ultra.model, ultra, t).grid.stopHz, 7.37e9 - 500e6);
        t.offsetHz = -500e6;  // below: the sweep starts where the generator can
        QCOMPARE(clampToTracker(c, *ultra.model, ultra, t).grid.startHz, 500e6 + 10e3);
        QCOMPARE(trackerFreq(t, 600e6), 100e6);
        t.offsetHz = 1e12;
        QCOMPARE(clampTrack(t, ultra).offsetHz, 7.37e9);

        // a mixer with its LO at 500 MHz between the units: the generator at f + 500 MHz (RF = IF + LO)
        for (const double offset : {500e6, -500e6}) {  // and RF = IF - LO (the sum product)
            Bench b;
            QVERIFY(b.open("sim:ultra:mixer=500", "sim:ultra"));
            SweepConfig s;
            s.grid = offset > 0 ? ScanGrid{10e6, 100e6, 10} : ScanGrid{600e6, 690e6, 10};
            b.analyzer.setConfig(s);
            TrackConfig tc{true, -30, -1};
            tc.offsetHz = offset;
            b.analyzer.setTracking(tc);
            b.analyzer.setScan(DeviceLink::Once);
            QTRY_VERIFY_WITH_TIMEOUT(b.complete(), 10000);
            const SweepFrame f = b.last();
            for (int i = 0; i < f.grid.points; ++i)  // -30 dBm minus the 7 dB conversion loss
                QVERIFY2(std::abs(f.dBm[i] - -37) < 2, qPrintable(QString("%1 MHz: %2 dBm").arg(f.grid.freq(i) / 1e6).arg(f.dBm[i])));
            const QByteArrayList g = b.g->received();
            QVERIFY(g.contains("sweep cw " + QByteArray::number(qint64(s.grid.startHz + offset))));
            QVERIFY(!g.contains("sweep cw 3000000000"));  // its frequency error not measured
            QVERIFY(b.msg.isEmpty() || !b.msg.last().first().toString().contains("No signal"));
        }
    }

    void sweepFollowsTheGenerator()
    {
        Bench b;
        QVERIFY(b.open("sim:ultra:lpf", "sim:ultra"));
        SweepConfig c;
        c.grid = {10e6, 300e6, 30};
        b.analyzer.setConfig(c);
        b.analyzer.setTracking({true, -30, -1});
        b.analyzer.setScan(DeviceLink::Once);
        QTRY_VERIFY_WITH_TIMEOUT(b.complete(), 10000);
        const SweepFrame f = b.last();
        QCOMPARE(f.received, 30);
        for (int i = 0; i < f.grid.points; ++i) {  // the low-pass, ±1.5 dB simulated noise
            const double want = -30 + SimTransport::dutGainDb(SimTransport::Dut::LowPass, f.grid.freq(i));
            QVERIFY2(std::abs(f.dBm[i] - want) < 2, qPrintable(QString("%1 Hz: %2 dBm, want %3")
                                                                   .arg(f.grid.freq(i)).arg(f.dBm[i]).arg(want)));
        }
        QVERIFY(f.dBm[29] < -55);  // 300 MHz: 30 dB down
        // stopped: RF off, the analyzer's screen back; the generator's stays off
        QTRY_VERIFY_WITH_TIMEOUT(!b.g->rfOn() && b.g->uiOff() && !b.a->uiOff(), 3000);
        const QByteArrayList g = b.g->received(), a = b.a->received();
        // its frequency measured first: at 3 GHz the low-pass leaves nothing, then at the top
        QVERIFY(g.indexOf("ui off \"Tracking generator\"") < g.indexOf("sweep cw 3000000000"));  // no redraws
        QVERIFY(g.indexOf("sweep cw 3000000000") < g.indexOf("level -30"));
        QVERIFY(g.indexOf("level -30") < g.indexOf("output on"));
        QVERIFY(g.indexOf("output on") < g.indexOf("sweep cw 300000000"));
        QCOMPARE(g.count("sweep cw 10000000"), 1);
        QVERIFY(g.indexOf("sweep cw 300000000") < g.indexOf("sweep cw 10000000"));
        QVERIFY(g.lastIndexOf("output off") > g.lastIndexOf("sweep cw 300000000"));
        QVERIFY(!g.contains("ui on"));
        // the sweep's RBW again; auto is 30 kHz while tracking (the firmware's misreads single points)
        QVERIFY(a.lastIndexOf("rbw 30") > a.lastIndexOf("rbw 1"));
        QVERIFY(a.lastIndexOf("rbw 30") < a.indexOf("scanraw 10000000 10000000 1"));
        QVERIFY(!g.contains("scanraw"));
        QCOMPARE(b.g->scansWhileGenerating, 0);
        QVERIFY(a.contains("scanraw 10000000 10000000 1"));
        QVERIFY(a.contains("scanraw 300000000 300000000 1"));
        QVERIFY(!a.contains("scanraw 10000000 310000000 30"));  // no ordinary sweep
    }

    void generatorFrequencyErrorIsCorrected()
    {
        // 1500 ppb: 4.5 kHz at 3 GHz, three times the half bandwidth of a 3 kHz RBW
        Bench b;
        QSignalSpy aligned(&b.analyzer, &DeviceLink::trackerAligned);
        QVERIFY(b.open("sim:ultra:thru", "sim:ultra:ppb=1500"));
        SweepConfig c;
        c.grid = {100e6, 3000e6, 30};
        c.rbwKHz = 3;
        b.analyzer.setConfig(c);
        b.analyzer.setTracking({true, -30, -1});
        b.analyzer.setScan(DeviceLink::Once);
        QTRY_VERIFY_WITH_TIMEOUT(b.complete(), 15000);
        QCOMPARE(aligned.size(), 1);
        QVERIFY(aligned.first().at(1).toBool());
        const double ppb = aligned.first().first().toDouble();
        QVERIFY2(std::abs(ppb - 1500) < 20, qPrintable(QString("measured %1 ppb").arg(ppb)));
        for (double v : b.last().dBm)
            QVERIFY2(std::abs(v - -30) < 0.5, qPrintable(QString("%1 dBm").arg(v)));
        const QByteArrayList g = b.g->received(), a = b.a->received();
        QVERIFY(g.contains("sweep cw " + QByteArray::number(std::llround(3e9 / (1 + ppb * 1e-9)))));  // ~2999995500
        QVERIFY(a.contains("rbw 1"));                 // the fine scan
        QVERIFY(a.lastIndexOf("rbw 3") > a.lastIndexOf("rbw 1"));  // the sweep's RBW again
        // measured once per connection of the generator unit
        b.analyzer.setScan(DeviceLink::Once);
        QTRY_VERIFY_WITH_TIMEOUT(b.frames.size() > 1 && b.complete() && b.scan.last().first().toInt() == DeviceLink::Off, 15000);
        QCOMPARE(aligned.size(), 1);
        QCOMPARE(b.a->received().count("rbw 1"), 1);
    }

    void noSignalLeavesTheFrequencyUncorrected()
    {
        Bench b;
        QSignalSpy aligned(&b.analyzer, &DeviceLink::trackerAligned);
        QVERIFY(b.open("sim:ultra", "sim:ultra:ppb=1500"));  // the input on the antenna
        SweepConfig c;
        c.grid = {50e6, 150e6, 11};
        b.analyzer.setConfig(c);
        b.analyzer.setTracking({true, -30, -1});
        b.analyzer.setScan(DeviceLink::Once);
        QTRY_VERIFY_WITH_TIMEOUT(b.complete(), 15000);
        QCOMPARE(aligned.size(), 1);
        QVERIFY(!aligned.first().at(1).toBool());
        QVERIFY(b.msg.size() >= 1);
        QCOMPARE(b.msg.first().first().toString(),
                 QString("No signal from the tracking generator to measure its frequency: not corrected"));
        QVERIFY(b.g->received().contains("sweep cw 150000000"));  // tried at the top too
    }

    void continuousSweepsResendTheLevel()
    {
        Bench b;
        QVERIFY(b.open("sim:ultra:thru", "sim:basic"));
        SweepConfig c;
        c.grid = {50e6, 150e6, 11};
        b.analyzer.setConfig(c);
        b.analyzer.setTracking({true, -40, 0});
        b.analyzer.setScan(DeviceLink::Continuous);
        QTRY_VERIFY_WITH_TIMEOUT(std::count_if(b.frames.begin(), b.frames.end(), [](const QList<QVariant> &v) {
                                     return v.first().value<SweepFrame>().complete; }) >= 3, 10000);
        b.analyzer.setScan(DeviceLink::Off);
        QTRY_VERIFY_WITH_TIMEOUT(!b.g->rfOn(), 3000);
        const SweepFrame f = b.last();
        for (double v : f.dBm)
            QVERIFY(std::abs(v - -40) < 2);
        QVERIFY(b.g->received().count("level -40") >= 3);  // once per sweep (at its first point)
        const QByteArrayList g = b.g->received();  // the basic has no `ui`
        QVERIFY(std::none_of(g.begin(), g.end(), [](const QByteArray &l) { return l.startsWith("ui "); }));
    }

    void levelChangeAppliesDuringTheSweep()
    {
        Bench b;
        QVERIFY(b.open("sim:ultra:thru", "sim:ultra"));
        SweepConfig c;
        c.grid = {50e6, 150e6, 11};
        b.analyzer.setConfig(c);
        b.analyzer.setTracking({true, -40, -1});
        b.analyzer.setScan(DeviceLink::Continuous);
        QTRY_VERIFY_WITH_TIMEOUT(b.complete(), 5000);
        b.analyzer.setTracking({true, -25, -1});
        QTRY_VERIFY_WITH_TIMEOUT(b.complete() && std::abs(b.last().dBm[0] - -25) < 2
                                     && std::abs(b.last().dBm[10] - -25) < 2, 5000);
        b.analyzer.setScan(DeviceLink::Off);
    }

    void touchOnTheGeneratorStopsTheScan()
    {
        Bench b;
        QVERIFY(b.open("sim:ultra:thru", "sim:ultra"));
        b.analyzer.setTracking({true, -30, -1});
        b.analyzer.setScan(DeviceLink::Continuous);
        QTRY_VERIFY_WITH_TIMEOUT(b.g->uiOff() && b.g->rfOn(), 5000);
        b.g->touchScreen();  // "ui on" from the generator unit
        QTRY_COMPARE_WITH_TIMEOUT(b.scan.last().first().toInt(), int(DeviceLink::Off), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!b.g->rfOn(), 3000);
        QVERIFY(b.msg.size() >= 1);
        QCOMPARE(b.msg.last().first().toString(), QString("Scan stopped on the tracking generator"));
        QTest::qWait(200);
        QVERIFY(!b.g->uiOff());  // the user's: not switched off again
        b.analyzer.setScan(DeviceLink::Continuous);  // until the next scan
        QTRY_VERIFY_WITH_TIMEOUT(b.g->uiOff() && b.g->rfOn(), 5000);
        b.analyzer.setScan(DeviceLink::Off);
    }

    void losingTheGeneratorStopsTheScan()
    {
        Bench b;
        QVERIFY(b.open("sim:ultra:thru", "sim:ultra"));
        b.analyzer.setTracking({true, -30, -1});
        b.analyzer.setScan(DeviceLink::Continuous);
        QTRY_VERIFY_WITH_TIMEOUT(b.g->rfOn(), 5000);
        QPointer<SimTransport> g = b.g;
        b.tg.close();  // its goodbye: RF off, screen on
        QVERIFY(g);
        QVERIFY(!g->rfOn());
        QVERIFY(!g->uiOff());
        QTRY_COMPARE_WITH_TIMEOUT(b.scan.last().first().toInt(), int(DeviceLink::Off), 3000);
        QCOMPARE(b.msg.last().first().toString(), QString("Tracking generator lost: scan stopped"));
    }

    void withoutGeneratorUnitTheSweepIsOrdinary()
    {
        Bench b;
        QVERIFY(b.open("sim:ultra", {}));
        SweepConfig c;
        c.grid = {50e6, 150e6, 101};
        b.analyzer.setConfig(c);
        b.analyzer.setTracking({true, -30, -1});
        b.analyzer.setScan(DeviceLink::Once);
        QTRY_VERIFY_WITH_TIMEOUT(b.complete(), 5000);
        QVERIFY(b.a->received().contains("scanraw 50000000 151000000 101"));
        QVERIFY(std::abs(b.last().dBm[50] - -30) < 2);  // the antenna's 100 MHz carrier
        QVERIFY(!b.a->received().contains("rbw 30"));    // auto stays auto
    }

    void rangeAndLevelInsideTheGenerator()
    {
        DeviceInfo basic, ultra;
        basic.model = detectModel("tinySA v0.3ZS304", {});
        ultra.model = detectModel("tinySA ULTRA+ ZS407", {});
        ultra.hardware = "ZS407";
        QVERIFY(basic.model && ultra.model);
        SweepConfig c;
        c.grid = {1e6, 800e6, 101};
        TrackConfig t{true, -10, -1};
        const SweepConfig clamped = clampToTracker(c, *ultra.model, basic, t);  // basic LOW output
        QCOMPARE(clamped.grid.startHz, std::max(1e6, basic.model->gen.minHz));
        QCOMPARE(clamped.grid.stopHz, basic.model->gen.maxHz);
        QCOMPARE(clampToTracker(c, *ultra.model, DeviceInfo{}, t).grid.stopHz, 800e6);  // none connected
        QCOMPARE(clampTrack(t, ultra).levelDbm, ultra.model->gen.maxDbm);
        t.levelDbm = -200;
        QCOMPARE(clampTrack(t, basic).levelDbm, basic.model->gen.minDbm);
        QCOMPARE(trackSettleMs(t, false), 150);  // its screen redraws before the frequency changes
        QCOMPARE(trackSettleMs(t, false, TrackChange::RfOn), 300);
        QCOMPARE(trackSettleMs(t, true), 10);
        QCOMPARE(trackSettleMs(t, true, TrackChange::Path), 10);  // the path change waited for (sync)
        QCOMPARE(trackSettleMs(t, true, TrackChange::RfOn), 10);
        QCOMPARE(trackerPath(ultra, t, 800e6), 0);
        QCOMPARE(trackerPath(ultra, t, 900e6), 1);
        QCOMPARE(trackerPath(ultra, t, 1.2e9), 2);
        QCOMPARE(trackerPath(basic, t, 300e6), 0);
        t.settleMs = 20;
        QCOMPARE(trackSettleMs(t, true), 20);
        QCOMPARE(trackSettleMs(t, true, TrackChange::Path), 20);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int fails = 0;
    // each class logs to stdout and to tst_<class>.log in the working directory
    auto run = [&](QObject *t) {
        QStringList args = app.arguments();
        args << "-o" << QString("tst_%1.log,txt").arg(t->metaObject()->className()) << "-o" << "-,txt";
        fails += QTest::qExec(t, args);
    };
    TstUnits units;
    run(&units);
    TstProtocol protocol;
    run(&protocol);
    TstModel model;
    run(&model);
    TstLink link;
    run(&link);
    TstGenerator generator;
    run(&generator);
    TstTracking tracking;
    run(&tracking);
    TstTraces traces;
    run(&traces);
    TstCsv csv;
    run(&csv);
    TstMarkers markers;
    run(&markers);
    TstDfu dfu;
    run(&dfu);
    return fails;
}

#include "tst_core.moc"
