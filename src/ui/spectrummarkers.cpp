// SpectrumView markers: one overlay layer draws user markers, peak markers and the
// optional readout block in a single pass.
#include "markers.h"
#include "spectrumview.h"
#include "units.h"
#include <algorithm>
#include <cmath>

class MarkerOverlay : public QCPLayerable {
public:
    explicit MarkerOverlay(SpectrumView *v) : QCPLayerable(v, "markers"), m_view(v) {}

protected:
    void applyDefaultAntialiasingHint(QCPPainter *p) const override
    {
        applyAntialiasingHint(p, true, QCP::aeItems);
    }
    void draw(QCPPainter *p) override { m_view->drawMarkers(p); }
    QRect clipRect() const override { return m_view->axisRect()->rect(); }

private:
    SpectrumView *m_view;
};

QCPLayerable *createMarkerOverlay(SpectrumView *v)
{
    v->addLayer("markers", v->layer("main"), QCustomPlot::limAbove);
    return new MarkerOverlay(v);
}

void SpectrumView::setMarkers(const QList<MarkerMark> &marks)
{
    m_marks = marks;
    replot(rpQueuedReplot);
}

QPointF SpectrumView::peak(bool max) const
{
    return m_graph[0]->visible() ? tracePeak(m_trace[0], max) : QPointF(qQNaN(), qQNaN());
}

int SpectrumView::markerAt(QPointF p) const
{
    int best = -1;
    double bestDist = 6;  // px
    for (int i = 0; i < m_marks.size(); ++i) {
        const double d = std::abs(xAxis->coordToPixel(m_marks[i].hz) - p.x());
        if (d <= bestDist)
            best = i, bestDist = d;
    }
    return best;
}

// Triangle with its tip at (x, y), pointing down (above the trace) or up.
static void triangle(QCPPainter *p, double x, double y, bool down, const QColor &c, bool filled)
{
    const double h = down ? -11 : 11;
    const QPolygonF tri{{x, y}, {x - 6, y + h}, {x + 6, y + h}};
    p->setPen(QPen(c, 1.2));
    p->setBrush(filled ? QBrush(c) : Qt::NoBrush);
    p->drawPolygon(tri);
}

QVector<SpectrumView::Mark> SpectrumView::markLayout() const
{
    const QRect r = axisRect()->rect();
    const QFontMetrics fm(font());
    QVector<Mark> out;
    auto add = [&](const QString &text, QPointF tip, double y, bool above, const QColor &c, auto kind) {
        QRectF at(QPointF(), fm.boundingRect(text).adjusted(-3, -1, 3, 1).size());
        at.moveCenter({tip.x(), above ? y - at.height() / 2 : y + at.height() / 2});
        at.moveLeft(std::clamp(at.left(), double(r.left()), double(r.right()) - at.width()));
        at.moveTop(std::clamp(at.top(), double(r.top()), double(r.bottom()) - at.height()));
        out.append(Mark{text, at, c, tip, kind});
    };
    // user markers: dotted line, triangle on the trace, name above it
    for (const MarkerMark &m : m_view.showMarkers ? m_marks : QList<MarkerMark>{}) {
        const double x = xAxis->coordToPixel(m.hz);
        if (x < r.left() || x > r.right())
            continue;
        const bool level = !std::isnan(m.dBm);
        const double y = level ? std::clamp(yAxis->coordToPixel(plotValue(m.dBm)), r.top() + 14.0, double(r.bottom()))
                               : r.top() + 14;
        add(m.label, {x, y}, y - 13, true, m.selected ? m_theme.selected : m_theme.marker,
            level ? Mark::User : Mark::UserLine);
    }
    // peak markers of the live trace, interpolated between points
    for (bool max : {true, false}) {
        if (!(max ? m_view.peakMax : m_view.peakMin))
            continue;
        const QPointF pk = peak(max);
        if (std::isnan(pk.x()))
            continue;
        const double x = xAxis->coordToPixel(pk.x());
        const double y = std::clamp(yAxis->coordToPixel(plotValue(pk.y())), double(r.top()), double(r.bottom()));
        const QString text = QString("%1 %2 %3").arg(max ? "max" : "min",
            formatHz(pk.x(), std::max(1.0, m_trace[0].grid.stepAt(m_trace[0].grid.nearest(pk.x())) / 100)), levelText(pk.y()));
        add(text, {x, y}, max ? y - 13 : y + 13, max, m_theme.trace[0], max ? Mark::Max : Mark::Min);
    }
    return out;
}

QRect SpectrumView::readoutRect() const
{
    if (!m_view.markerText || !m_view.showMarkers || m_marks.isEmpty())
        return {};
    const QRect r = axisRect()->rect();
    const QFontMetrics fm(font());
    int w = 0;
    for (const MarkerMark &m : m_marks)
        w = std::max(w, fm.horizontalAdvance(m.readout));
    const int top = r.top() + 6 + (legend->visible() ? legend->outerRect().height() + 6 : 0);
    return QRect(r.right() - w - 14, top, w + 10, fm.height() * int(m_marks.size()) + 6);
}

bool SpectrumView::marksIntersect(const QRectF &box) const
{
    const QVector<Mark> marks = markLayout();
    return box.intersects(readoutRect())
           || std::any_of(marks.begin(), marks.end(), [&](const Mark &m) { return m.box.intersects(box); });
}

// The hover readout goes top left, else bottom left or top centre: the first place clear of the
// marks (a strong peak at the far left, e.g. 0 Hz with a large span, puts its label top left)
void SpectrumView::placeHover()
{
    static const struct { double x, y; Qt::Alignment a; } kPlaces[] = {
        {0.01, 0.01, Qt::AlignLeft | Qt::AlignTop}, {0.01, 0.99, Qt::AlignLeft | Qt::AlignBottom},
        {0.5, 0.01, Qt::AlignHCenter | Qt::AlignTop}};
    if (!m_hoverText->visible())
        return;
    const QRect r = axisRect()->rect();
    const QFontMetrics fm(m_hoverText->font());
    const QSizeF s(fm.horizontalAdvance(m_hoverText->text()) + 12, fm.height() + 6);  // + padding
    const auto *pl = std::find_if(std::begin(kPlaces), std::end(kPlaces), [&](const auto &k) {
        return !marksIntersect({{r.left() + k.x * r.width() - (k.a & Qt::AlignHCenter ? s.width() / 2 : 0),
                                 r.top() + k.y * r.height() - (k.a & Qt::AlignBottom ? s.height() : 0)}, s});
    });
    if (pl == std::end(kPlaces))
        pl = kPlaces;  // nowhere clear: the usual place
    m_hoverText->position->setCoords(pl->x, pl->y);
    m_hoverText->setPositionAlignment(pl->a);
}

bool SpectrumView::hoverCoversMark() const
{
    return m_hoverText->visible()
           && marksIntersect({m_hoverText->topLeft->pixelPosition(), m_hoverText->bottomRight->pixelPosition()});
}

void SpectrumView::drawMarkers(QCPPainter *p)
{
    const QRect r = axisRect()->rect();
    QColor bg = m_theme.background;
    bg.setAlpha(170);
    for (const Mark &m : markLayout()) {
        if (m.kind == Mark::User || m.kind == Mark::UserLine) {
            p->setPen(QPen(m.colour, 1, Qt::DotLine));
            p->drawLine(QPointF(m.tip.x(), r.top()), QPointF(m.tip.x(), r.bottom()));
        }
        if (m.kind != Mark::UserLine)
            triangle(p, m.tip.x(), m.tip.y(), m.kind != Mark::Min, m.colour, m.kind == Mark::User);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        p->drawRect(m.box);
        p->setPen(m.colour);
        p->drawText(m.box, Qt::AlignCenter, m.text);
    }
    // readout block, top right
    const QRect box = readoutRect();
    if (box.isEmpty())
        return;
    const QFontMetrics fm(font());
    bg.setAlpha(200);
    p->setPen(QPen(m_theme.grid));
    p->setBrush(bg);
    p->drawRect(box);
    for (int i = 0; i < m_marks.size(); ++i) {
        p->setPen(m_marks[i].selected ? m_theme.selected : m_theme.text);
        p->drawText(box.left() + 5, box.top() + 3 + fm.height() * i + fm.ascent(), m_marks[i].readout);
    }
}
