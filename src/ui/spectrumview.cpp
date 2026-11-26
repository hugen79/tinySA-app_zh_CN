// SpectrumView: traces, scaling, hover readout, image export, context menu.
// Mouse and wheel handling is in spectrummouse.cpp.
#include "spectrumview.h"
#include "units.h"
#include <QInputDialog>
#include <cmath>

static const char *kTraceNames[SpectrumView::kTraces] = {"Live", "M1", "M2", "M3"};
QCPLayerable *createMarkerOverlay(SpectrumView *v);  // spectrummarkers.cpp

double SiTicker::getTickStep(const QCPRange &range)
{
    static const struct { double unit; const char *prefix; } kPrefixes[] = {
        {1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1, ""}, {1e-3, "m"}, {1e-6, "µ"}, {1e-9, "n"},
        {1e-12, "p"}, {1e-15, "f"}};
    m_step = niceStep(range.size() / std::max(2, mTickCount));
    const double a = std::max(std::abs(range.lower), std::abs(range.upper));
    m_unit = 1;
    m_prefix.clear();
    for (const auto &p : kPrefixes)
        if (a >= p.unit) {
            m_unit = p.unit;
            m_prefix = QString::fromUtf8(p.prefix);
            break;
        }
    return m_step;
}

QString SiTicker::getTickLabel(double tick, const QLocale &, QChar, int)
{
    int decimals = 0;  // enough to show the step exactly (2.5 MHz steps need one)
    const double s = m_step / m_unit;
    while (decimals < 9 && std::abs(s * std::pow(10, decimals) - std::round(s * std::pow(10, decimals))) > 1e-6)
        ++decimals;
    return QString::number(tick / m_unit, 'f', decimals) + m_prefix;
}

double LogSiTicker::getTickStep(const QCPRange &range)
{
    m_linear = range.lower <= 0 || range.upper / range.lower < std::sqrt(10.0);
    return m_linear ? SiTicker::getTickStep(range) : 1;
}

int LogSiTicker::getSubTickCount(double tickStep)
{
    return m_linear ? SiTicker::getSubTickCount(tickStep) : 0;
}

QVector<double> LogSiTicker::createTickVector(double tickStep, const QCPRange &range)
{
    if (m_linear)
        return SiTicker::createTickVector(tickStep, range);
    const double decades = std::log10(range.upper / range.lower);
    const QVector<double> mantissas = decades <= 3 ? QVector<double>{1, 2, 5} : QVector<double>{1};
    QVector<double> ticks;
    for (double p = std::pow(10.0, std::floor(std::log10(range.lower))); p <= range.upper; p *= 10)
        for (double m : mantissas)
            if (m * p >= range.lower * (1 - 1e-9) && m * p <= range.upper * (1 + 1e-9))
                ticks << m * p;
    return ticks;
}

QString LogSiTicker::getTickLabel(double tick, const QLocale &locale, QChar formatChar, int precision)
{
    if (m_linear)
        return SiTicker::getTickLabel(tick, locale, formatChar, precision);
    static const struct { double unit; const char *prefix; } kPrefixes[] = {{1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1, ""}};
    for (const auto &p : kPrefixes)
        if (tick >= p.unit * (1 - 1e-9) || p.unit == 1)
            return QString::number(tick / p.unit, 'g', 6) + QString::fromUtf8(p.prefix);
    return {};
}

SpectrumView::SpectrumView(QWidget *parent) : QCustomPlot(parent)
{
    xAxis->setTicker(QSharedPointer<SiTicker>::create());
    xAxis->setLabel(tr("Frequency (Hz)"));
    for (int i = kTraces - 1; i >= 0; --i) {  // added last = drawn on top: the live trace
        m_graph[i] = addGraph();
        m_graph[i]->setName(kTraceNames[i]);
    }
    m_hoverLine = new QCPItemStraightLine(this);
    m_hoverLine->setVisible(false);
    m_hoverText = new QCPItemText(this);
    m_hoverText->position->setType(QCPItemPosition::ptAxisRectRatio);
    m_hoverText->position->setCoords(0.01, 0.01);
    m_hoverText->setPositionAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_hoverText->setPadding(QMargins(6, 3, 6, 3));
    m_hoverText->setVisible(false);
    connect(this, &QCustomPlot::afterLayout, this, &SpectrumView::placeHover);  // clear of this replot's marks
    legend->setVisible(false);
    m_overlay = createMarkerOverlay(this);
    setAntialiasedElements(QCP::aePlottables | QCP::aeItems);
    setNotAntialiasedElements(QCP::aeGrid | QCP::aeAxes);
    setMouseTracking(true);
    setTheme(Theme{});
    setView(ViewSettings{});
    setSpan(0, 350e6);
}

void SpectrumView::setTheme(const Theme &t)
{
    m_theme = t;
    QFont f = font();
    f.setPointSize(std::clamp(t.fontPt, 6, 20));
    setFont(f);  // marker labels use the widget font
    legend->setFont(f);
    m_hoverText->setFont(f);
    setBackground(t.background);
    for (QCPAxis *ax : {xAxis, yAxis}) {
        ax->setBasePen(QPen(t.text));
        ax->setTickPen(QPen(t.text));
        ax->setSubTickPen(Qt::NoPen);
        ax->setTickLabelColor(t.text);
        ax->setLabelColor(t.text);
        ax->setTickLabelFont(f);
        ax->setLabelFont(f);
        ax->grid()->setPen(QPen(t.grid, 1, Qt::DashLine));
    }
    for (int i = 0; i < kTraces; ++i)
        m_graph[i]->setPen(QPen(t.trace[i], t.lineWidth));
    m_hoverLine->setPen(QPen(t.text, 1, Qt::DotLine));
    m_hoverText->setColor(t.text);
    QColor box = t.background;
    box.setAlpha(200);
    m_hoverText->setBrush(box);
    legend->setBrush(box);
    legend->setTextColor(t.text);
    legend->setBorderPen(QPen(t.grid));
    replot(rpQueuedReplot);
}

void SpectrumView::setView(const ViewSettings &v)
{
    m_view = v;
    setUnit(v.unit);
}

void SpectrumView::setUnit(LevelUnit u)
{
    m_view.unit = u;
    if (u == LevelUnit::W || u == LevelUnit::V) {  // SI prefixes on the ticks: 2.5µ
        yAxis->setTicker(QSharedPointer<SiTicker>::create());
    } else {
        auto t = QSharedPointer<QCPAxisTicker>::create();
        t->setTickCount(isDbUnit(u) ? 10 : 5);
        yAxis->setTicker(t);
    }
    QString label = isDbUnit(u) ? levelAxisUnit(u, m_normalized)
                    : m_normalized ? (isVoltUnit(u) ? "Voltage ratio" : "Power ratio")
                                   : QString("%1 (%2)").arg(isVoltUnit(u) ? "Voltage" : "Power", levelUnitName(u));
    if (isVoltUnit(u) && !m_normalized)
        label += ", 50 Ω";
    if (m_extGainDb != 0 && !m_normalized)  // absolute levels: at the device under test
        label += QString(", ext. gain %1 dB").arg(QString::asprintf("%+.1f", m_extGainDb));
    yAxis->setLabel(label);
    for (int i = 0; i < kTraces; ++i)
        redraw(i);
    applyLevels();
}

void SpectrumView::setNormalized(bool on)
{
    m_normalized = on;
    setUnit(m_view.unit);  // labels and values change meaning
}

void SpectrumView::setExtGain(double db)
{
    if (db == m_extGainDb)
        return;
    m_extGainDb = db;
    setUnit(m_view.unit);  // the level label
}

void SpectrumView::setSpan(double startHz, double stopHz, bool log)
{
    if (m_drag == Zone::XStart || m_drag == Zone::XPan || m_drag == Zone::XStop)
        return;  // the user is dragging the axis
    log = log && startHz > 0;
    if (log != m_logX) {
        m_logX = log;
        if (log)
            xAxis->setTicker(QSharedPointer<LogSiTicker>::create());
        else
            xAxis->setTicker(QSharedPointer<SiTicker>::create());
        xAxis->setScaleType(log ? QCPAxis::stLogarithmic : QCPAxis::stLinear);
    }
    xAxis->setRange(startHz, stopHz);
    for (int i = 0; i < kTraces; ++i)
        redraw(i);  // spline density depends on the span
    replot(rpQueuedReplot);
}

void SpectrumView::setTrace(int i, const Trace &t)
{
    m_trace[i] = t;
    redraw(i);
    if (m_view.autoMin || m_view.autoMax)
        applyLevels();
    replot(rpQueuedReplot);
}

void SpectrumView::setShown(int i, bool on)
{
    m_view.show[i] = on;
    redraw(i);
    applyLevels();
}

double SpectrumView::plotValue(double dBm) const
{
    return levelFromDbm(dBm, m_view.unit, m_normalized);
}

QString SpectrumView::levelText(double dBm) const
{
    return formatLevel(dBm, m_view.unit, m_normalized);
}

// Graph levels are kept in dBm (dB units) and W (linear units; normalised: dB and power
// ratio), so a unit change keeps the levels shown. The axis has the unit's values.
double SpectrumView::axisLevel(double kept) const
{
    const LevelUnit u = m_view.unit;
    if (isDbUnit(u))
        return levelFromDbm(kept, u, m_normalized);
    if (!isVoltUnit(u))
        return m_normalized || u == LevelUnit::W ? kept : kept * 1e3;
    const double v = std::copysign(std::sqrt(std::abs(kept) * (m_normalized ? 1 : 50)), kept);  // below 0: dragged there
    return m_normalized || u == LevelUnit::V ? v : v * 1e3;
}

double SpectrumView::keptLevel(double axis) const
{
    const LevelUnit u = m_view.unit;
    if (isDbUnit(u))
        return axis - levelFromDbm(0, u, m_normalized);  // an offset
    const double scale = m_normalized || u == LevelUnit::W || u == LevelUnit::V ? 1 : 1e-3;
    const double v = axis * scale;
    return isVoltUnit(u) ? std::copysign(v * v / (m_normalized ? 1 : 50), v) : v;
}

void SpectrumView::redraw(int i)
{
    const Trace &t = m_trace[i];
    const int n = t.dBm.size();
    QVector<double> x, y;
    const bool spline = m_view.spline && n >= 4 && axisRect()->width() >= 3 * n;
    if (spline) {  // Catmull-Rom through the points, 8 segments per interval
        const int sub = 8;
        auto p = [&](int k) { return plotValue(t.dBm[std::clamp(k, 0, n - 1)]); };
        for (int k = 0; k < n - 1; ++k) {
            const double p0 = p(k - 1), p1 = p(k), p2 = p(k + 1), p3 = p(k + 2);
            for (int s = 0; s < sub; ++s) {
                const double u = double(s) / sub;
                x << t.grid.freqAt(k + u);
                y << 0.5 * (2 * p1 + (p2 - p0) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u * u
                            + (3 * p1 - p0 - 3 * p2 + p3) * u * u * u);
            }
        }
        x << t.grid.freq(n - 1);
        y << p(n - 1);
    } else {
        x.resize(n);
        y.resize(n);
        for (int k = 0; k < n; ++k) {
            x[k] = t.grid.freq(k);
            y[k] = plotValue(t.dBm[k]);
        }
    }
    QCPGraph *g = m_graph[i];
    g->setData(x, y, true);
    g->setVisible(m_view.show[i] && n > 0);
    g->setScatterStyle(m_view.points ? QCPScatterStyle(QCPScatterStyle::ssDisc, 4) : QCPScatterStyle());
    g->setScatterSkip(spline ? 7 : 0);  // dots only at the measured points (every 8th spline sample)
    int shown = 0;
    for (int k = 0; k < kTraces; ++k)
        shown += m_graph[k]->visible();
    legend->setVisible(shown > 1);
    for (int k = 0; k < kTraces; ++k)
        if (m_graph[k]->visible())
            m_graph[k]->addToLegend();
        else
            m_graph[k]->removeFromLegend();
}

void SpectrumView::applyLevels()
{
    const bool linear = m_view.linear();
    double lo = axisLevel(linear ? m_view.linMin : m_view.logMin);
    double hi = axisLevel(linear ? m_view.linMax : m_view.logMax);
    if (m_view.autoMin || m_view.autoMax) {
        double vmin = qInf(), vmax = -qInf();
        for (int i = 0; i < kTraces; ++i)
            if (m_graph[i]->visible())
                for (double d : m_trace[i].dBm)
                    if (!std::isnan(d)) {
                        vmin = std::min(vmin, plotValue(d));
                        vmax = std::max(vmax, plotValue(d));
                    }
        if (vmin <= vmax) {
            if (m_view.autoMax)
                hi = linear ? vmax * 1.1 : std::ceil(vmax / 5) * 5 + 5;
            if (m_view.autoMin)
                lo = linear ? 0 : std::floor(vmin / 5) * 5 - 5;
        }
    }
    if (!(hi > lo))
        hi = linear ? lo * 2 + 1e-12 : lo + 10;
    yAxis->setRange(lo, hi);
    storeLevels();
    replot(rpQueuedReplot);
}

void SpectrumView::storeLevels()
{
    const bool linear = m_view.linear();
    (linear ? m_view.linMin : m_view.logMin) = keptLevel(yAxis->range().lower);
    (linear ? m_view.linMax : m_view.logMax) = keptLevel(yAxis->range().upper);
}

void SpectrumView::autoScale()
{
    const bool amin = m_view.autoMin, amax = m_view.autoMax;
    m_view.autoMin = m_view.autoMax = true;
    applyLevels();
    m_view.autoMin = amin;
    m_view.autoMax = amax;
}

void SpectrumView::updateHover(QPointF p)
{
    int ref = -1;  // the live trace, else the first shown memory
    for (int i = 0; i < kTraces && ref < 0; ++i)
        if (m_graph[i]->visible())
            ref = i;
    if (ref < 0)
        return hideHover();
    const Trace &t = m_trace[ref];
    const int k = t.grid.nearest(xAxis->pixelToCoord(p.x()));
    const double f = t.grid.freq(k);
    QString text = formatHz(f, t.grid.stepAt(k));
    for (int i = 0; i < kTraces; ++i)
        if (m_graph[i]->visible())
            text += QString("   %1 %2").arg(kTraceNames[i], levelText(i == ref ? t.dBm[k] : m_trace[i].at(f)));
    m_hoverLine->point1->setCoords(f, 0);
    m_hoverLine->point2->setCoords(f, 1);
    m_hoverLine->setVisible(true);
    m_hoverText->setText(text);
    m_hoverText->setVisible(true);
    replot(rpQueuedReplot);
}

void SpectrumView::hideHover()
{
    if (!m_hoverText->visible())
        return;
    m_hoverLine->setVisible(false);
    m_hoverText->setVisible(false);
    replot(rpQueuedReplot);
}

QString SpectrumView::hoverText() const
{
    return m_hoverText->visible() ? m_hoverText->text() : QString("");
}

QImage SpectrumView::image(const QString &header)
{
    const bool hover = m_hoverText->visible();
    m_hoverLine->setVisible(false);
    m_hoverText->setVisible(false);
    const QPixmap pm = toPixmap(width(), height());
    m_hoverLine->setVisible(hover);
    m_hoverText->setVisible(hover);
    if (header.isEmpty())
        return pm.toImage();
    const int hh = fontMetrics().height() + 10;
    QImage img(pm.width(), pm.height() + hh, QImage::Format_RGB32);
    img.fill(m_theme.background);
    QPainter p(&img);
    p.setPen(m_theme.text);
    p.drawText(QRect(8, 0, img.width() - 16, hh), Qt::AlignVCenter | Qt::AlignLeft, header);
    p.drawPixmap(0, hh, pm);
    return img;
}

QList<QAction *> SpectrumView::menuActions(QPointF pos, QObject *owner)
{
    const double hz = xAxis->pixelToCoord(pos.x());
    const double res = niceStep((m_logX ? hz : xAxis->range().size()) / 1000);
    QList<QAction *> list;
    auto add = [&](const QString &text, auto fn) {
        auto *a = new QAction(text, owner);
        connect(a, &QAction::triggered, this, fn);
        list << a;
        return a;
    };
    auto sep = [&] {
        auto *a = new QAction(owner);
        a->setSeparator(true);
        list << a;
    };
    auto toggle = [&](const QString &text, bool &flag) {
        QAction *a = add(text, [this, &flag](bool on) {
            flag = on;
            for (int i = 0; i < kTraces; ++i)
                redraw(i);
            applyLevels();
            emit viewChanged();
        });
        a->setCheckable(true);
        a->setChecked(flag);
    };
    const int mi = markerAt(pos);
    if (mi >= 0) {
        const QString name = m_marks[mi].label;
        add("Delete marker " + name, [this, mi] { emit markerRemoveRequested(mi); });
        QAction *d = add("Delta marker " + name, [this, mi] { emit markerDeltaRequested(mi); });
        d->setCheckable(true);
        d->setChecked(name.startsWith('D'));
    } else {
        add("Add marker here", [this, hz] { emit markerAddRequested(hz); });
    }
    add("Marker to peak", [this] { emit markerPeakRequested(); });
    toggle("Show max marker", m_view.peakMax);
    toggle("Show min marker", m_view.peakMin);
    toggle("Marker readout on graph", m_view.markerText);
    toggle("Show markers", m_view.showMarkers);
    sep();
    const QString f = formatHz(roundTo(hz, res), res);
    add("Start here (" + f + ")", [this, hz] { emit rangeRequested(RangeOp::Start, hz, 0); });
    add("Center here (" + f + ")", [this, hz] { emit rangeRequested(RangeOp::Center, hz, 0); });
    add("Stop here (" + f + ")", [this, hz] { emit rangeRequested(RangeOp::Stop, hz, 0); });
    sep();
    add("Auto scale now", [this] { autoScale(); emit viewChanged(); });
    toggle("Auto scale top", m_view.autoMax);
    toggle("Auto scale bottom", m_view.autoMin);
    auto askLevel = [this](bool top) {
        const QString unit = levelAxisUnit(m_view.unit, m_normalized);
        bool ok = false;
        const double cur = top ? yAxis->range().upper : yAxis->range().lower;
        const double v = QInputDialog::getDouble(this, top ? "Graph top" : "Graph bottom", unit, cur,
                                                 -1e9, 1e9, m_view.linear() ? 12 : 1, &ok);
        if (!ok)
            return;
        (top ? m_view.autoMax : m_view.autoMin) = false;
        yAxis->setRange(top ? yAxis->range().lower : v, top ? v : yAxis->range().upper);
        storeLevels();
        replot(rpQueuedReplot);
        emit viewChanged();
    };
    add("Set top...", [askLevel] { askLevel(true); });
    add("Set bottom...", [askLevel] { askLevel(false); });
    sep();
    toggle("Spline", m_view.spline);
    toggle("Show points", m_view.points);
    sep();
    add("Save image...", [this] { emit saveImageRequested(); });
    add("Copy image", [this] { emit copyImageRequested(); });
    return list;
}

void SpectrumView::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu menu(this);
    menu.addActions(menuActions(e->pos(), &menu));
    menu.exec(e->globalPos());
}

QPointF SpectrumView::pointFor(const QString &zone, double hz, double level) const
{
    const QRect r = axisRect()->rect();
    if (zone == "xaxis")
        return {xAxis->coordToPixel(hz), r.bottom() + 12.0};
    if (zone == "yaxis")
        return {r.left() - 25.0, yAxis->coordToPixel(level)};
    return {xAxis->coordToPixel(hz), yAxis->coordToPixel(level)};
}
