#include "digitedit.h"
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

DigitEdit::DigitEdit(QWidget *parent) : QWidget(parent), m_entry(new QLineEdit(this))
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_entry->setObjectName("entry");
    m_entry->setAlignment(Qt::AlignCenter);
    m_entry->hide();
    m_entry->installEventFilter(this);  // Enter / Esc / focus out: eventFilter()
}

void DigitEdit::setFormat(double scale, int decimals, const QString &unit, bool group, bool leadingZeros)
{
    m_scale = scale;
    m_decimals = decimals;
    m_unit = unit;
    m_group = group;
    m_leadingZeros = leadingZeros;
    m_selected = decimals;  // the 1 MHz / 1 dB digit
    update();
}

void DigitEdit::setRange(double min, double max)
{
    m_min = min;
    m_max = max;
    m_selected = std::min(m_selected, intDigits() + m_decimals - 1);
    update();
}

void DigitEdit::setParser(std::function<std::optional<double>(const QString &)> parse)
{
    m_parse = std::move(parse);
}

void DigitEdit::setValue(double v)
{
    m_value = v;
    update();
}

void DigitEdit::setUnknown(bool unknown)
{
    m_unknown = unknown;
    update();
}

QString DigitEdit::text() const
{
    return m_unknown ? QString("?") : QString::number(m_value / m_scale, 'f', m_decimals);
}

bool DigitEdit::enter(const QString &typed)
{
    std::optional<double> v;
    if (m_parse) {
        v = m_parse(typed);
    } else {
        bool ok = false;
        const double x = QString(typed).replace(QChar(0x2212), '-').replace(',', '.').trimmed().toDouble(&ok);
        if (ok)
            v = x * m_scale;
    }
    if (!v)
        return false;
    m_value = std::clamp(*v, m_min, m_max);
    m_unknown = false;
    update();
    emit valueEdited(m_value);
    return true;
}

bool DigitEdit::isEditing() const
{
    return m_entry->isVisible();
}

int DigitEdit::intDigits() const
{
    const double big = std::max({std::abs(m_min), std::abs(m_max), std::abs(m_value)}) / m_scale;
    return std::max(1, int(std::floor(std::log10(std::max(big, 1.0)))) + 1);
}

qint64 DigitEdit::units() const
{
    return std::llround(m_value / m_scale * std::pow(10.0, m_decimals));
}

QVector<DigitEdit::Cell> DigitEdit::cells(QFont *digitFont) const
{
    const qint64 n = units();
    const QString digits = QString::number(std::llabs(n)).rightJustified(intDigits() + m_decimals, '0');
    const int total = int(digits.size()), point = total - m_decimals;
    int first = 0;  // first integer digit that is not a leading zero
    while (first < point - 1 && digits[first] == '0')
        ++first;
    QVector<Cell> out;
    out.append(Cell{n < 0 && m_leadingZeros ? QString(QChar(0x2212)) : QString()});  // sign slot
    for (int i = 0; i < total; ++i) {
        if (i == point && m_decimals > 0)
            out.append(Cell{"."});
        else if (m_group && i > point && (i - point) % 3 == 0)
            out.append(Cell{" "});
        Cell c{QString(digits[i]), total - 1 - i};
        c.dim = i < first && m_leadingZeros;
        c.blank = i < first && !m_leadingZeros;
        out.append(c);
    }
    if (n < 0 && !m_leadingZeros)  // the sign right before the first shown digit
        out[std::max(0, first)] = {QString(QChar(0x2212)), first > 0 ? out[first].digit : -1};

    // geometry at a 100 px reference font, then scaled to fit the widget
    QFont f = font();
    f.setPixelSize(100);
    const QFontMetricsF fm(f);
    double digitW = 0;
    for (char ch = '0'; ch <= '9'; ++ch)
        digitW = std::max(digitW, fm.horizontalAdvance(QChar(ch)));
    auto widthOf = [&](const Cell &c) {  // a digit cell keeps its width when it shows the sign
        return c.digit >= 0 ? digitW
               : c.text == "." ? fm.horizontalAdvance('.') * 1.3
               : c.text == " " ? digitW * 0.35
                               : digitW * 0.75;
    };
    QFont uf = f;
    uf.setPixelSize(45);
    const double unitW = m_unit.isEmpty() ? 0 : QFontMetricsF(uf).horizontalAdvance(" " + m_unit);
    double sum = unitW;
    for (const Cell &c : out)
        sum += widthOf(c);
    const double s = std::min(height() * 0.8 / fm.height(), width() * 0.96 / sum);
    double x = (width() - sum * s) / 2;
    for (Cell &c : out) {
        const double w = widthOf(c) * s;
        c.rect = QRectF(x, 0, w, height());
        x += w;
    }
    if (digitFont) {
        *digitFont = f;
        digitFont->setPixelSize(std::max(1, int(100 * s)));
    }
    return out;
}

int DigitEdit::digitAt(QPointF p) const
{
    for (const Cell &c : cells())
        if (c.digit >= 0 && c.rect.contains(p))
            return c.digit;
    return -1;
}

void DigitEdit::step(int digit, int count)
{
    if (!isEnabled() || digit < 0)
        return;
    const double unit = m_scale / std::pow(10.0, m_decimals);
    const double v = std::clamp((units() + count * std::llround(std::pow(10.0, digit))) * unit, m_min, m_max);
    if (v == m_value && !m_unknown)
        return;
    m_value = v;
    m_unknown = false;
    update();
    emit valueEdited(v);
}

void DigitEdit::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QFont f;
    const QVector<Cell> cs = cells(&f);
    const QPalette::ColorGroup group = isEnabled() ? QPalette::Active : QPalette::Disabled;
    const QColor text = palette().color(group, QPalette::WindowText);
    QColor dim = text, mark = palette().color(QPalette::Highlight);
    dim.setAlphaF(0.22f);
    const QFontMetricsF fm(f);
    p.setFont(f);
    if (m_unknown) {
        const QRectF span = cs.first().rect.united(cs.last().rect);
        p.setPen(dim);
        p.drawText(span, Qt::AlignCenter, "?");
    }
    for (const Cell &c : cs) {
        if (m_unknown)
            break;
        if (c.digit >= 0 && c.digit == m_hover && isEnabled()) {
            QColor bg = mark;
            bg.setAlphaF(0.18f);
            p.fillRect(c.rect.adjusted(1, height() * 0.08, -1, -height() * 0.08), bg);
        }
        if (c.digit >= 0 && c.digit == m_selected && hasFocus() && !m_entry->isVisible())
            p.fillRect(QRectF(c.rect.left() + c.rect.width() * 0.1, (height() + fm.height()) / 2,
                              c.rect.width() * 0.8, std::max(2.0, fm.height() * 0.05)), mark);
        if (c.blank)
            continue;
        p.setPen(c.dim ? dim : text);
        p.drawText(c.rect, Qt::AlignCenter, c.text);
    }
    if (!m_unit.isEmpty()) {
        QFont uf = f;
        uf.setPixelSize(std::max(1, int(f.pixelSize() * 0.45)));
        p.setFont(uf);
        p.setPen(text);
        const double base = (height() + fm.ascent() - fm.descent()) / 2;  // the digits' baseline
        p.drawText(QPointF(cs.last().rect.right() + QFontMetricsF(uf).horizontalAdvance(' '), base), m_unit);
    }
}

void DigitEdit::wheelEvent(QWheelEvent *e)
{
    e->accept();
    m_wheel += e->angleDelta().y() / 120.0;
    const int n = int(m_wheel);
    m_wheel -= n;
    if (n) {
        const int d = digitAt(e->position());
        step(d >= 0 ? d : m_selected, n);
    }
}

void DigitEdit::mousePressEvent(QMouseEvent *e)
{
    setFocus(Qt::MouseFocusReason);
    const int d = digitAt(e->position());
    if (d >= 0)
        m_selected = d;
    update();
}

void DigitEdit::mouseDoubleClickEvent(QMouseEvent *)
{
    startEntry(text() == "?" ? QString() : text());
}

void DigitEdit::mouseMoveEvent(QMouseEvent *e)
{
    const int d = digitAt(e->position());
    if (d != m_hover) {
        m_hover = d;
        update();
    }
}

void DigitEdit::leaveEvent(QEvent *)
{
    m_hover = -1;
    update();
}

void DigitEdit::focusInEvent(QFocusEvent *) { update(); }
void DigitEdit::focusOutEvent(QFocusEvent *) { update(); }

void DigitEdit::keyPressEvent(QKeyEvent *e)
{
    const int top = intDigits() + m_decimals - 1;
    switch (e->key()) {
    case Qt::Key_Left: m_selected = std::min(m_selected + 1, top); return update();
    case Qt::Key_Right: m_selected = std::max(m_selected - 1, 0); return update();
    case Qt::Key_Up: return step(m_selected, 1);
    case Qt::Key_Down: return step(m_selected, -1);
    case Qt::Key_Return:
    case Qt::Key_Enter: return startEntry(text() == "?" ? QString() : text());
    default: break;
    }
    const QString t = e->text();
    if (!t.isEmpty() && (t[0].isDigit() || QString("-+.,").contains(t[0])))
        return startEntry(t);
    QWidget::keyPressEvent(e);
}

void DigitEdit::startEntry(const QString &first)
{
    if (!isEnabled())
        return;
    m_entry->setObjectName(objectName() + "Entry");  // autotest: freqDisplayEntry
    QFont f = font();
    f.setPixelSize(std::max(12, int(height() * 0.35)));
    m_entry->setFont(f);
    m_entry->setGeometry(rect());
    m_entry->setText(first);
    m_entry->show();
    m_entry->setFocus();
    if (first == text())
        m_entry->selectAll();
    update();
}

void DigitEdit::finishEntry(bool apply)
{
    if (!m_entry->isVisible())
        return;
    const QString typed = m_entry->text();
    m_entry->hide();
    setFocus();
    if (apply)
        enter(typed);
    update();
}

bool DigitEdit::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_entry) {
        auto *k = static_cast<QKeyEvent *>(e);
        if (e->type() == QEvent::ShortcutOverride && k->key() == Qt::Key_Escape) {
            e->accept();  // Esc cancels the entry; it does not reach the window's shortcut
            return true;
        }
        if (e->type() == QEvent::KeyPress && k->key() == Qt::Key_Escape) {
            finishEntry(false);
            return true;
        }
        // QLineEdit ignores Return after returnPressed, so it would reach keyPressEvent() and
        // open the entry again: take it here
        if (e->type() == QEvent::KeyPress && (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter)) {
            finishEntry(true);
            return true;
        }
        if (e->type() == QEvent::FocusOut)
            finishEntry(false);
    }
    return QWidget::eventFilter(o, e);
}

QSize DigitEdit::sizeHint() const { return {420, 90}; }
QSize DigitEdit::minimumSizeHint() const { return {200, 44}; }
