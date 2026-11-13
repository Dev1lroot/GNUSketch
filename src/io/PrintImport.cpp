// SPDX-License-Identifier: GPL-3.0-or-later
#include "PrintImport.h"

#include "core/Components.h"
#include "core/Effects.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QtEndian>

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <set>

namespace gs {

namespace {

bool fail(QString* error, const QString& msg) {
    if (error) *error = msg;
    return false;
}

// "millimeter", "inch", "mm", "in", ... in meters (0 if unknown).
double unitMeters(QString u) {
    u = u.trimmed().toLower();
    if (u == "micron" || u == "um") return 1e-6;
    if (u == "millimeter" || u == "millimetre" || u == "mm") return 1e-3;
    if (u == "centimeter" || u == "centimetre" || u == "cm") return 1e-2;
    if (u == "meter" || u == "metre" || u == "m") return 1.0;
    if (u == "inch" || u == "in") return 0.0254;
    if (u == "foot" || u == "feet" || u == "ft") return 0.3048;
    return 0;
}

// The unit tag GNUSketch writes into STL headers and PLY/OFF comments.
bool taggedUnit(const QByteArray& head, double& m) {
    static const QRegularExpression re(QStringLiteral("(?:units[=_ ]|gnusketch_units_)(mm|cm|m|in|ft)\\b"));
    const auto match = re.match(QString::fromLatin1(head));
    if (!match.hasMatch()) return false;
    m = unitMeters(match.captured(1));
    return m > 0;
}

Color parseHexColor(const QString& s) {
    QString h = s.trimmed();
    if (h.startsWith('#')) h.remove(0, 1);
    bool ok = false;
    const uint v = h.toUInt(&ok, 16);
    if (!ok || (h.size() != 6 && h.size() != 8)) return Color(0.8f, 0.8f, 0.8f);
    if (h.size() == 6) return Color::rgb8((v >> 16) & 255, (v >> 8) & 255, v & 255);
    return Color::rgb8((v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255);
}

// Colours shared by many triangles become one material each.
struct ColorTable {
    explicit ColorTable(PrintModel& model) : m(model) {}
    PrintModel& m;
    std::map<std::pair<QString, int>, int> index;
    int get(const QString& group, int i, const Color& c, const std::string& name) {
        auto key = std::make_pair(group, i);
        auto it = index.find(key);
        if (it != index.end()) return it->second;
        const int k = (int)m.colors.size();
        m.colors.push_back(c);
        m.colorNames.push_back(name.empty() ? "Color " + std::to_string(k + 1) : name);
        index[key] = k;
        return k;
    }
};

// ---- 3MF ---------------------------------------------------------------------

bool read3mf(const QByteArray& bytes, PrintModel& out, QString* error) {
    std::map<QString, QByteArray> files;
    if (!readZip(bytes, files, error)) return false;
    QByteArray xml;
    for (const auto& [name, data] : files)
        if (name.endsWith(QLatin1String(".model"), Qt::CaseInsensitive) && (xml.isEmpty() || name.contains(QLatin1String("3dmodel"))))
            xml = data;
    if (xml.isEmpty()) return fail(error, QStringLiteral("The 3MF file has no model inside."));

    struct Tri { int v[3]; QString pid; int p = -1; };
    struct Comp { QString object; Mat4 xf; };
    struct Object { std::vector<Vec3> verts; std::vector<Tri> tris; QString pid; int pindex = -1; std::vector<Comp> comps; };
    std::map<QString, Object> objects;
    std::map<QString, std::vector<std::pair<Color, std::string>>> groups; // basematerials and colorgroups
    std::vector<Comp> build;
    double scale = 1e-3;

    auto parseXf = [](const QString& s) {
        Mat4 m;
        const QStringList v = s.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (v.size() != 12) return m;
        double a[12];
        for (int i = 0; i < 12; ++i) a[i] = v[i].toDouble();
        // Row vectors: p' = p * M, the last row is the translation.
        for (int col = 0; col < 3; ++col)
            for (int row = 0; row < 3; ++row) m.m[col * 4 + row] = a[col * 3 + row];
        m.m[12] = a[9];
        m.m[13] = a[10];
        m.m[14] = a[11];
        return m;
    };

    QXmlStreamReader r(xml);
    Object* cur = nullptr;
    QString curGroup;
    while (!r.atEnd()) {
        if (r.readNext() != QXmlStreamReader::StartElement) continue;
        const QString n = r.name().toString();
        const auto at = r.attributes();
        if (n == QLatin1String("model")) {
            const double m = unitMeters(at.hasAttribute("unit") ? at.value("unit").toString() : QStringLiteral("millimeter"));
            if (m > 0) scale = m;
        } else if (n == QLatin1String("basematerials") || n == QLatin1String("colorgroup")) {
            curGroup = at.value("id").toString();
            groups[curGroup];
        } else if (n == QLatin1String("base")) {
            groups[curGroup].push_back({parseHexColor(at.value("displaycolor").toString()), at.value("name").toString().toStdString()});
        } else if (n == QLatin1String("color") && !curGroup.isEmpty()) {
            groups[curGroup].push_back({parseHexColor(at.value("color").toString()), std::string()});
        } else if (n == QLatin1String("object")) {
            cur = &objects[at.value("id").toString()];
            cur->pid = at.value("pid").toString();
            cur->pindex = at.hasAttribute("pindex") ? at.value("pindex").toInt() : -1;
        } else if (n == QLatin1String("vertex") && cur) {
            cur->verts.emplace_back(at.value("x").toDouble(), at.value("y").toDouble(), at.value("z").toDouble());
        } else if (n == QLatin1String("triangle") && cur) {
            Tri t;
            t.v[0] = at.value("v1").toInt();
            t.v[1] = at.value("v2").toInt();
            t.v[2] = at.value("v3").toInt();
            t.pid = at.value("pid").toString();
            if (at.hasAttribute("p1")) t.p = at.value("p1").toInt();
            cur->tris.push_back(t);
        } else if (n == QLatin1String("component") && cur) {
            cur->comps.push_back({at.value("objectid").toString(), parseXf(at.value("transform").toString())});
        } else if (n == QLatin1String("item")) {
            build.push_back({at.value("objectid").toString(), parseXf(at.value("transform").toString())});
        }
    }
    if (r.hasError()) return fail(error, QStringLiteral("The 3MF model could not be read: %1").arg(r.errorString()));

    ColorTable colors(out);
    std::function<void(const QString&, const Mat4&, int)> place = [&](const QString& id, const Mat4& xf, int depth) {
        auto it = objects.find(id);
        if (it == objects.end() || depth > 16) return;
        const Object& o = it->second;
        const int base = (int)out.verts.size();
        for (const Vec3& v : o.verts) out.verts.push_back(xf.transformPoint(v) * scale);
        for (const Tri& t : o.tris) {
            if (t.v[0] < 0 || t.v[1] < 0 || t.v[2] < 0 || t.v[0] >= (int)o.verts.size() || t.v[1] >= (int)o.verts.size() ||
                t.v[2] >= (int)o.verts.size())
                continue;
            out.tris.push_back({base + t.v[0], base + t.v[1], base + t.v[2]});
            const QString pid = t.pid.isEmpty() ? o.pid : t.pid;
            const int p = t.p >= 0 ? t.p : o.pindex;
            int c = -1;
            auto g = groups.find(pid);
            if (g != groups.end() && p >= 0 && p < (int)g->second.size()) c = colors.get(pid, p, g->second[p].first, g->second[p].second);
            out.triColor.push_back(c);
        }
        for (const Comp& c : o.comps) place(c.object, xf * c.xf, depth + 1);
    };
    if (build.empty())
        for (const auto& [id, o] : objects) build.push_back({id, Mat4()});
    for (const Comp& item : build) place(item.object, item.xf, 0);
    return true;
}

// ---- AMF ---------------------------------------------------------------------

bool readAmf(QByteArray bytes, PrintModel& out, QString* error) {
    if (bytes.startsWith("PK")) { // zipped AMF
        std::map<QString, QByteArray> files;
        if (!readZip(bytes, files, error) || files.empty()) return false;
        bytes = files.begin()->second;
    }
    QXmlStreamReader r(bytes);
    double scale = 1e-3;
    std::map<QString, std::pair<Color, std::string>> materials;
    QString curMat, volumeMat;
    Color color(0.8f, 0.8f, 0.8f);
    int objBase = 0;
    Vec3 v;
    int tri[3] = {0, 0, 0};
    ColorTable colors(out);
    std::string matName;
    bool inMaterial = false;
    while (!r.atEnd()) {
        const auto tok = r.readNext();
        const QString n = r.name().toString();
        if (tok == QXmlStreamReader::StartElement) {
            const auto at = r.attributes();
            if (n == QLatin1String("amf")) {
                const double m = unitMeters(at.hasAttribute("unit") ? at.value("unit").toString() : QStringLiteral("millimeter"));
                if (m > 0) scale = m;
            } else if (n == QLatin1String("material")) {
                inMaterial = true;
                curMat = at.value("id").toString();
                color = Color(0.8f, 0.8f, 0.8f);
                matName.clear();
            } else if (n == QLatin1String("metadata") && inMaterial && at.value("type") == QLatin1String("name")) {
                matName = r.readElementText().toStdString();
            } else if (inMaterial && (n == "r" || n == "g" || n == "b" || n == "a")) {
                const float x = (float)r.readElementText().toDouble();
                (n == "r" ? color.r : n == "g" ? color.g : n == "b" ? color.b : color.a) = x;
            } else if (n == QLatin1String("object")) {
                objBase = (int)out.verts.size();
            } else if (n == "x" || n == "y" || n == "z") {
                v[n == "x" ? 0 : n == "y" ? 1 : 2] = r.readElementText().toDouble();
            } else if (n == "v1" || n == "v2" || n == "v3") {
                tri[n == "v1" ? 0 : n == "v2" ? 1 : 2] = r.readElementText().toInt();
            } else if (n == QLatin1String("volume")) {
                volumeMat = at.value("materialid").toString();
            }
        } else if (tok == QXmlStreamReader::EndElement) {
            if (n == QLatin1String("material")) {
                materials[curMat] = {color, matName};
                inMaterial = false;
            } else if (n == QLatin1String("vertex")) {
                out.verts.push_back(v * scale);
            } else if (n == QLatin1String("triangle")) {
                out.tris.push_back({objBase + tri[0], objBase + tri[1], objBase + tri[2]});
                int c = -1;
                auto it = materials.find(volumeMat);
                if (it != materials.end()) c = colors.get(volumeMat, 0, it->second.first, it->second.second);
                out.triColor.push_back(c);
            }
        }
    }
    if (r.hasError()) return fail(error, QStringLiteral("The AMF file could not be read: %1").arg(r.errorString()));
    return true;
}

// ---- STL ---------------------------------------------------------------------

bool readStl(const QByteArray& b, double scale, PrintModel& out, QString* error) {
    uint32_t count = 0;
    if (b.size() >= 84) std::memcpy(&count, b.constData() + 80, 4);
    count = qFromLittleEndian(count);
    const bool binary = b.size() >= 84 && qint64(84) + qint64(count) * 50 == b.size();
    if (!binary) {
        if (!b.trimmed().startsWith("solid")) return fail(error, QStringLiteral("This is not an STL file."));
        int k = 0;
        for (const QByteArray& line : b.split('\n')) {
            const QByteArray t = line.trimmed();
            if (!t.startsWith("vertex")) continue;
            const QList<QByteArray> p = t.simplified().split(' ');
            if (p.size() < 4) continue;
            out.verts.emplace_back(p[1].toDouble() * scale, p[2].toDouble() * scale, p[3].toDouble() * scale);
            if (++k % 3 == 0) {
                const int n = (int)out.verts.size();
                out.tris.push_back({n - 3, n - 2, n - 1});
                out.triColor.push_back(-1);
            }
        }
        return true;
    }
    const char* d = b.constData() + 84;
    auto f = [&](int off) {
        uint32_t bits;
        std::memcpy(&bits, d + off, 4);
        bits = qFromLittleEndian(bits);
        float x;
        std::memcpy(&x, &bits, 4);
        return (double)x;
    };
    for (uint32_t i = 0; i < count; ++i, d += 50) {
        for (int k = 0; k < 3; ++k) out.verts.emplace_back(f(12 + k * 12) * scale, f(16 + k * 12) * scale, f(20 + k * 12) * scale);
        const int n = (int)out.verts.size();
        out.tris.push_back({n - 3, n - 2, n - 1});
        out.triColor.push_back(-1);
    }
    return true;
}

// ---- OFF ---------------------------------------------------------------------

bool readOff(const QByteArray& b, double scale, PrintModel& out, QString* error) {
    // Rows of words, comments taken out.
    std::vector<QList<QByteArray>> rows;
    for (const QByteArray& line : b.split('\n')) {
        const int hash = line.indexOf('#');
        const QByteArray l = (hash >= 0 ? line.left(hash) : line).simplified();
        if (!l.isEmpty()) rows.push_back(l.split(' '));
    }
    if (rows.empty() || !rows[0][0].endsWith("OFF")) return fail(error, QStringLiteral("This is not an OFF file."));
    // The counts follow "OFF" on its line or on the next one.
    size_t row = 1;
    QList<QByteArray> counts = rows[0].mid(1);
    if (counts.size() < 2) {
        if (rows.size() < 2) return fail(error, QStringLiteral("The OFF file is cut short."));
        counts = rows[1];
        row = 2;
    }
    const int nv = counts[0].toInt(), nf = counts.value(1).toInt();
    for (int k = 0; k < nv && row < rows.size(); ++k, ++row)
        out.verts.emplace_back(rows[row][0].toDouble() * scale, rows[row].value(1).toDouble() * scale, rows[row].value(2).toDouble() * scale);
    for (int k = 0; k < nf && row < rows.size(); ++k, ++row) {
        const int n = rows[row][0].toInt();
        if (n < 3 || rows[row].size() < n + 1) continue;
        for (int t = 1; t + 1 < n; ++t) { // a fan; flat faces are joined again later
            out.tris.push_back({rows[row][1].toInt(), rows[row][1 + t].toInt(), rows[row][2 + t].toInt()});
            out.triColor.push_back(-1);
        }
    }
    return true;
}

// ---- PLY ---------------------------------------------------------------------

bool readPly(const QByteArray& b, double scale, PrintModel& out, QString* error) {
    const int end = b.indexOf("end_header");
    if (!b.startsWith("ply") || end < 0) return fail(error, QStringLiteral("This is not a PLY file."));
    const int dataStart = b.indexOf('\n', end) + 1;
    struct Prop { QByteArray type, name, countType; bool list = false; };
    struct Element { QByteArray name; int count = 0; std::vector<Prop> props; };
    std::vector<Element> elements;
    enum { Ascii, LE, BE } format = Ascii;
    for (const QByteArray& line : b.left(end).split('\n')) {
        const QList<QByteArray> p = line.simplified().split(' ');
        if (p.isEmpty()) continue;
        if (p[0] == "format" && p.size() > 1) format = p[1] == "ascii" ? Ascii : p[1] == "binary_big_endian" ? BE : LE;
        else if (p[0] == "element" && p.size() > 2) elements.push_back({p[1], p[2].toInt(), {}});
        else if (p[0] == "property" && !elements.empty()) {
            if (p.size() > 4 && p[1] == "list") elements.back().props.push_back({p[3], p[4], p[2], true});
            else if (p.size() > 2) elements.back().props.push_back({p[1], p[2], {}, false});
        }
    }
    QList<QByteArray> words;
    int wi = 0;
    if (format == Ascii) words = b.mid(dataStart).simplified().split(' ');
    const char* ptr = b.constData() + dataStart;
    const char* stop = b.constData() + b.size();
    auto sizeOf = [](const QByteArray& t) {
        if (t == "char" || t == "uchar" || t == "int8" || t == "uint8") return 1;
        if (t == "short" || t == "ushort" || t == "int16" || t == "uint16") return 2;
        if (t == "double" || t == "float64") return 8;
        return 4;
    };
    bool ok = true;
    auto read = [&](const QByteArray& t) -> double {
        if (format == Ascii) {
            if (wi >= words.size()) { ok = false; return 0; }
            return words[wi++].toDouble();
        }
        const int n = sizeOf(t);
        if (ptr + n > stop) { ok = false; return 0; }
        unsigned char buf[8];
        std::memcpy(buf, ptr, n);
        ptr += n;
        if (format == BE) std::reverse(buf, buf + n);
        if (t == "float" || t == "float32") { float f; std::memcpy(&f, buf, 4); return f; }
        if (t == "double" || t == "float64") { double f; std::memcpy(&f, buf, 8); return f; }
        if (t == "char" || t == "int8") return (int8_t)buf[0];
        if (t == "uchar" || t == "uint8") return buf[0];
        if (t == "short" || t == "int16") { int16_t v; std::memcpy(&v, buf, 2); return v; }
        if (t == "ushort" || t == "uint16") { uint16_t v; std::memcpy(&v, buf, 2); return v; }
        if (t == "uint" || t == "uint32") { uint32_t v; std::memcpy(&v, buf, 4); return v; }
        int32_t v;
        std::memcpy(&v, buf, 4);
        return v;
    };
    ColorTable colors(out);
    for (const Element& el : elements) {
        const bool vertex = el.name == "vertex", face = el.name == "face";
        for (int k = 0; k < el.count && ok; ++k) {
            Vec3 p;
            std::vector<int> idx;
            int rgb[3] = {-1, -1, -1};
            for (const Prop& pr : el.props) {
                if (pr.list) {
                    const int n = (int)read(pr.countType);
                    for (int j = 0; j < n && ok; ++j) idx.push_back((int)read(pr.type));
                    continue;
                }
                const double x = read(pr.type);
                if (pr.name == "x") p.x = x;
                else if (pr.name == "y") p.y = x;
                else if (pr.name == "z") p.z = x;
                else if (pr.name == "red") rgb[0] = (int)x;
                else if (pr.name == "green") rgb[1] = (int)x;
                else if (pr.name == "blue") rgb[2] = (int)x;
            }
            if (vertex) out.verts.push_back(p * scale);
            if (face && idx.size() >= 3) {
                int c = -1;
                if (rgb[0] >= 0 && rgb[1] >= 0 && rgb[2] >= 0)
                    c = colors.get(QStringLiteral("ply"), (rgb[0] << 16) | (rgb[1] << 8) | rgb[2], Color::rgb8(rgb[0], rgb[1], rgb[2]), std::string());
                for (size_t t = 1; t + 1 < idx.size(); ++t) {
                    out.tris.push_back({idx[0], idx[t], idx[t + 1]});
                    out.triColor.push_back(c);
                }
            }
        }
    }
    if (!ok) return fail(error, QStringLiteral("The PLY file is cut short."));
    return true;
}

} // namespace

bool readZip(const QByteArray& zip, std::map<QString, QByteArray>& files, QString* error) {
    auto u16 = [&](qsizetype o) { return o + 2 <= zip.size() ? qFromLittleEndian<quint16>(zip.constData() + o) : quint16(0); };
    auto u32 = [&](qsizetype o) { return o + 4 <= zip.size() ? qFromLittleEndian<quint32>(zip.constData() + o) : quint32(0); };
    // End of central directory (searched from the back, past any comment).
    qsizetype eocd = -1;
    for (qsizetype o = zip.size() - 22; o >= 0 && o >= zip.size() - 22 - 65535; --o)
        if (u32(o) == 0x06054b50) { eocd = o; break; }
    if (eocd < 0) return fail(error, QStringLiteral("The file is not a ZIP archive."));
    const int count = u16(eocd + 10);
    qsizetype p = u32(eocd + 16);
    for (int i = 0; i < count; ++i) {
        if (u32(p) != 0x02014b50) return fail(error, QStringLiteral("The ZIP archive is damaged."));
        const int method = u16(p + 10);
        const quint32 csize = u32(p + 20), usize = u32(p + 24);
        const int nameLen = u16(p + 28), extraLen = u16(p + 30), commentLen = u16(p + 32);
        const quint32 local = u32(p + 42);
        const QString name = QString::fromUtf8(zip.mid(p + 46, nameLen));
        p += 46 + nameLen + extraLen + commentLen;
        if (u32(local) != 0x04034b50) continue;
        const qsizetype dataAt = local + 30 + u16(local + 26) + u16(local + 28);
        const QByteArray data = zip.mid(dataAt, csize);
        if (method == 0) {
            files[name] = data;
        } else if (method == 8) {
            QByteArray outBuf(qsizetype(usize), Qt::Uninitialized);
            z_stream zs{};
            if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) continue;
            zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData()));
            zs.avail_in = (uInt)data.size();
            zs.next_out = reinterpret_cast<Bytef*>(outBuf.data());
            zs.avail_out = (uInt)outBuf.size();
            const int rc = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            if (rc == Z_STREAM_END) files[name] = outBuf.left(qsizetype(zs.total_out));
        }
    }
    return true;
}

QString printImportFormat(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    for (const char* e : {"3mf", "amf", "stl", "ply", "off"})
        if (ext == QLatin1String(e)) return ext;
    return {};
}

bool printFileUnit(const QString& path, double& metersPerUnit) {
    const QString fmt = printImportFormat(path);
    if (fmt == QLatin1String("3mf") || fmt == QLatin1String("amf")) {
        metersPerUnit = 0; // said inside (default millimeters)
        return true;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(4096);
    if (fmt == QLatin1String("stl")) return taggedUnit(head.left(80), metersPerUnit) || taggedUnit(head.left(200), metersPerUnit);
    return taggedUnit(head, metersPerUnit);
}

bool readPrintFile(const QString& path, double metersPerUnit, PrintModel& out, QString* error) {
    out = PrintModel();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(error, f.errorString());
    const QByteArray bytes = f.readAll();
    const QString fmt = printImportFormat(path);
    double tagged;
    if (fmt != QLatin1String("3mf") && fmt != QLatin1String("amf") && printFileUnit(path, tagged)) metersPerUnit = tagged;
    bool ok = false;
    if (fmt == QLatin1String("3mf")) ok = read3mf(bytes, out, error);
    else if (fmt == QLatin1String("amf")) ok = readAmf(bytes, out, error);
    else if (fmt == QLatin1String("stl")) ok = readStl(bytes, metersPerUnit, out, error);
    else if (fmt == QLatin1String("ply")) ok = readPly(bytes, metersPerUnit, out, error);
    else if (fmt == QLatin1String("off")) ok = readOff(bytes, metersPerUnit, out, error);
    else return fail(error, QStringLiteral("Not a 3D printing file (3MF, AMF, STL, PLY, OFF)."));
    if (!ok) return false;
    // Drop triangles pointing at vertices that are not there.
    std::vector<std::array<int, 3>> tris;
    std::vector<int> cols;
    for (size_t i = 0; i < out.tris.size(); ++i) {
        bool good = true;
        for (int k : out.tris[i]) good = good && k >= 0 && k < (int)out.verts.size();
        if (!good) continue;
        tris.push_back(out.tris[i]);
        cols.push_back(i < out.triColor.size() ? out.triColor[i] : -1);
    }
    out.tris.swap(tris);
    out.triColor.swap(cols);
    if (out.tris.empty()) return fail(error, QStringLiteral("The file has no triangles."));
    return true;
}

Id importPrintModel(Document& doc, const PrintModel& model, const std::string& name, QString* error) {
    ModelState& st = doc.state;
    // Colours as materials (one per colour, reused by name if already there).
    std::vector<Id> matOf(model.colors.size(), kNone);
    for (size_t c = 0; c < model.colors.size(); ++c) {
        for (Id m = 0; m < (Id)st.materials.size(); ++m)
            if (st.materials[m].name == model.colorNames[c] && st.materials[m].color == model.colors[c]) matOf[c] = m;
        if (matOf[c] != kNone) continue;
        Material m;
        m.name = model.colorNames[c];
        m.color = model.colors[c];
        st.materials.push_back(m);
        matOf[c] = (Id)st.materials.size() - 1;
    }
    if (!model.colors.empty()) ++doc.materialsRevision;

    ComponentDef def;
    def.name = uniqueComponentName(st, name.empty() ? "Imported" : name);
    def.uid = newComponentUid();
    Mesh& m = def.mesh;
    std::vector<Id> vid(model.verts.size(), kNone);
    std::set<Id> faces;
    for (size_t i = 0; i < model.tris.size(); ++i) {
        const auto& t = model.tris[i];
        Id ids[3];
        for (int k = 0; k < 3; ++k) {
            if (vid[t[k]] == kNone) vid[t[k]] = m.addVertex(model.verts[t[k]]); // welds within the tolerance
            ids[k] = vid[t[k]];
        }
        if (ids[0] == ids[1] || ids[1] == ids[2] || ids[0] == ids[2]) continue;
        const Vec3 a = m.pos(ids[0]), b = m.pos(ids[1]), c = m.pos(ids[2]);
        const Vec3 n = (b - a).cross(c - a);
        if (n.length() < 1e-14) continue;
        const Id f = m.addFaceRaw({ids[0], ids[1], ids[2]}, {}, n);
        if (f == kNone) continue;
        const int col = i < model.triColor.size() ? model.triColor[i] : -1;
        if (col >= 0 && col < (int)matOf.size()) m.faces[f].front.material = m.faces[f].back.material = matOf[col];
        faces.insert(f);
    }
    if (faces.empty()) {
        if (error) *error = QStringLiteral("No faces could be made from the file.");
        return kNone;
    }
    // Flat neighbours become n-gons again; curved surfaces get smooth edges.
    effectMergeFlat(m, faces);
    for (Id e = 0; e < (Id)m.edges.size(); ++e) {
        if (!m.edges[e].alive) continue;
        const auto& fs = m.edgeFaces(e);
        if (fs.size() != 2) continue;
        if (m.faces[fs[0]].normal.dot(m.faces[fs[1]].normal) > 0.95 && m.faces[fs[0]].front.material == m.faces[fs[1]].front.material) {
            m.edges[e].soft = true;
            m.edges[e].smooth = true;
        }
    }
    m.removeDegenerateFaces();
    m.touch();
    st.defs.push_back(std::move(def));
    Instance in;
    in.def = (Id)st.defs.size() - 1;
    in.parent = st.activeDef();
    st.instances.push_back(in);
    return (Id)st.instances.size() - 1;
}

} // namespace gs
