#include "protocol.h"
#include <QImage>
#include <cstring>

void RxParser::expect(const QByteArray &commandLine, Body body, int bytes)
{
    m_echo = commandLine;
    m_body = body;
    m_captureBytes = bytes;
}

void RxParser::reset()
{
    *this = RxParser();
}

QVector<RxEvent> RxParser::feed(const QByteArray &bytes)
{
    QVector<RxEvent> out;
    m_buf += bytes;
    while (m_pos < m_buf.size() && step(out))
        ;
    flushScan(out);
    m_buf.remove(0, m_pos);
    m_pos = 0;
    if (m_state == State::Text && m_buf.size() > 65536) {  // binary junk without newlines
        out.append(RxEvent{RxEvent::Line, m_buf.left(80) + "..."});
        m_buf.clear();
    }
    return out;
}

bool RxParser::atPrompt() const
{
    return m_buf.size() - m_pos >= 4 && std::memcmp(m_buf.constData() + m_pos, "ch> ", 4) == 0;
}

bool RxParser::atPromptPrefix() const
{
    const int n = int(m_buf.size() - m_pos);
    return n < 4 && std::memcmp(m_buf.constData() + m_pos, "ch> ", n) == 0;
}

bool RxParser::step(QVector<RxEvent> &out)
{
    const int avail = int(m_buf.size() - m_pos);
    const char *p = m_buf.constData() + m_pos;
    switch (m_state) {
    case State::Text:
        return textStep(out);
    case State::ScanStart:
        if (*p == '\r' || *p == '\n') {
            ++m_pos;
            return true;
        }
        if (*p == '{') {
            ++m_pos;
            m_scanCount = 0;
            m_state = State::ScanPoints;
            return true;
        }
        m_state = State::Text;  // text reply instead of data, e.g. "frequency range is invalid"
        return true;
    case State::ScanPoints:
        return scanStep(out);
    case State::CaptureHead: {  // RLE8 header, or already the first raw pixels
        if (avail < 8 && avail < m_captureBytes)
            return false;
        auto u16 = [&](int k) { return int(quint8(p[k]) | quint8(p[k + 1]) << 8); };
        const bool rle = avail >= 8 && p[0] == 'B' && p[1] == 'M' && p[6] == 8 && p[7] == 1
                         && u16(2) * u16(4) * 2 == m_captureBytes;
        if (!rle) {
            m_state = State::Capture;
            return true;
        }
        m_rle = m_buf.mid(m_pos, 8);
        m_rleRows = u16(4);
        m_pos += 8;
        m_state = State::Rle8Palette;
        return true;
    }
    case State::Capture:
        if (avail < m_captureBytes)
            return false;
        out.append(RxEvent{RxEvent::Pixels, m_buf.mid(m_pos, m_captureBytes)});
        m_pos += m_captureBytes;
        m_state = State::Text;
        return true;
    case State::Rle8Palette:
    case State::Rle8Rows: {  // u16 length + that many bytes: the palette, then each row
        if (avail < 2)
            return false;
        const int n = quint8(p[0]) | quint8(p[1]) << 8;
        if (n > 4096) {  // not a sane block: give up on this capture
            m_state = State::Text;
            m_rleRegion = false;
            return true;
        }
        if (avail < 2 + n)
            return false;
        m_rle += m_buf.mid(m_pos, 2 + n);
        m_pos += 2 + n;
        if (m_state == State::Rle8Palette) {
            m_state = m_rleRegion ? State::Rle8Stream : State::Rle8Rows;
        } else if (--m_rleRows == 0) {
            RxEvent e{RxEvent::Pixels, m_rle};
            e.rle8 = true;
            out.append(e);
            m_rle.clear();
            m_state = State::Text;
        }
        return true;
    }
    case State::Rle8Stream: {  // "rle8" push: PackBits blocks until w*h indices
        const int n = qint8(p[0]);
        const int need = n >= 0 ? n + 2 : n == -128 ? 1 : 2;
        if (avail < need)
            return false;
        m_rle += m_buf.mid(m_pos, need);
        m_pos += need;
        m_rleLeft -= n >= 0 ? n + 1 : n == -128 ? 0 : 1 - n;
        if (m_rleLeft <= 0) {
            RxEvent e{RxEvent::Region, m_rle};
            e.rect = m_region;
            e.rle8 = true;
            out.append(e);
            m_rle.clear();
            m_rleRegion = false;
            m_swallowPrompt = true;  // the push ends with its own "ch> "
            m_state = State::Text;
        }
        return true;
    }
    case State::RegionHeader: {
        if (avail < 8)
            return false;
        auto i16 = [&](int k) { return int(qint16(quint8(p[2 * k]) | quint8(p[2 * k + 1]) << 8)); };
        m_region = QRect(i16(0), i16(1), i16(2), i16(3));
        m_pos += 8;
        const bool sane = m_region.x() >= 0 && m_region.y() >= 0 && m_region.width() > 0
                          && m_region.height() > 0 && m_region.right() < 1024
                          && m_region.bottom() < 1024;
        m_state = !sane ? State::Text : m_push == 'r' ? State::Rle8Palette : State::RegionBody;
        if (sane && m_push == 'r') {  // palette block, then the PackBits stream
            m_rle.clear();
            m_rleLeft = m_region.width() * m_region.height();
            m_rleRegion = true;
        }
        return true;
    }
    case State::RegionBody: {
        const int need = m_push == 'b' ? m_region.width() * m_region.height() * 2 : 2;
        if (avail < need)
            return false;
        RxEvent e{RxEvent::Region, m_buf.mid(m_pos, need)};
        e.rect = m_region;
        e.fill = m_push == 'f';
        e.scroll = m_push == 's';
        out.append(e);
        m_pos += need;
        m_swallowPrompt = true;  // the push ends with its own "ch> "
        m_state = State::Text;
        return true;
    }
    }
    return false;
}

bool RxParser::textStep(QVector<RxEvent> &out)
{
    if (atPrompt()) {
        m_pos += 4;
        if (m_swallowPrompt)
            m_swallowPrompt = false;
        else
            out.append(RxEvent{RxEvent::Prompt});
        return true;
    }
    if (atPromptPrefix())
        return false;
    const int nl = int(m_buf.indexOf('\n', m_pos));
    if (nl < 0)
        return false;
    QByteArray line = m_buf.mid(m_pos, nl - m_pos);
    m_pos = nl + 1;
    if (line.endsWith('\r'))
        line.chop(1);
    // The unit echoes typed characters one by one while another thread pushes screen
    // regions, so a push can land inside a line ("re" + "bulk..." + "fresh off"):
    // keep the fragment and finish the line after the push.
    for (const char *push : {"bulk", "fill", "rle8", "scrl"})
        if (line.endsWith(push)) {
            m_fragment += line.left(line.size() - 4);
            m_push = push[0];
            m_state = State::RegionHeader;
            return true;
        }
    line.prepend(m_fragment);
    m_fragment.clear();

    if (!m_echo.isEmpty() && line.endsWith(m_echo)) {
        m_echo.clear();
        out.append(RxEvent{RxEvent::Echo, line});
        if (m_body == Body::ScanRaw)
            m_state = State::ScanStart;
        else if (m_body == Body::Capture)
            m_state = State::CaptureHead;
        return true;
    }
    out.append(RxEvent{RxEvent::Line, line});
    return true;
}

bool RxParser::scanStep(QVector<RxEvent> &out)
{
    const int avail = int(m_buf.size() - m_pos);
    const char *p = m_buf.constData() + m_pos;
    if (*p == 'x') {
        if (avail < 3)
            return false;
        m_scan.append(qint16(quint8(p[1]) | quint8(p[2]) << 8));
        ++m_scanCount;
        m_pos += 3;
        return true;
    }
    if (*p == '}') {
        ++m_pos;
        flushScan(out);
        RxEvent e{RxEvent::ScanEnd};
        e.count = m_scanCount;
        out.append(e);
        m_state = State::Text;
        return true;
    }
    // Out-of-band text between 20-point chunks, e.g. "Command timeout" + prompt from
    // the shell thread while a long scan runs. Logged, prompt swallowed.
    if (atPrompt()) {
        m_pos += 4;
        return true;
    }
    if (atPromptPrefix())
        return false;
    const int nl = int(m_buf.indexOf('\n', m_pos));
    if (nl < 0)
        return false;
    QByteArray line = m_buf.mid(m_pos, nl - m_pos);
    m_pos = nl + 1;
    flushScan(out);  // keep event order: points before the text
    out.append(RxEvent{RxEvent::Line, line.trimmed()});
    return true;
}

void RxParser::flushScan(QVector<RxEvent> &out)
{
    if (m_scan.isEmpty())
        return;
    RxEvent e{RxEvent::ScanData};
    e.values.swap(m_scan);
    out.append(e);
}

QByteArray unpackBits(const char *src, int srcLen, int outLen)
{
    QByteArray out;
    out.reserve(outLen);
    for (int i = 0; i < srcLen && out.size() < outLen;) {
        const int n = qint8(src[i++]);
        if (n >= 0) {  // n+1 literal bytes
            const int k = std::min(n + 1, srcLen - i);
            out.append(src + i, k);
            i += k;
        } else if (n != -128 && i < srcLen) {  // next byte, 1-n times
            out.append(QByteArray(1 - n, src[i++]));
        }
    }
    out.resize(outLen, 0);
    return out;
}

static inline quint16 be16(const char *p)
{
    return quint16(quint8(p[0]) << 8 | quint8(p[1]));
}

void blitRgb565BE(QImage &img, const QRect &r, const QByteArray &pixels)
{
    const QRect clip = r & img.rect();
    const char *src = pixels.constData();
    for (int y = clip.top(); y <= clip.bottom(); ++y) {
        auto *dst = reinterpret_cast<quint16 *>(img.scanLine(y));
        const char *row = src + 2 * ((y - r.top()) * r.width());
        for (int x = clip.left(); x <= clip.right(); ++x)
            dst[x] = be16(row + 2 * (x - r.left()));
    }
}

void fillRgb565BE(QImage &img, const QRect &r, const QByteArray &colour)
{
    if (colour.size() < 2)
        return;
    const QRect clip = r & img.rect();
    const quint16 c = be16(colour.constData());
    for (int y = clip.top(); y <= clip.bottom(); ++y) {
        auto *dst = reinterpret_cast<quint16 *>(img.scanLine(y));
        std::fill(dst + clip.left(), dst + clip.right() + 1, c);
    }
}

bool decodeRle8(const QByteArray &payload, QImage &img)
{
    const char *p = payload.constData();
    int at = 0;
    auto u16 = [&](int k) { return int(quint8(p[k]) | quint8(p[k + 1]) << 8); };
    if (payload.size() < 10 || p[0] != 'B' || p[1] != 'M')
        return false;
    const int w = u16(2), h = u16(4), paletteBytes = u16(8);
    at = 10;
    if (payload.size() < at + paletteBytes)
        return false;
    QVector<quint16> palette;
    for (int i = 0; i + 1 < paletteBytes; i += 2)
        palette << be16(p + at + i);  // stored as read from the LCD: big-endian RGB565
    at += paletteBytes;
    img = QImage(w, h, QImage::Format_RGB16);
    for (int y = 0; y < h; ++y) {
        if (payload.size() < at + 2)
            return false;
        const int n = u16(at);
        at += 2;
        if (payload.size() < at + n)
            return false;
        const QByteArray idx = unpackBits(p + at, n, w);
        at += n;
        auto *row = reinterpret_cast<quint16 *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int i = quint8(idx[x]);
            row[x] = i < palette.size() ? palette[i] : 0;
        }
    }
    return true;
}

bool blitRle8(QImage &img, const QRect &r, const QByteArray &payload)
{
    const char *p = payload.constData();
    const int n = payload.size() >= 2 ? quint8(p[0]) | quint8(p[1]) << 8 : 0;  // palette bytes
    if (payload.size() < 2 + n)
        return false;
    const QByteArray idx = unpackBits(p + 2 + n, int(payload.size()) - 2 - n, r.width() * r.height());
    const QRect clip = r & img.rect();
    for (int y = clip.top(); y <= clip.bottom(); ++y) {
        auto *row = reinterpret_cast<quint16 *>(img.scanLine(y));
        const char *ix = idx.constData() + (y - r.top()) * r.width() - r.left();
        for (int x = clip.left(); x <= clip.right(); ++x) {
            const int i = 2 * quint8(ix[x]);
            row[x] = i + 1 < n ? be16(p + 2 + i) : 0;  // BE RGB565 as read from the LCD
        }
    }
    return true;
}

void scrollRgb16(QImage &img, const QRect &r, int dy)
{
    const QRect clip = r & img.rect();
    const int top = std::max(clip.top(), -dy), bottom = std::min(clip.bottom(), img.height() - 1 - dy);
    for (int i = 0; i <= bottom - top; ++i) {  // bottom-up when moving down, so rows are read before overwritten
        const int y = dy > 0 ? bottom - i : top + i;
        std::memcpy(img.scanLine(y + dy) + 2 * clip.left(), img.scanLine(y) + 2 * clip.left(),
                    size_t(2 * clip.width()));
    }
}
