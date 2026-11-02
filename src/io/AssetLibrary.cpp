// SPDX-License-Identifier: GPL-3.0-or-later
#include "AssetLibrary.h"

#include "ImageUtil.h"
#include "NativeFormat.h"
#include "core/Components.h"

#include <QBuffer>
#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace gs {

namespace {

// A file name from a free-form name: no path separators or control characters.
QString safeFileName(const QString& name) {
    QString n = name.trimmed();
    n.replace(QRegularExpression(QStringLiteral(R"([/\\:*?"<>|\x00-\x1f])")), QStringLiteral("_"));
    while (n.startsWith('.')) n.remove(0, 1);
    return n.isEmpty() ? QStringLiteral("Unnamed") : n.left(120);
}

QStringList filesIn(const QStringList& dirs, const QStringList& filters) {
    QStringList out;
    for (const QString& d : dirs) {
        QDir dir(d);
        for (const QString& f : dir.entryList(filters, QDir::Files)) out << dir.filePath(f);
    }
    QCollator col;
    col.setCaseSensitivity(Qt::CaseInsensitive);
    col.setNumericMode(true);
    std::sort(out.begin(), out.end(), [&](const QString& a, const QString& b) {
        return col.compare(QFileInfo(a).fileName(), QFileInfo(b).fileName()) < 0;
    });
    return out;
}

} // namespace

QString assetDir(const QString& kind) {
    const QString root = qEnvironmentVariableIsSet("GNUSKETCH_DATA_DIR") ? qEnvironmentVariable("GNUSKETCH_DATA_DIR")
                                                                         : QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath(QStringLiteral("GNUSketch"));
    const QString path = QDir(root).filePath(kind);
    QDir().mkpath(path);
    return path;
}

QStringList assetDirs(const QString& kind) {
    QStringList dirs{assetDir(kind)};
    if (qEnvironmentVariableIsSet("GNUSKETCH_DATA_DIR")) return dirs;
    for (const QString& d : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        const QString p = QDir(d).filePath(QStringLiteral("GNUSketch/") + kind);
        if (!dirs.contains(p) && QDir(p).exists()) dirs << p;
    }
    return dirs;
}

QString savePrefab(const ModelState& state, Id def, const QString& name, const QImage& thumbnail, QString* error, const EntityProps& entity) {
    ModelState prefab = extractDefinition(state, def);
    if (prefab.defs.empty()) {
        if (error) *error = QStringLiteral("There is no such component.");
        return {};
    }
    prefab.defs[0].name = name.toStdString();
    prefab.instances[0].entity = entity;
    QJsonObject view;
    view["prefab"] = true;
    if (!thumbnail.isNull()) {
        QByteArray bytes;
        QBuffer buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        thumbnail.save(&buf, "PNG");
        view["thumbnail"] = QString::fromLatin1(bytes.toBase64());
    }
    const QDir dir(assetDir(QStringLiteral("prefabs")));
    const QString base = safeFileName(name);
    QString path = dir.filePath(base + QStringLiteral(".gsketch"));
    for (int i = 2; QFileInfo::exists(path); ++i) path = dir.filePath(QStringLiteral("%1 %2.gsketch").arg(base).arg(i));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return {};
    }
    f.write(modelToJson(prefab, view));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return {};
    }
    return path;
}

bool loadPrefab(const QString& path, ModelState& out, QString* name, QImage* thumbnail, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    QJsonObject view;
    ModelState st;
    if (!modelFromJson(f.readAll(), st, &view, error)) return false;
    // A model that was saved by hand into the folder: its loose geometry and
    // components together make up the prefab.
    int roots = 0;
    for (const Instance& in : st.instances) roots += in.alive && in.parent == kNone;
    const bool isPrefab = roots == 1 && !st.instances.empty() && st.instances[0].parent == kNone && st.instances[0].def == 0;
    if (!isPrefab || st.mesh.aliveFaceCount() > 0 || st.mesh.aliveEdgeCount() > 0) {
        ComponentDef d;
        d.name = QFileInfo(path).completeBaseName().toStdString();
        d.uid = "file:" + QFileInfo(path).fileName().toStdString(); // stays linked when placed again
        d.mesh = flattenModel(st);
        if (d.mesh.aliveEdgeCount() == 0) {
            if (error) *error = QStringLiteral("%1 has no geometry.").arg(QFileInfo(path).fileName());
            return false;
        }
        Vec3 mn, mx;
        bool any = false;
        d.mesh.bounds(mn, mx, any);
        Mesh shifted;
        appendMesh(shifted, d.mesh, Mat4::translation(Vec3() - mn));
        d.mesh = std::move(shifted);
        st.mesh = Mesh();
        st.defs = {std::move(d)};
        Instance in;
        in.def = 0;
        st.instances = {in};
    }
    st.objects.clear();
    st.guides.clear();
    if (name) *name = QString::fromStdString(st.defs[0].name);
    if (thumbnail) {
        const QByteArray b = QByteArray::fromBase64(view["thumbnail"].toString().toLatin1());
        *thumbnail = b.isEmpty() ? QImage() : QImage::fromData(b, "PNG");
    }
    out = std::move(st);
    return true;
}

QStringList prefabFiles() { return filesIn(assetDirs(QStringLiteral("prefabs")), {QStringLiteral("*.gsketch")}); }

QString saveLibraryMaterial(const Material& m, QString* error) {
    const QString path = QDir(assetDir(QStringLiteral("materials"))).filePath(safeFileName(QString::fromStdString(m.name)) + QStringLiteral(".gsmat"));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return {};
    }
    QJsonObject root = materialToJson(m);
    root["format"] = "GNUSketch Material";
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return {};
    }
    return path;
}

QStringList libraryMaterialFiles() { return filesIn(assetDirs(QStringLiteral("materials")), {QStringLiteral("*.gsmat")}); }

bool loadLibraryMaterial(const QString& path, Material& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll());
    if (!d.isObject() || d.object()["format"].toString() != QLatin1String("GNUSketch Material")) return false;
    out = materialFromJson(d.object());
    return true;
}

std::vector<Material> builtinPbrMaterials() {
    struct P { const char* name; Color c; float metal, rough; };
    const P presets[] = {{"Gold", Color::rgb8(255, 200, 90), 1, 0.25f},       {"Silver", Color::rgb8(240, 240, 236), 1, 0.2f},
                         {"Copper", Color::rgb8(240, 150, 110), 1, 0.3f},     {"Polished Steel", Color::rgb8(190, 192, 195), 1, 0.15f},
                         {"Rough Iron", Color::rgb8(120, 118, 115), 1, 0.75f}, {"Red Plastic", Color::rgb8(200, 30, 30), 0, 0.3f},
                         {"White Plastic", Color::rgb8(235, 235, 235), 0, 0.35f}, {"Black Rubber", Color::rgb8(30, 30, 30), 0, 0.9f},
                         {"Glossy Paint", Color::rgb8(40, 90, 180), 0, 0.12f},  {"Matte Clay", Color::rgb8(190, 150, 120), 0, 1.0f}};
    std::vector<Material> out;
    for (const P& p : presets) {
        Material m;
        m.name = p.name;
        m.color = p.c;
        m.pbr = true;
        m.maps[MapMetallic].strength = p.metal;
        m.maps[MapRoughness].strength = p.rough;
        out.push_back(std::move(m));
    }
    Material glow;
    glow.name = "Warm Glow";
    glow.color = Color::rgb8(40, 30, 20);
    glow.pbr = true;
    glow.emission = Color::rgb8(255, 190, 110);
    out.push_back(std::move(glow));
    return out;
}

QStringList textureFiles(const QStringList& extraFolders) {
    static const QStringList kFilters = {QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.webp"),
                                         QStringLiteral("*.bmp"), QStringLiteral("*.tga"), QStringLiteral("*.tif"), QStringLiteral("*.tiff")};
    return filesIn(assetDirs(QStringLiteral("textures")) + extraFolders, kFilters);
}

bool materialFromImages(const QStringList& paths, Material& out, QString* error) {
    Material m;
    QString firstName;
    bool any = false;
    for (const QString& path : paths) {
        Texture t;
        if (!loadTextureFile(path, t, error)) continue;
        const int slot = guessMapSlot(QFileInfo(path).fileName().toStdString());
        if (slot < 0 || slot == MapSlotCount) {
            if (slot == MapSlotCount || !m.hasTexture()) {
                if (firstName.isEmpty()) firstName = QFileInfo(path).completeBaseName();
                m.texture = t;
                m.color = averageColor(decodeTexture(t));
            }
        } else {
            m.maps[slot].tex = t;
            if (slot == MapMetallic || slot == MapRoughness) m.maps[slot].strength = 1;
            if (slot == MapEmission) m.emission = Color(1, 1, 1);
        }
        any = true;
    }
    if (!any) {
        if (error && error->isEmpty()) *error = QStringLiteral("No usable image.");
        return false;
    }
    // The common stem of the names, without the map's own word ("oak_Roughness" -> "oak").
    QString stem = firstName.isEmpty() ? QFileInfo(paths.first()).completeBaseName() : firstName;
    stem.remove(QRegularExpression(QStringLiteral(R"([-_. ]*(albedo|basecolor|base|diffuse|diff|colou?r|col|normals?|nor|nrm|metallic|metalness|metal|roughness|rough|ao|occlusion|height|displacement|disp|bump|emissive|emission|opacity|alpha)([-_. ]*\d*k?)?$)"),
                                     QRegularExpression::CaseInsensitiveOption));
    m.name = (stem.isEmpty() ? QStringLiteral("Material") : stem).toStdString();
    m.pbr = true;
    // Maps follow the base image's tile size, or the 1 m default.
    double w = 1, h = 1;
    m.tileSize(w, h);
    m.setTileSize(w, h);
    out = std::move(m);
    return true;
}

} // namespace gs
