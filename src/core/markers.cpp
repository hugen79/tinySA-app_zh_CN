#include "markers.h"
#include <cmath>

// Least-squares parabola through the points within 6 dB of the extreme (at least the
// two neighbours, at most 8 each side). Using more than 3 points makes the vertex
// robust against the unit's 0.5 dB level steps and its slightly lopsided RBW filter.
QPointF interpolatePeak(const Trace &t, int k)
{
    const int n = t.dBm.size();
    const double f = t.grid.freq(k), top = t.dBm[k];
    auto usable = [&](int i) { return i >= 0 && i < n && !std::isnan(t.dBm[i]); };
    if (!usable(k - 1) || !usable(k + 1))
        return {f, top};
    int lo = k - 1, hi = k + 1;
    while (usable(lo - 1) && k - lo < 8 && std::abs(t.dBm[lo - 1] - top) <= 6)
        --lo;
    while (usable(hi + 1) && hi - k < 8 && std::abs(t.dBm[hi + 1] - top) <= 6)
        ++hi;
    double s[5] = {}, sy[3] = {};  // sums of x^0..x^4 and y*x^0..x^2, x = i - k
    for (int i = lo; i <= hi; ++i) {
        const double x = i - k, y = t.dBm[i];
        double p = 1;
        for (int e = 0; e < 5; ++e, p *= x)
            s[e] += p;
        sy[0] += y, sy[1] += y * x, sy[2] += y * x * x;
    }
    // normal equations for y = a + b x + c x^2 (Cramer's rule)
    auto det3 = [](double m00, double m01, double m02, double m10, double m11, double m12, double m20,
                   double m21, double m22) {
        return m00 * (m11 * m22 - m12 * m21) - m01 * (m10 * m22 - m12 * m20) + m02 * (m10 * m21 - m11 * m20);
    };
    const double D = det3(s[0], s[1], s[2], s[1], s[2], s[3], s[2], s[3], s[4]);
    if (std::abs(D) < 1e-12)
        return {f, top};
    const double a = det3(sy[0], s[1], s[2], sy[1], s[2], s[3], sy[2], s[3], s[4]) / D;
    const double b = det3(s[0], sy[0], s[2], s[1], sy[1], s[3], s[2], sy[2], s[4]) / D;
    const double c = det3(s[0], s[1], sy[0], s[1], s[2], sy[1], s[2], s[3], sy[2]) / D;
    if (std::abs(c) < 1e-12)
        return {f, top};
    const double d = std::clamp(-b / (2 * c), -1.0, 1.0);  // vertex offset in points
    return {t.grid.freqAt(k + d), a + b * d + c * d * d};
}

QPointF tracePeak(const Trace &t, bool max)
{
    int best = -1;
    for (int i = 0; i < t.dBm.size(); ++i)
        if (!std::isnan(t.dBm[i]) && (best < 0 || (max ? t.dBm[i] > t.dBm[best] : t.dBm[i] < t.dBm[best])))
            best = i;
    return best < 0 ? QPointF(qQNaN(), qQNaN()) : interpolatePeak(t, best);
}

int MarkerSet::add(double hz)
{
    m_list.append(Marker{hz, false});
    return m_selected = int(m_list.size()) - 1;
}

void MarkerSet::remove(int i)
{
    if (i < 0 || i >= count())
        return;
    m_list.removeAt(i);
    if (m_selected == i)
        m_selected = -1;
    else if (m_selected > i)
        --m_selected;  // the same marker, one place up
}

void MarkerSet::clear()
{
    m_list.clear();
    m_selected = -1;
}

void MarkerSet::move(int i, double hz)
{
    if (i >= 0 && i < count())
        m_list[i].hz = hz;
}

void MarkerSet::setDelta(int i, bool on)
{
    if (i >= 0 && i < count())
        m_list[i].delta = on;
}

void MarkerSet::select(int i)
{
    m_selected = i >= 0 && i < count() ? i : -1;
}

int MarkerSet::reference() const
{
    for (int i = 0; i < count(); ++i)
        if (!m_list[i].delta)
            return i;
    return -1;
}

int MarkerSet::near(double hz, double tolHz) const
{
    int best = -1;
    for (int i = 0; i < count(); ++i)
        if (std::abs(m_list[i].hz - hz) <= tolHz
            && (best < 0 || std::abs(m_list[i].hz - hz) < std::abs(m_list[best].hz - hz)))
            best = i;
    return best;
}

MarkerReading MarkerSet::read(int i, const Trace &t) const
{
    MarkerReading r;
    const Marker &m = m_list[i];
    r.name = (m.delta ? "D" : "") + QString::number(i + 1);
    r.hz = m.hz;
    if (!t.isEmpty() && m.hz >= t.grid.startHz && m.hz <= t.grid.stopHz) {
        const int k = t.grid.nearest(m.hz);
        r.hz = t.grid.freq(k);
        r.dBm = t.dBm[k];
    }
    const int ref = reference();
    if (m.delta && ref >= 0 && ref != i) {
        const MarkerReading base = read(ref, t);
        r.dHz = r.hz - base.hz;
        r.dDb = r.dBm - base.dBm;
    }
    return r;
}

QString MarkerSet::serialize() const
{
    QStringList parts;
    for (const Marker &m : m_list)
        parts << (m.delta ? "D:" : "") + QString::number(m.hz, 'f', 0);
    return parts.join(';');
}

void MarkerSet::deserialize(const QString &s)
{
    clear();
    for (const QString &p : s.split(';', Qt::SkipEmptyParts)) {
        const bool delta = p.startsWith("D:");
        bool ok = false;
        const double hz = (delta ? p.mid(2) : p).toDouble(&ok);
        if (ok)
            m_list.append(Marker{hz, delta});
    }
}
