// SPDX-License-Identifier: GPL-3.0-or-later
#include "PrintExport.h"

#include "GltfExport.h"
#include "core/Components.h"

#include <QSaveFile>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <map>

namespace gs {

PrintMesh buildPrintMesh(const Document& doc, LengthUnit unit) {
    const Mesh m = flattenModel(doc.state);
    const double scale = 1.0 / Units::toMeters(unit);
    PrintMesh pm;
    std::vector<int> vmap(m.vertices.size(), -1);
    std::map<Id, int> matIndex;
    auto vertexIndex = [&](Id v) {
        if (vmap[v] < 0) {
            vmap[v] = (int)pm.verts.size();
            pm.verts.push_back(m.pos(v) * scale);
        }
        return vmap[v];
    };
    for (Id f = 0; f < (Id)m.faces.size(); ++f) {
        const Face& F = m.faces[f];
        if (!F.alive || F.hidden) continue;
        // Loop vertex ids in the order triangulateFace() lists positions.
        std::vector<Id> ids(F.outer);
        for (const auto& h : F.holes) ids.insert(ids.end(), h.begin(), h.end());
        const FaceTriangles ft = triangulateFace(m, f, true, nullptr);
        int mat = -1;
        Id mid = F.front.material != kNone ? F.front.material : F.back.material;
        if (const Material* mm = doc.material(mid)) {
            auto it = matIndex.find(mid);
            if (it == matIndex.end()) {
                it = matIndex.emplace(mid, (int)pm.colors.size()).first;
                pm.colors.push_back(mm->color);
                pm.colorNames.push_back(mm->name.empty() ? "Material " + std::to_string(pm.colors.size()) : mm->name);
            }
            mat = it->second;
        }
        for (const auto& t : ft.tris) {
            std::array<int, 3> tri{vertexIndex(ids[t[0]]), vertexIndex(ids[t[1]]), vertexIndex(ids[t[2]])};
            if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) continue;
            const Vec3 n = (pm.verts[tri[1]] - pm.verts[tri[0]]).cross(pm.verts[tri[2]] - pm.verts[tri[0]]);
            if (n.dot(F.normal) < 0) std::swap(tri[1], tri[2]);
            pm.tris.push_back(tri);
            pm.triMaterial.push_back(mat);
        }
    }
    return pm;
}

const char* printFormatName(PrintFormat f) {
    switch (f) {
    case PrintFormat::ThreeMF: return "3D Manufacturing Format";
    case PrintFormat::Amf: return "Additive Manufacturing File";
    case PrintFormat::StlBinary: return "STL (binary)";
    case PrintFormat::StlAscii: return "STL (ASCII)";
    case PrintFormat::Ply: return "Stanford PLY";
    case PrintFormat::Off: return "Object File Format";
    }
    return "";
}

const char* printFormatExtension(PrintFormat f) {
    switch (f) {
    case PrintFormat::ThreeMF: return "3mf";
    case PrintFormat::Amf: return "amf";
    case PrintFormat::StlBinary:
    case PrintFormat::StlAscii: return "stl";
    case PrintFormat::Ply: return "ply";
    case PrintFormat::Off: return "off";
    }
    return "";
}

LengthUnit printUnitFor(PrintFormat f, LengthUnit wanted) {
    if (f == PrintFormat::Amf && wanted == LengthUnit::Centimeters) return LengthUnit::Millimeters;
    return wanted;
}

const char* printUnitName(PrintFormat f, LengthUnit u) {
    u = printUnitFor(f, u);
    switch (u) {
    case LengthUnit::Millimeters: return "millimeter";
    case LengthUnit::Centimeters: return "centimeter";
    case LengthUnit::Meters: return "meter";
    case LengthUnit::Inches: return "inch";
    case LengthUnit::Feet: return f == PrintFormat::Amf ? "feet" : "foot";
    }
    return "millimeter";
}

namespace {

// Unit tag for the unitless formats' header/comment (no quote characters).
QByteArray unitTag(LengthUnit u) {
    switch (u) {
    case LengthUnit::Millimeters: return "mm";
    case LengthUnit::Centimeters: return "cm";
    case LengthUnit::Meters: return "m";
    case LengthUnit::Inches: return "in";
    case LengthUnit::Feet: return "ft";
    }
    return "mm";
}

QByteArray num(double v) {
    if (std::abs(v) < 1e-12) v = 0;
    return QByteArray::number(v, 'g', 9);
}

QByteArray hexColor(const Color& c) {
    auto b = [](float v) { return std::clamp(int(std::lround(v * 255.f)), 0, 255); };
    return QStringLiteral("#%1%2%3%4")
        .arg(b(c.r), 2, 16, QLatin1Char('0'))
        .arg(b(c.g), 2, 16, QLatin1Char('0'))
        .arg(b(c.b), 2, 16, QLatin1Char('0'))
        .arg(b(c.a), 2, 16, QLatin1Char('0'))
        .toUpper()
        .toLatin1();
}

QByteArray xmlEscape(const std::string& s) { return QString::fromStdString(s).toHtmlEscaped().toUtf8(); }

uint32_t crc32(const QByteArray& data) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (char ch : data) c = table[(c ^ uint8_t(ch)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

QByteArray build3mf(const PrintMesh& pm, const char* unit) {
    QByteArray model;
    model.reserve(int(pm.verts.size() * 60 + pm.tris.size() * 50 + 1024));
    model += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    model += "<model unit=\"" + QByteArray(unit) + "\" xml:lang=\"en-US\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n";
    model += " <metadata name=\"Application\">GNUSketch</metadata>\n";
    model += " <resources>\n";
    const bool colored = !pm.colors.empty();
    if (colored) {
        model += "  <basematerials id=\"1\">\n";
        for (size_t i = 0; i < pm.colors.size(); ++i)
            model += "   <base name=\"" + xmlEscape(pm.colorNames[i]) + "\" displaycolor=\"" + hexColor(pm.colors[i]) + "\"/>\n";
        model += "  </basematerials>\n";
    }
    const char* objId = colored ? "2" : "1";
    model += QByteArray("  <object id=\"") + objId + "\" type=\"model\"";
    // Triangles without a material use the first one (3MF needs a default when pid is set).
    if (colored) model += " pid=\"1\" pindex=\"0\"";
    model += ">\n   <mesh>\n    <vertices>\n";
    for (const Vec3& v : pm.verts) model += "     <vertex x=\"" + num(v.x) + "\" y=\"" + num(v.y) + "\" z=\"" + num(v.z) + "\"/>\n";
    model += "    </vertices>\n    <triangles>\n";
    for (size_t i = 0; i < pm.tris.size(); ++i) {
        const auto& t = pm.tris[i];
        model += "     <triangle v1=\"" + QByteArray::number(t[0]) + "\" v2=\"" + QByteArray::number(t[1]) + "\" v3=\"" + QByteArray::number(t[2]) + "\"";
        if (colored && pm.triMaterial[i] >= 0) model += " pid=\"1\" p1=\"" + QByteArray::number(pm.triMaterial[i]) + "\"";
        model += "/>\n";
    }
    model += "    </triangles>\n   </mesh>\n  </object>\n </resources>\n";
    model += QByteArray(" <build>\n  <item objectid=\"") + objId + "\"/>\n </build>\n</model>\n";

    const QByteArray types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
        " <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
        " <Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>\n"
        "</Types>\n";
    const QByteArray rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
        " <Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>\n"
        "</Relationships>\n";
    return makeZip({{QStringLiteral("[Content_Types].xml"), types}, {QStringLiteral("_rels/.rels"), rels}, {QStringLiteral("3D/3dmodel.model"), model}});
}

QByteArray buildAmf(const PrintMesh& pm, const char* unit) {
    QByteArray out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<amf unit=\"" + QByteArray(unit) + "\" version=\"1.1\">\n";
    out += " <metadata type=\"producer\">GNUSketch</metadata>\n";
    for (size_t i = 0; i < pm.colors.size(); ++i) {
        const Color& c = pm.colors[i];
        out += " <material id=\"" + QByteArray::number(int(i) + 1) + "\">\n  <metadata type=\"name\">" + xmlEscape(pm.colorNames[i]) +
               "</metadata>\n  <color><r>" + num(c.r) + "</r><g>" + num(c.g) + "</g><b>" + num(c.b) + "</b><a>" + num(c.a) + "</a></color>\n </material>\n";
    }
    out += " <object id=\"0\">\n  <mesh>\n   <vertices>\n";
    for (const Vec3& v : pm.verts)
        out += "    <vertex><coordinates><x>" + num(v.x) + "</x><y>" + num(v.y) + "</y><z>" + num(v.z) + "</z></coordinates></vertex>\n";
    out += "   </vertices>\n";
    // One volume per material (AMF colours whole volumes).
    std::map<int, std::vector<size_t>> byMat;
    for (size_t i = 0; i < pm.tris.size(); ++i) byMat[pm.triMaterial[i]].push_back(i);
    for (const auto& [mat, list] : byMat) {
        out += mat >= 0 ? "   <volume materialid=\"" + QByteArray::number(mat + 1) + "\">\n" : QByteArray("   <volume>\n");
        for (size_t i : list) {
            const auto& t = pm.tris[i];
            out += "    <triangle><v1>" + QByteArray::number(t[0]) + "</v1><v2>" + QByteArray::number(t[1]) + "</v2><v3>" + QByteArray::number(t[2]) + "</v3></triangle>\n";
        }
        out += "   </volume>\n";
    }
    out += "  </mesh>\n </object>\n</amf>\n";
    return out;
}

Vec3 triNormal(const PrintMesh& pm, const std::array<int, 3>& t) {
    Vec3 n = (pm.verts[t[1]] - pm.verts[t[0]]).cross(pm.verts[t[2]] - pm.verts[t[0]]);
    const double l = n.length();
    return l > 0 ? n * (1.0 / l) : Vec3(0, 0, 1);
}

QByteArray buildStlBinary(const PrintMesh& pm, LengthUnit u) {
    QByteArray out(80, '\0');
    const QByteArray head = "GNUSketch binary STL, units=" + unitTag(u);
    std::memcpy(out.data(), head.constData(), std::min<qsizetype>(head.size(), 80));
    const uint32_t count = qToLittleEndian<uint32_t>((uint32_t)pm.tris.size());
    out.append(reinterpret_cast<const char*>(&count), 4);
    out.reserve(84 + int(pm.tris.size()) * 50);
    auto putF = [&](double v) {
        const float f = (float)v;
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        bits = qToLittleEndian(bits);
        out.append(reinterpret_cast<const char*>(&bits), 4);
    };
    for (const auto& t : pm.tris) {
        const Vec3 n = triNormal(pm, t);
        putF(n.x); putF(n.y); putF(n.z);
        for (int k : t) { putF(pm.verts[k].x); putF(pm.verts[k].y); putF(pm.verts[k].z); }
        out.append(2, '\0');
    }
    return out;
}

QByteArray buildStlAscii(const PrintMesh& pm, LengthUnit u) {
    QByteArray out = "solid gnusketch_units_" + unitTag(u) + "\n";
    for (const auto& t : pm.tris) {
        const Vec3 n = triNormal(pm, t);
        out += "  facet normal " + num(n.x) + " " + num(n.y) + " " + num(n.z) + "\n    outer loop\n";
        for (int k : t) out += "      vertex " + num(pm.verts[k].x) + " " + num(pm.verts[k].y) + " " + num(pm.verts[k].z) + "\n";
        out += "    endloop\n  endfacet\n";
    }
    out += "endsolid gnusketch_units_" + unitTag(u) + "\n";
    return out;
}

QByteArray buildPly(const PrintMesh& pm, LengthUnit u) {
    const bool colored = !pm.colors.empty();
    QByteArray out = "ply\nformat ascii 1.0\ncomment GNUSketch export\ncomment units " + unitTag(u) + "\n";
    out += "element vertex " + QByteArray::number((qsizetype)pm.verts.size()) + "\nproperty float x\nproperty float y\nproperty float z\n";
    out += "element face " + QByteArray::number((qsizetype)pm.tris.size()) + "\nproperty list uchar int vertex_indices\n";
    if (colored) out += "property uchar red\nproperty uchar green\nproperty uchar blue\n";
    out += "end_header\n";
    for (const Vec3& v : pm.verts) out += num(v.x) + " " + num(v.y) + " " + num(v.z) + "\n";
    auto b = [](float v) { return QByteArray::number(std::clamp(int(std::lround(v * 255.f)), 0, 255)); };
    for (size_t i = 0; i < pm.tris.size(); ++i) {
        const auto& t = pm.tris[i];
        out += "3 " + QByteArray::number(t[0]) + " " + QByteArray::number(t[1]) + " " + QByteArray::number(t[2]);
        if (colored) {
            const Color c = pm.triMaterial[i] >= 0 ? pm.colors[pm.triMaterial[i]] : Color::rgb8(200, 200, 200);
            out += " " + b(c.r) + " " + b(c.g) + " " + b(c.b);
        }
        out += "\n";
    }
    return out;
}

QByteArray buildOff(const PrintMesh& pm, LengthUnit u) {
    QByteArray out = "OFF\n# GNUSketch export, units " + unitTag(u) + "\n";
    out += QByteArray::number((qsizetype)pm.verts.size()) + " " + QByteArray::number((qsizetype)pm.tris.size()) + " 0\n";
    for (const Vec3& v : pm.verts) out += num(v.x) + " " + num(v.y) + " " + num(v.z) + "\n";
    for (const auto& t : pm.tris) out += "3 " + QByteArray::number(t[0]) + " " + QByteArray::number(t[1]) + " " + QByteArray::number(t[2]) + "\n";
    return out;
}

} // namespace

QByteArray makeZip(const std::vector<std::pair<QString, QByteArray>>& files) {
    QByteArray out, central;
    auto u16 = [](QByteArray& b, uint16_t v) { v = qToLittleEndian(v); b.append(reinterpret_cast<const char*>(&v), 2); };
    auto u32 = [](QByteArray& b, uint32_t v) { v = qToLittleEndian(v); b.append(reinterpret_cast<const char*>(&v), 4); };
    for (const auto& [name, data] : files) {
        const QByteArray fname = name.toUtf8();
        const uint32_t crc = crc32(data);
        // qCompress = 4-byte length + zlib stream (2-byte header ... 4-byte Adler-32).
        QByteArray z = qCompress(data, 9);
        QByteArray deflated = z.size() > 10 ? z.mid(6, z.size() - 10) : QByteArray();
        uint16_t method = 8;
        if (deflated.isEmpty() || deflated.size() >= data.size()) { deflated = data; method = 0; }
        const uint32_t offset = (uint32_t)out.size();
        u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, method); u16(out, 0); u16(out, 0x21);
        u32(out, crc); u32(out, (uint32_t)deflated.size()); u32(out, (uint32_t)data.size());
        u16(out, (uint16_t)fname.size()); u16(out, 0);
        out += fname;
        out += deflated;
        u32(central, 0x02014b50); u16(central, 20); u16(central, 20); u16(central, 0); u16(central, method); u16(central, 0); u16(central, 0x21);
        u32(central, crc); u32(central, (uint32_t)deflated.size()); u32(central, (uint32_t)data.size());
        u16(central, (uint16_t)fname.size()); u16(central, 0); u16(central, 0); u16(central, 0); u16(central, 0); u32(central, 0);
        u32(central, offset);
        central += fname;
    }
    const uint32_t cdOffset = (uint32_t)out.size();
    out += central;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0);
    u16(out, (uint16_t)files.size()); u16(out, (uint16_t)files.size());
    u32(out, (uint32_t)central.size()); u32(out, cdOffset); u16(out, 0);
    return out;
}

bool exportPrint(const Document& doc, const QString& path, PrintFormat fmt, LengthUnit unit, QString* error) {
    unit = printUnitFor(fmt, unit);
    const PrintMesh pm = buildPrintMesh(doc, unit);
    if (pm.tris.empty()) {
        if (error) *error = QStringLiteral("There are no visible faces to export.");
        return false;
    }
    QByteArray bytes;
    switch (fmt) {
    case PrintFormat::ThreeMF: bytes = build3mf(pm, printUnitName(fmt, unit)); break;
    case PrintFormat::Amf: bytes = buildAmf(pm, printUnitName(fmt, unit)); break;
    case PrintFormat::StlBinary: bytes = buildStlBinary(pm, unit); break;
    case PrintFormat::StlAscii: bytes = buildStlAscii(pm, unit); break;
    case PrintFormat::Ply: bytes = buildPly(pm, unit); break;
    case PrintFormat::Off: bytes = buildOff(pm, unit); break;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(bytes);
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

} // namespace gs
