#include "dfufile.h"
#include <QFile>
#include <QRegularExpression>
#include <algorithm>

int FirmwareImage::size() const
{
    int n = 0;
    for (const FirmwareSegment &s : segments)
        n += int(s.data.size());
    return n;
}

static quint32 le32(const char *p)
{
    return quint32(quint8(p[0])) | quint32(quint8(p[1])) << 8 | quint32(quint8(p[2])) << 16
           | quint32(quint8(p[3])) << 24;
}

bool readFirmware(const QString &path, FirmwareImage *image, QString *error, quint32 baseAddress)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return false;
    }
    const QByteArray data = f.readAll();
    *image = FirmwareImage{};
    const QString ext = path.section('.', -1).toLower();
    if (ext == "hex")
        return parseIntelHex(data, image, error);
    if (ext == "dfu")
        return parseDfuSe(data, image, error);
    if (data.isEmpty()) {
        *error = "the file is empty";
        return false;
    }
    image->segments.append(FirmwareSegment{baseAddress, data});
    return true;
}

// Intel HEX: data (00), end (01), extended segment (02) and linear (04) address;
// start addresses (03, 05) are ignored. Contiguous records merge into one segment.
bool parseIntelHex(const QByteArray &text, FirmwareImage *image, QString *error)
{
    quint32 upper = 0;
    int lineNo = 0;
    for (const QByteArray &raw : text.split('\n')) {
        ++lineNo;
        const QByteArray line = raw.trimmed();
        if (line.isEmpty())
            continue;
        const QByteArray rec = QByteArray::fromHex(line.mid(1));
        if (line[0] != ':' || rec.size() < 5 || rec.size() != 5 + quint8(rec[0])) {
            *error = QString("bad HEX record on line %1").arg(lineNo);
            return false;
        }
        quint8 sum = 0;
        for (char c : rec)
            sum += quint8(c);
        if (sum != 0) {
            *error = QString("HEX checksum error on line %1").arg(lineNo);
            return false;
        }
        const int n = quint8(rec[0]), type = quint8(rec[3]);
        const quint32 offset = quint32(quint8(rec[1])) << 8 | quint8(rec[2]);
        const QByteArray payload = rec.mid(4, n);
        if (type == 0) {
            const quint32 addr = upper + offset;
            auto &segs = image->segments;
            if (!segs.isEmpty() && segs.last().address + segs.last().data.size() == addr)
                segs.last().data += payload;
            else
                segs.append(FirmwareSegment{addr, payload});
        } else if (type == 1) {
            break;
        } else if (type == 2 && n == 2) {
            upper = (quint32(quint8(payload[0])) << 8 | quint8(payload[1])) << 4;
        } else if (type == 4 && n == 2) {
            upper = (quint32(quint8(payload[0])) << 8 | quint8(payload[1])) << 16;
        }
    }
    if (image->segments.isEmpty()) {
        *error = "no data in the HEX file";
        return false;
    }
    return true;
}

quint32 dfuCrc(const QByteArray &data)
{
    static quint32 table[256];
    if (!table[1])
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    quint32 crc = 0xffffffffu;
    for (qsizetype i = 0; i < data.size() - 4; ++i)
        crc = table[(crc ^ quint8(data[i])) & 0xff] ^ (crc >> 8);
    return crc;
}

// DfuSe file: "DfuSe" prefix (11 bytes), targets ("Target" + 268 bytes) with address
// elements, and the 16-byte DFU suffix ending in "UFD", length and CRC.
bool parseDfuSe(const QByteArray &file, FirmwareImage *image, QString *error)
{
    const char *p = file.constData();
    const int n = int(file.size());
    if (n < 11 + 16 || !file.startsWith("DfuSe")) {
        *error = "not a DfuSe file";
        return false;
    }
    if (file.mid(n - 16 + 8, 3) != "UFD" || le32(p + n - 4) != dfuCrc(file)) {
        *error = "DfuSe file suffix or CRC is wrong";
        return false;
    }
    const int targets = quint8(p[10]);
    int at = 11;
    for (int t = 0; t < targets; ++t) {
        if (at + 274 > n - 16 || file.mid(at, 6) != "Target") {
            *error = "bad DfuSe target";
            return false;
        }
        const quint32 elements = le32(p + at + 270);
        at += 274;
        for (quint32 e = 0; e < elements; ++e) {
            if (at + 8 > n - 16) {
                *error = "bad DfuSe element";
                return false;
            }
            const quint32 addr = le32(p + at), size = le32(p + at + 4);
            at += 8;
            if (at + qint64(size) > n - 16) {
                *error = "DfuSe element beyond the end of the file";
                return false;
            }
            image->segments.append(FirmwareSegment{addr, file.mid(at, size)});
            at += int(size);
        }
    }
    if (image->segments.isEmpty()) {
        *error = "no data in the DfuSe file";
        return false;
    }
    return true;
}

int FlashLayout::size() const
{
    int n = 0;
    for (auto [count, bytes] : sectors)
        n += count * bytes;
    return n;
}

quint32 FlashLayout::pageStart(quint32 address) const
{
    quint32 at = base;
    for (auto [count, bytes] : sectors) {
        const quint32 end = at + quint32(count) * quint32(bytes);
        if (address >= at && address < end)
            return at + (address - at) / quint32(bytes) * quint32(bytes);
        at = end;
    }
    return address;
}

int FlashLayout::pageSize(quint32 address) const
{
    quint32 at = base;
    for (auto [count, bytes] : sectors) {
        const quint32 end = at + quint32(count) * quint32(bytes);
        if (address >= at && address < end)
            return bytes;
        at = end;
    }
    return 0;
}

// "@Internal Flash  /0x08000000/064*0002Kg" or with several groups "/04*016Kg,01*064Kg".
FlashLayout parseFlashLayout(const QString &s, bool *ok)
{
    FlashLayout layout;
    static const QRegularExpression head(R"(/\s*0x([0-9A-Fa-f]+)\s*/(.*))");
    static const QRegularExpression group(R"((\d+)\s*\*\s*(\d+)\s*([KM]?))");
    const auto m = head.match(s);
    bool good = m.hasMatch();
    if (good) {
        layout.base = m.captured(1).toUInt(nullptr, 16);
        for (const QString &g : m.captured(2).section('/', 0, 0).split(',')) {
            const auto gm = group.match(g);
            if (!gm.hasMatch())
                continue;
            const QString unit = gm.captured(3).trimmed().toUpper();
            const int mult = unit == "K" ? 1024 : unit == "M" ? 1024 * 1024 : 1;
            layout.sectors.append(std::pair<int, int>{gm.captured(1).toInt(), gm.captured(2).toInt() * mult});
        }
        good = !layout.sectors.isEmpty();
    }
    if (ok)
        *ok = good;
    return layout;
}
