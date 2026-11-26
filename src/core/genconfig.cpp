#include "genconfig.h"
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

// The Ultra's output paths (sa_core.c update_min_max_freq, the unit's OUTPUT menu): the mixer
// path ("highest accuracy") reaches the LO maximum plus the first IF, the direct PLL path
// ("cleanest signal") the LO maximum. Above that the unit uses the LO's harmonic (3 after a
// mode switch): mixer up to 3 x LO maximum - DEFAULT_IF (977.4 MHz on every Ultra), direct up
// to 3 x LO maximum.
static double ultraMaxHz(const QString &hw, bool mixer, bool harmonic)
{
    static const QRegularExpression zs(R"(ZS(\d{3}))");
    const auto m = zs.match(hw);
    const int id = m.hasMatch() ? m.captured(1).toInt() : 0;
    const bool max2871 = id >= 407 || hw.contains("max2871", Qt::CaseInsensitive);  // else ADF4351
    if (harmonic) {
        const double lo = max2871 ? 6.3e9 : 4.35e9;  // MAX_LO_FREQ (main.c set_freq_boundaries)
        return mixer ? 3 * lo - 977.4e6 : 3 * lo;
    }
    if (max2871)
        return mixer ? 7.37e9 : 6.3e9;
    if (!mixer)
        return 4.35e9;
    return id == 406 || hw.contains("V0.4.6") ? 5.42e9 : 5.327e9;
}

double ultraFundamentalMaxHz(const DeviceInfo &d, bool mixer)
{
    return ultraMaxHz(d.hardware, mixer, false);
}

QString ultraOutputKey(bool mixer, bool harmonic)
{
    return QString(harmonic ? "ultra " : "") + (mixer ? "mixer" : "normal");
}

QVector<UltraOutput> ultraOutputs(const DeviceInfo &d)
{
    if (!d.model || !d.model->gen.hasMixer)
        return {};
    QVector<UltraOutput> out;
    for (const bool harmonic : {false, true})
        for (const bool mixer : {true, false}) {
            const QString name = harmonic ? (mixer ? "Ultra accurate" : "Ultra cleanest") : (mixer ? "Accurate" : "Cleanest");
            out.append(UltraOutput{mixer, harmonic, ultraOutputKey(mixer, harmonic),
                        name + ", max " + formatHz(ultraMaxHz(d.hardware, mixer, harmonic), 1e7)});
        }
    return out;
}

GenLimits genLimits(const GenConfig &c, const DeviceInfo &d)
{
    GenLimits l{0, 100e9, -200, 50};  // not connected: anything, clamped on connect
    if (const ModelDesc *m = d.model) {
        const GenDesc &g = m->gen;
        if (c.path == GenPath::High && g.highMaxHz > 0)
            l = {g.highMinHz, g.highMaxHz, g.highMinDbm, g.highMaxDbm};
        else
            l = {g.minHz, g.hasMixer ? ultraMaxHz(d.hardware, c.mixer, c.harmonic) : g.maxHz, g.minDbm, g.maxDbm};
    }
    l.minDbm += c.extGainDb;
    l.maxDbm += c.extGainDb;
    return l;
}

static bool isFm(GenType t) { return t == GenType::FM || t == GenType::NFM || t == GenType::WFM; }

GenConfig clampGen(GenConfig c, const DeviceInfo &d)
{
    c.extGainDb = std::clamp(std::round(c.extGainDb * 10) / 10, -100.0, 100.0);  // as clampConfig
    c.sweepS = std::clamp(c.sweepS, 0.0, 600.0);
    c.modHz = std::clamp(c.modHz, 1, d.model ? d.model->gen.maxModHz : 10000);
    c.amDepth = std::clamp(c.amDepth, 0, 100);
    c.fmDevHz = std::clamp(c.fmDevHz, 1000, 300000);
    c.levelChangeDb = std::clamp(c.levelChangeDb, -90.0, 90.0);
    c.burstPeriodS = std::clamp(c.burstPeriodS, 2e-6, 89.0);  // as set_burst_period / _on_time
    c.burstOnS = std::clamp(c.burstOnS, 1e-6, c.burstPeriodS - 1e-6);
    c.burstCount = std::clamp(c.burstCount, 0, 65535);
    if (const ModelDesc *m = d.model) {
        const GenDesc &g = m->gen;
        if (g.highMaxHz <= 0)
            c.path = GenPath::Low;
        if (!g.hasMixer)
            c.harmonic = false;
        const QVector<GenType> types = genTypes(d);
        if (c.type && !types.contains(*c.type)) {  // a type of the other model or firmware: nearest one
            const auto fm = std::find_if(types.begin(), types.end(), isFm);
            c.type = isFm(*c.type) && fm != types.end() ? *fm : GenType::CW;
        }
        if (c.path == GenPath::High) {  // the HIGH output has no AM, external modulation or level sweep
            if (c.type == GenType::AM || c.type == GenType::External)
                c.type = GenType::CW;
            c.levelChangeDb = 0;
        }
        c.levelChangeDb = std::clamp(c.levelChangeDb, -g.maxLevelChange, g.maxLevelChange);
        if (!d.ctcssTone)  // firmware without `modulation ctcss`
            c.ctcss.reset();
    }
    const GenLimits l = genLimits(c, d);
    c.levelDbm = std::clamp(std::round(c.levelDbm * 10) / 10, l.minDbm, l.maxDbm);
    c.spanHz = std::clamp(std::round(c.spanHz), 0.0, l.maxHz - l.minHz);
    c.freqHz = std::clamp(std::round(c.freqHz), l.minHz, l.maxHz);
    if (c.type == GenType::Sweep)  // the whole sweep inside the limits
        c.freqHz = std::clamp(c.freqHz, l.minHz + c.spanHz / 2, l.maxHz - c.spanHz / 2);
    return c;
}

static QByteArray num(double v) { return QByteArray::number(v, 'g', 10); }
static QByteArray hz(qint64 v) { return QByteArray::number(v); }
static QByteArray seconds(double s)  // the firmware's my_atof takes m and u, not exponents
{
    return s < 1e-3 ? QByteArray::number(s * 1e6, 'g', 7) + "u"
           : s < 1  ? QByteArray::number(s * 1e3, 'g', 7) + "m"
                    : QByteArray::number(s, 'g', 7);
}

static QByteArray modulationName(std::optional<GenType> t)
{
    switch (t.value_or(GenType::CW)) {
    case GenType::AM: return "am";
    case GenType::FM: return "fm";
    case GenType::NFM: return "nfm";
    case GenType::WFM: return "wfm";
    case GenType::External: return "extern";
    case GenType::Burst: return "burst";
    default: return "off";
    }
}

QVector<GenType> genTypes(const DeviceInfo &d)
{
    QVector<GenType> t = d.model ? d.model->gen.types : QVector<GenType>{};
    if (d.hasBurst)
        t << GenType::Burst;
    return t;
}

bool burstPossible(const GenConfig &c)
{
    return c.mixer || c.freqHz <= 1130e6;
}

std::optional<CtcssTone> ctcssTone(const GenConfig &c)
{
    if (c.type != GenType::FM || c.ctcss != true || c.modHz < 1 || c.modHz >= kCtcssMaxHz)
        return std::nullopt;
    // as sa_core.c: hn = (int)(1000.0 / f + 0.5); deviation in steps of 100 Hz, the rate's own /7
    const int hn = int(1000.0 / c.modHz + 0.5);
    const int div100 = c.fmDevHz / 100;
    return CtcssTone{hn * c.modHz, div100 * 100, div100 / 7 * 100};
}

AppliedGen keepUnitConfig(const AppliedGen &h)
{
    AppliedGen k;
    k.ctcss = h.ctcss;
    return k;
}

QByteArrayList genCommands(const GenConfig &w, AppliedGen &h, const ModelDesc &m)
{
    const GenDesc &g = m.gen;
    QByteArrayList cmds;
    const GenPath path = g.highMaxHz > 0 ? w.path : GenPath::Low;
    if (h.mode != path) {
        cmds << (path == GenPath::High ? "mode high output" : "mode low output");
        // a mode switch resets the unit (sa_core.c reset_settings): output off, no modulation
        // (1 kHz, 80 %, 3 kHz; burst 1 ms, 100 us, continuous), no level change, not sweeping,
        // mixer output; level at maximum. Its config (the CTCSS tone) stays.
        h = keepUnitConfig(h);
        h.mode = path;
        h.on = false;
        h.modulation = "off";
        h.modHz = 1000;
        h.amDepth = 80;
        h.fmDevHz = 3000;
        h.burstPeriodS = 1e-3;
        h.burstOnS = 100e-6;
        h.burstCount = 0;
        h.levelChangeDb = 0;
        h.sweeping = false;
        if (g.hasMixer)
            h.mixer = true;
    }
    const bool known = w.type.has_value(), sweep = w.type == GenType::Sweep;
    if (g.needsSweepGo && known && !sweep && h.sweeping != false) {  // stop before the span goes
        cmds << "sweep abort";
        h.sweeping = false;
    }
    const QByteArray mod = modulationName(w.type);
    if (known && mod == "off" && h.modulation != mod) {  // before a span: the basic sweeps at once
        cmds << "modulation off";
        h.modulation = mod;
    }
    // the path only matters above 1.13 GHz (below, both make the same signal): unknown after
    // taking a unit over, it is set once the frequencies get there
    const bool pathMatters = w.freqHz + (sweep ? w.spanHz / 2 : 0) > 1130e6;
    if (g.hasMixer && h.mixer != w.mixer && (h.mixer || pathMatters)) {
        cmds << (w.mixer ? "output mixer" : "output normal");
        h.mixer = w.mixer;
    }
    if (h.extGainDb != w.extGainDb) {
        cmds << "ext_gain " + num(w.extGainDb);
        h.extGainDb = w.extGainDb;
    }
    // centre then span: `sweep center` keeps the centre (shrinking the old span to fit),
    // `sweep span` keeps it too (main.c set_sweep_frequency)
    const std::pair<qint64, qint64> freq{std::llround(w.freqHz), sweep ? std::llround(w.spanHz) : 0};
    if (h.freq != freq) {
        if (freq.second == 0)
            cmds << "sweep cw " + hz(freq.first);
        else
            cmds << "sweep center " + hz(freq.first) << "sweep span " + hz(freq.second);
        h.freq = freq;
        h.levelDbm.reset();  // the Ultra lowers the level for good where it cannot reach it
        h.sweepS.reset();    // and a span change to zero resets the sweep time
    }
    const double port = w.levelDbm - w.extGainDb;  // `level` is the absolute level at the port
    if (w.levelKnown && h.levelDbm != port) {
        cmds << "level " + num(port);
        h.levelDbm = port;
    }
    // the unit's CTCSS tone (its config, any signal type): before `modulation fm`, which then
    // starts with the right table
    if (w.ctcss && h.ctcss != w.ctcss) {
        cmds << (*w.ctcss ? "modulation ctcss on" : "modulation ctcss off");
        h.ctcss = w.ctcss;
    }
    if (known) {
        const double change = sweep && path == GenPath::Low ? w.levelChangeDb : 0;
        if (h.levelChangeDb != change) {
            cmds << "levelchange " + num(change);
            h.levelChangeDb = change;
        }
        if (sweep && h.sweepS != w.sweepS) {
            cmds << "sweeptime " + num(w.sweepS);
            h.sweepS = w.sweepS;
        }
        if (mod == "burst") {  // parameters first: the bursts start right (period before ON time)
            if (h.burstPeriodS != w.burstPeriodS) {
                cmds << "modulation period " + seconds(w.burstPeriodS);
                h.burstPeriodS = w.burstPeriodS;
            }
            if (h.burstOnS != w.burstOnS) {
                cmds << "modulation ontime " + seconds(w.burstOnS);
                h.burstOnS = w.burstOnS;
            }
            if (h.burstCount != w.burstCount) {  // counted bursts wait for `modulation trigger`
                cmds << "modulation count " + QByteArray::number(w.burstCount);
                h.burstCount = w.burstCount;
            }
        } else if (mod != "off" && mod != "extern") {  // parameters first: the modulation starts right
            if (h.modHz != w.modHz) {
                cmds << "modulation freq " + QByteArray::number(w.modHz);
                h.modHz = w.modHz;
            }
            if (g.adjustableModulation && mod == "am" && h.amDepth != w.amDepth) {
                cmds << "modulation depth " + QByteArray::number(w.amDepth);
                h.amDepth = w.amDepth;
            }
            if (g.adjustableModulation && mod == "fm" && h.fmDevHz != w.fmDevHz) {
                cmds << "modulation deviation " + QByteArray::number(w.fmDevHz);
                h.fmDevHz = w.fmDevHz;
            }
        }
        if (h.modulation != mod) {
            cmds << "modulation " + mod;
            h.modulation = mod;
        }
        if (g.needsSweepGo && sweep && h.sweeping != true) {
            cmds << "sweep go";
            h.sweeping = true;
        }
    }
    if (w.on && h.on != w.on) {  // last: everything else is in place when the signal appears
        cmds << (*w.on ? "output on" : "output off");
        h.on = w.on;
    }
    return cmds;
}

void forgetGenCommand(const QByteArray &cmd, AppliedGen &h)
{
    const QByteArrayList t = cmd.split(' ');
    const QByteArray a = t.value(0), b = t.value(1);
    if (a == "mode") h.mode.reset();  // sent again: no reset if it did arrive (same mode)
    else if (a == "output") (b == "on" || b == "off" ? h.on : h.mixer).reset();
    else if (a == "ext_gain") h.extGainDb.reset();
    else if (a == "level") h.levelDbm.reset();
    else if (a == "levelchange") h.levelChangeDb.reset();
    else if (a == "sweeptime") h.sweepS.reset();
    else if (a == "sweep" && (b == "go" || b == "abort")) h.sweeping.reset();
    else if (a == "sweep") h.freq.reset();
    else if (a == "modulation" && b == "freq") h.modHz.reset();
    else if (a == "modulation" && b == "depth") h.amDepth.reset();
    else if (a == "modulation" && b == "deviation") h.fmDevHz.reset();
    else if (a == "modulation" && b == "period") h.burstPeriodS.reset();
    else if (a == "modulation" && b == "ontime") h.burstOnS.reset();
    else if (a == "modulation" && b == "count") h.burstCount.reset();
    else if (a == "modulation" && b == "ctcss") h.ctcss.reset();
    else if (a == "modulation" && b != "trigger") h.modulation.reset();  // a trigger is no setting
}

GenConfig adoptUnitGenerator(GenConfig c, const DeviceInfo &d, AppliedGen *have)
{
    const UnitGenerator &u = d.gen;
    c.path = u.high ? GenPath::High : GenPath::Low;
    c.mixer = true;  // cannot be read: the unit's default after a mode switch
    c.ctcss = d.ctcssTone;  // read at connect (or unknown)
    const ScanGrid &grid = d.deviceGrid;
    const double span = grid.points > 0 ? std::max(0.0, grid.stopHz - grid.startHz) : 0;
    const double unitHz = grid.points > 0 ? (grid.startHz + grid.stopHz) / 2 : c.freqHz;
    c.freqHz = unitHz;
    // above the mixer path's maximum: the LO's harmonic, an Ultra output mode
    c.harmonic = d.model && d.model->gen.hasMixer && unitHz + span / 2 > ultraFundamentalMaxHz(d, true);
    if (u.extGainDb)
        c.extGainDb = *u.extGainDb;
    c.levelKnown = false;  // `attenuate` reports the attenuator, several dB off the level
    c.on.reset();
    if (span > 0) {  // the unit's menus never combine a span with modulation
        c.type = GenType::Sweep;
        c.spanHz = span;
        c.levelChangeDb = 0;  // cannot be read: assumed
        if (u.sweepS)
            c.sweepS = *u.sweepS;
    } else {
        c.type.reset();
    }
    c = clampGen(c, d);
    AppliedGen h;
    if (d.model)
        genCommands(c, h, *d.model);  // the unit as far as it reported (or is assumed)
    // what the unit really has where the app had to assume or clamp: sent when it matters
    if (grid.points > 0)  // outside the app's limits (e.g. an LO harmonic): moved inside at once
        h.freq = std::pair<qint64, qint64>{std::llround(unitHz), c.type == GenType::Sweep ? std::llround(span) : 0};
    h.on.reset();
    h.mixer.reset();  // sent once the frequency is in the range where the path matters
    h.modHz.reset();  // sent with the first modulation chosen
    h.amDepth.reset();
    h.fmDevHz.reset();
    h.burstPeriodS.reset();
    h.burstOnS.reset();
    h.burstCount.reset();
    if (!u.extGainDb)  // not read: ours goes out
        h.extGainDb.reset();
    if (!u.sweepS)
        h.sweepS.reset();
    if (!c.type) {
        h.modulation.reset();
        h.levelChangeDb.reset();
        h.sweeping.reset();
    }
    if (have)
        *have = h;
    return c;
}

QString outputPathText(const GenConfig &c, const DeviceInfo &d)
{
    if (!d.model)
        return {};
    if (!d.model->gen.hasMixer)
        return c.path == GenPath::High ? "Level in coarse steps" : "";
    // as the unit's OUTPUT line (ui.c menu_lowoutput_settings_acb) over the frequencies used
    const double span = c.type == GenType::Sweep ? c.spanHz : 0;
    const double start = c.freqHz - span / 2, stop = c.freqHz + span / 2;
    QStringList parts;
    if (start < 823e6)
        parts << "Sine";
    if (stop >= 823e6)
        parts << "Square wave";
    if (stop > 1130e6 && c.mixer)
        parts << "Mixer";
    if (stop > ultraFundamentalMaxHz(d, c.mixer))  // the unit's HARM
        parts << "LO harmonic";
    return parts.join(", ");
}

static const char *const kTypeNames[] = {"CW", "Sweep", "AM", "FM", "NFM", "WFM", "Ext", "Burst"};

QString genTypeName(GenType t)
{
    return kTypeNames[int(t)];
}

std::optional<GenType> genTypeFromName(const QString &name)
{
    for (int i = 0; i < int(std::size(kTypeNames)); ++i)
        if (name.compare(kTypeNames[i], Qt::CaseInsensitive) == 0)
            return GenType(i);
    return std::nullopt;
}
