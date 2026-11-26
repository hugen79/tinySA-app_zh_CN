// DeviceLink, signal generator: switching between analyzer and generator, the GenConfig
// diff, and following a unit that is already generating when it connects.
#include "link.h"
#include <QRegularExpression>
#include <cmath>

void DeviceLink::setGenerator(const GenConfig &g)
{
    m_gen = m_info.model ? clampGen(g, m_info) : g;
    pump();
}

void DeviceLink::setActivity(int activity)
{
    if (!m_ready || activity == m_activity)
        return;
    if (activity == Generator) {
        m_modeBeforeGen = m_mode;
        setMode(Off);  // abort + resume: a paused unit does not apply output changes
        m_activity = Generator;
        m_genHave = keepUnitConfig(m_genHave);  // unknown: the diff starts with the mode switch
    } else {
        m_activity = Analyzer;
        // generator commands not sent yet are obsolete: no `output on` just before the switch
        m_queue.removeIf([](const Cmd &c) { return c.generator; });
        const InputMode input = m_info.model->hasInputMode ? m_want.input : InputMode::Low;
        // at once, so the unit shows its own spectrum again; it resets the unit's settings
        queueChecked({input == InputMode::High ? "mode high input" : "mode low input"});
        m_have = AppliedConfig{input};
        m_genHave = keepUnitConfig(m_genHave);
    }
    emit activityChanged(m_activity);
    if (activity == Analyzer)
        setMode(m_modeBeforeGen);
    pump();
}

void DeviceLink::triggerBurst()
{
    // the settings not sent yet go first (the diff is only made when the queue is empty)
    if (m_ready && m_activity == Generator && m_gen.type == GenType::Burst)
        queueChecked(genCommands(m_gen, m_genHave, *m_info.model) + QByteArrayList{"modulation trigger"}, true);
}

void DeviceLink::pumpGenerator()
{
    if (m_paused)  // output on/off and frequency changes only take effect in the unit's sweep loop
        return enqueue({"resume"});
    // tracking generator: its screen off before anything else (no command waits for a redraw),
    // on again when touched or no longer wanted
    if (trackScreenOff() && (!m_uiOff || uiTextDue()))
        return enqueue(uiCommand(true));
    const QByteArrayList cmds = genCommands(m_gen, m_genHave, *m_info.model);
    if (!cmds.isEmpty())
        return queueChecked(cmds, true);
    if (!trackScreenOff() && m_uiOff)
        enqueue(uiCommand(false));
}

// The text under the "Remote only mode" message (firmware from v1.4-317): which tinySA is
// which when two work together
QByteArray DeviceLink::uiOffText() const
{
    if (m_isTracker)
        return "Tracking generator";
    return m_tg && m_tg->m_ready ? "Spectrum analyzer" : "";  // "": an earlier text goes
}

bool DeviceLink::uiTextDue() const
{
    return m_uiOff && m_uiTextOk && m_uiText != uiOffText();
}

DeviceLink::Cmd DeviceLink::uiCommand(bool off)
{
    if (!off)
        return {"ui on"};
    if (!m_uiTextOk)
        return {"ui off"};
    Cmd c{"ui off \"" + uiOffText() + "\""};
    c.done = [this](bool, const QStringList &lines) {
        if (!lines.join(' ').contains("usage"))
            return;
        m_uiTextOk = false;  // firmware before v1.4-317 (ui off|on): its screen stayed on
        m_uiOff = false;
        pump();              // plain ui off
    };
    return c;
}

// "usage: sweeptime 0.003..60\r\n 2.003s" -> 2.003; the unit prints SI prefixes (500m).
static std::optional<double> lastValue(const QStringList &lines)
{
    static const QRegularExpression re(R"(^\s*([-+]?\d+(?:\.\d+)?)\s*([pnumkMG]?)s?\s*$)");
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const auto m = re.match(*it);
        if (!m.hasMatch())
            continue;
        static const QString prefixes = "pnum kMG";
        const int p = m.captured(2).isEmpty() ? 4 : int(prefixes.indexOf(m.captured(2)));
        return m.captured(1).toDouble() * std::pow(10.0, 3 * (p - 4));
    }
    return std::nullopt;
}

// Handshake: `ext_gain` alone prints "usage: ext_gain -100.0..+100.0" and the value, in either
// mode (one value for analyzer and generator, kept in the unit's backup memory)
DeviceLink::Cmd DeviceLink::extGainRead()
{
    return {"ext_gain", {}, 0, 3000, [this](bool ok, const QStringList &lines) {
        if (ok && !lines.join(' ').contains('?'))
            m_info.extGainDb = lastValue(lines);
    }};
}

void DeviceLink::probeGenerator(const QStringList &levelReply)
{
    const QString usage = levelReply.join(' ');
    if (!usage.contains("usage: level"))
        return;  // input mode: `level` printed nothing
    m_info.gen.active = true;
    m_info.gen.high = usage.contains("-38");  // basic HIGH output: "usage: level -38..13"
    m_info.gen.extGainDb = m_info.extGainDb;  // read just before
    // read what else the unit can tell, before the rest of the handshake (not the level:
    // `attenuate` reports the attenuator, on a ZS407 5 dB below the level)
    m_queue.prepend(Cmd{"sweeptime", {}, 0, 3000, [this](bool ok, const QStringList &lines) {
        if (ok)
            m_info.gen.sweepS = lastValue(lines);
    }});
}

void DeviceLink::adoptGenerator()
{
    if (m_info.gen.active) {
        setMode(Off);  // a scan asked for during the handshake does not happen
        m_gen = adoptUnitGenerator(m_gen, m_info, &m_genHave);
        m_activity = Generator;
        m_modeBeforeGen = Off;
        const double center = (m_info.deviceGrid.startHz + m_info.deviceGrid.stopHz) / 2;
        if (m_info.deviceGrid.points > 0 && std::abs(m_gen.freqHz - center) >= 1)  // sent at once
            emit message(QString("The tinySA generated at %1, outside the app's range: moved to %2")
                             .arg(formatHz(center, 1e3), formatHz(m_gen.freqHz, 1e3)));
        emit generatorAdopted(m_gen);
    }
    emit activityChanged(m_activity);
}
