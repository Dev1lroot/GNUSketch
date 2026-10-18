// SPDX-License-Identifier: GPL-3.0-or-later
#include "StlExport.h"

#include "GltfExport.h"

#include <QSaveFile>

#include <cstring>

namespace gs {

bool exportStl(const Document& doc, const QString& path, double scale, QString* error) {
    const Mesh& m = doc.mesh();
    QByteArray out(80, '\0');
    std::memcpy(out.data(), "GNUSketch binary STL", 20);
    QByteArray body;
    uint32_t count = 0;
    auto putF = [&](float v) { body.append(reinterpret_cast<const char*>(&v), 4); };
    for (Id f = 0; f < (Id)m.faces.size(); ++f) {
        if (!m.faces[f].alive) continue;
        FaceTriangles ft = triangulateFace(m, f, true, nullptr);
        const Vec3 n = m.faces[f].normal;
        for (const auto& t : ft.tris) {
            putF((float)n.x); putF((float)n.y); putF((float)n.z);
            for (int k : t) {
                const Vec3 p = ft.pos[k] * scale;
                putF((float)p.x); putF((float)p.y); putF((float)p.z);
            }
            body.append(2, '\0');
            ++count;
        }
    }
    out.append(reinterpret_cast<const char*>(&count), 4);
    out.append(body);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(out);
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

} // namespace gs
