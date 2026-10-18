// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImageUtil.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>

#include <atomic>

namespace gs {

static uint64_t nextRevision() {
    static std::atomic<uint64_t> r{1};
    return r++;
}

static void setWorldSize(Texture& t) {
    // Default tile: 1 m wide, height from the aspect ratio.
    t.worldWidth = 1.0;
    t.worldHeight = t.pixelWidth > 0 ? double(t.pixelHeight) / double(t.pixelWidth) : 1.0;
}

bool textureFromBytes(const QByteArray& bytes, Texture& out) {
    QBuffer buf;
    buf.setData(bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    QByteArray fmt = reader.format().toLower();
    QImage img = reader.read();
    if (img.isNull()) return false;
    if (fmt == "png" || fmt == "jpeg" || fmt == "jpg") {
        out.data = std::make_shared<const std::vector<uint8_t>>(bytes.begin(), bytes.end());
        out.mime = fmt == "png" ? "image/png" : "image/jpeg";
    } else {
        Texture t = textureFromImage(img);
        out.data = t.data;
        out.mime = t.mime;
    }
    out.pixelWidth = img.width();
    out.pixelHeight = img.height();
    out.revision = nextRevision();
    setWorldSize(out);
    return true;
}

bool loadTextureFile(const QString& path, Texture& out, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Cannot open %1").arg(path);
        return false;
    }
    if (!textureFromBytes(f.readAll(), out)) {
        if (error) *error = QStringLiteral("Unsupported or corrupt image: %1").arg(path);
        return false;
    }
    return true;
}

Texture textureFromImage(const QImage& img) {
    Texture t;
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    t.data = std::make_shared<const std::vector<uint8_t>>(bytes.begin(), bytes.end());
    t.mime = "image/png";
    t.pixelWidth = img.width();
    t.pixelHeight = img.height();
    t.revision = nextRevision();
    setWorldSize(t);
    return t;
}

QImage decodeTexture(const Texture& t) {
    if (!t.valid()) return QImage();
    return QImage::fromData(reinterpret_cast<const uchar*>(t.data->data()), (int)t.data->size());
}

Color averageColor(const QImage& imgIn) {
    if (imgIn.isNull()) return Color();
    QImage img = imgIn.scaled(16, 16, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                     .convertToFormat(QImage::Format_RGBA8888);
    double r = 0, g = 0, b = 0;
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            QColor c = img.pixelColor(x, y);
            r += c.redF(); g += c.greenF(); b += c.blueF();
            ++n;
        }
    if (!n) return Color();
    return Color(float(r / n), float(g / n), float(b / n), 1.0f);
}

ScalarImage scalarImage(const QImage& imgIn, TraceSource src, int maxSide) {
    QImage img = imgIn;
    if (img.width() > maxSide || img.height() > maxSide)
        img = img.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    img = img.convertToFormat(QImage::Format_RGBA8888);
    ScalarImage s;
    s.width = img.width();
    s.height = img.height();
    s.values.resize(size_t(s.width) * s.height);
    for (int y = 0; y < s.height; ++y) {
        const uchar* row = img.constScanLine(y);
        for (int x = 0; x < s.width; ++x) {
            const uchar* p = row + 4 * x;
            float a = p[3] / 255.f;
            // Transparent pixels count as white paper for the brightness modes.
            float lum = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.f;
            lum = lum * a + (1 - a);
            float v = src == TraceSource::Alpha ? a : (src == TraceSource::Dark ? 1 - lum : lum);
            s.values[size_t(y) * s.width + x] = v;
        }
    }
    return s;
}

bool hasTransparency(const QImage& imgIn) {
    if (!imgIn.hasAlphaChannel()) return false;
    QImage img = imgIn.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < img.height(); ++y) {
        const uchar* row = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x)
            if (row[4 * x + 3] < 250) return true;
    }
    return false;
}

bool isImageFile(const QString& path) {
    QString s = QFileInfo(path).suffix().toLower();
    static const QStringList exts = {"png", "jpg", "jpeg", "bmp", "gif", "webp", "tga", "tif", "tiff", "ppm", "avif", "jxl", "heic"};
    return exts.contains(s);
}

} // namespace gs
