// SPDX-License-Identifier: GPL-3.0-or-later
#include "Primitives.h"

#include <algorithm>
#include <cmath>

namespace gs {

namespace {

// One face of the primitive before it goes into a mesh.
struct Poly {
    std::vector<Vec3> pts;
    Vec3 out;          // any vector pointing to the outside of the face
    int group = -1;    // faces of one curved surface share a group (edges between them get softened)
    bool curve = false; // its outline is a circle (a cylinder or cone cap)
};

Vec3 centroid(const std::vector<Vec3>& pts) {
    Vec3 c;
    for (const Vec3& p : pts) c += p;
    return pts.empty() ? c : c / double(pts.size());
}

// Faces of the convex hull of a small, symmetric point set: every plane through
// three points with all points on one side, and all points lying on it.
std::vector<Poly> convexHull(const std::vector<Vec3>& pts) {
    const double eps = 1e-9;
    std::vector<Poly> polys;
    std::vector<Plane> seen;
    const size_t n = pts.size();
    for (size_t a = 0; a < n; ++a)
        for (size_t b = a + 1; b < n; ++b)
            for (size_t c = b + 1; c < n; ++c) {
                Vec3 nrm = (pts[b] - pts[a]).cross(pts[c] - pts[a]);
                if (nrm.length() < eps) continue;
                Plane P(nrm, pts[a]);
                int above = 0, below = 0;
                for (const Vec3& q : pts) {
                    const double d = P.distance(q);
                    if (d > eps) ++above;
                    else if (d < -eps) ++below;
                }
                if (above && below) continue;
                if (above) P = Plane(-nrm, pts[a]); // normal points away from the rest
                bool dup = false;
                for (const Plane& s : seen)
                    if (s.n.dot(P.n) > 1 - 1e-9 && std::fabs(s.d - P.d) < 1e-7) dup = true;
                if (dup) continue;
                seen.push_back(P);
                Poly poly;
                for (const Vec3& q : pts)
                    if (std::fabs(P.distance(q)) <= eps) poly.pts.push_back(q);
                // Order the face's points around its centre.
                const Vec3 mid = centroid(poly.pts);
                Vec3 u, v;
                planeBasis(P.n, u, v);
                auto angle = [&](const Vec3& q) { return std::atan2((q - mid).dot(v), (q - mid).dot(u)); };
                std::sort(poly.pts.begin(), poly.pts.end(),
                          [&](const Vec3& q1, const Vec3& q2) { return angle(q1) < angle(q2); });
                poly.out = P.n;
                polys.push_back(std::move(poly));
            }
    return polys;
}

std::vector<Vec3> scaledTo(std::vector<Vec3> pts, double radius) {
    for (Vec3& p : pts) p = p.normalized() * radius;
    return pts;
}

const double kPhi = (1 + std::sqrt(5.0)) / 2;

std::vector<Vec3> icosahedronPoints() {
    std::vector<Vec3> pts;
    for (int s1 : {-1, 1})
        for (int s2 : {-1, 1}) {
            pts.push_back({0, double(s1), s2 * kPhi});
            pts.push_back({double(s1), s2 * kPhi, 0});
            pts.push_back({s1 * kPhi, 0, double(s2)});
        }
    return pts;
}

std::vector<Vec3> platonicPoints(PrimitiveKind k) {
    std::vector<Vec3> pts;
    switch (k) {
    case PrimitiveKind::Tetrahedron:
        pts = {{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}};
        break;
    case PrimitiveKind::Octahedron:
        pts = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        break;
    case PrimitiveKind::Dodecahedron: {
        const double ip = 1 / kPhi;
        for (int x : {-1, 1})
            for (int y : {-1, 1}) {
                for (int z : {-1, 1}) pts.push_back({double(x), double(y), double(z)});
                pts.push_back({0, x * ip, y * kPhi});
                pts.push_back({x * ip, y * kPhi, 0});
                pts.push_back({x * kPhi, 0, y * ip});
            }
        break;
    }
    default:
        pts = icosahedronPoints();
        break;
    }
    return pts;
}

// Turns the polygons so that the face pointing most downwards lies flat.
void restOnFace(std::vector<Poly>& polys) {
    const Vec3 down(0, 0, -1);
    Vec3 best;
    double bestDot = -2;
    for (const Poly& p : polys) {
        const Vec3 n = p.out.normalized();
        if (n.dot(down) > bestDot + 1e-9) {
            bestDot = n.dot(down);
            best = n;
        }
    }
    if (bestDot > 1 - 1e-12) return;
    Vec3 axis = best.cross(down);
    if (axis.length() < 1e-12) axis = Vec3(1, 0, 0);
    const Mat4 r = Mat4::rotation(axis.normalized(), std::acos(std::clamp(bestDot, -1.0, 1.0)));
    for (Poly& p : polys) {
        for (Vec3& q : p.pts) q = r.transformPoint(q);
        p.out = r.transformDir(p.out);
    }
}

std::vector<Poly> buildBox(const PrimitiveParams& p) {
    const double x = p.width / 2, y = p.depth / 2, h = p.height;
    const Vec3 c[8] = {{-x, -y, 0}, {x, -y, 0}, {x, y, 0}, {-x, y, 0},
                       {-x, -y, h}, {x, -y, h}, {x, y, h}, {-x, y, h}};
    const int quads[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    const Vec3 mid(0, 0, h / 2);
    std::vector<Poly> polys;
    for (const auto& q : quads) {
        Poly poly;
        for (int i : q) poly.pts.push_back(c[i]);
        poly.out = centroid(poly.pts) - mid;
        polys.push_back(std::move(poly));
    }
    return polys;
}

std::vector<Poly> buildSphere(const PrimitiveParams& p) {
    const int n = p.segments, m = p.rings;
    auto at = [&](int ring, int seg) {
        const double th = kPi * ring / m, ph = 2 * kPi * seg / n;
        return Vec3(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th)) * p.radius;
    };
    std::vector<Poly> polys;
    for (int i = 0; i < m; ++i)
        for (int j = 0; j < n; ++j) {
            Poly poly;
            if (i == 0) poly.pts = {at(0, 0), at(1, j), at(1, j + 1)};
            else if (i == m - 1) poly.pts = {at(i, j), at(m, 0), at(i, j + 1)};
            else poly.pts = {at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)};
            poly.out = centroid(poly.pts);
            poly.group = 0;
            polys.push_back(std::move(poly));
        }
    return polys;
}

std::vector<Poly> buildGeoSphere(const PrimitiveParams& p) {
    const int f = p.frequency;
    std::vector<Poly> polys;
    for (const Poly& face : convexHull(icosahedronPoints())) {
        const Vec3 a = face.pts[0], b = face.pts[1], c = face.pts[2];
        auto at = [&](int i, int j) { return (a + (b - a) * (double(i) / f) + (c - a) * (double(j) / f)).normalized() * p.radius; };
        for (int i = 0; i < f; ++i)
            for (int j = 0; i + j < f; ++j) {
                Poly t;
                t.pts = {at(i, j), at(i + 1, j), at(i, j + 1)};
                t.group = 0;
                t.out = centroid(t.pts);
                polys.push_back(t);
                if (i + j + 2 <= f) {
                    t.pts = {at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)};
                    t.out = centroid(t.pts);
                    polys.push_back(t);
                }
            }
    }
    return polys;
}

// Cylinder (r0 == r1), cone (r1 == 0) or frustum.
std::vector<Poly> buildCone(double r0, double r1, double h, int n) {
    auto ring = [&](double r, double z, int j) {
        const double a = 2 * kPi * j / n;
        return Vec3(r * std::cos(a), r * std::sin(a), z);
    };
    std::vector<Poly> polys;
    Poly bottom;
    bottom.out = Vec3(0, 0, -1);
    bottom.curve = true;
    for (int j = 0; j < n; ++j) bottom.pts.push_back(ring(r0, 0, j));
    polys.push_back(bottom);
    if (r1 > 0) {
        Poly top;
        top.out = Vec3(0, 0, 1);
        top.curve = true;
        for (int j = 0; j < n; ++j) top.pts.push_back(ring(r1, h, j));
        polys.push_back(top);
    }
    for (int j = 0; j < n; ++j) {
        Poly side;
        side.group = 0;
        if (r1 > 0) side.pts = {ring(r0, 0, j), ring(r0, 0, j + 1), ring(r1, h, j + 1), ring(r1, h, j)};
        else side.pts = {ring(r0, 0, j), ring(r0, 0, j + 1), Vec3(0, 0, h)};
        const Vec3 c = centroid(side.pts);
        side.out = Vec3(c.x, c.y, 0);
        polys.push_back(std::move(side));
    }
    return polys;
}

std::vector<Poly> buildTorus(const PrimitiveParams& p) {
    const double R = p.radius, r = p.radius2;
    const int n = p.segments, m = p.rings;
    auto at = [&](int i, int j) {
        const double u = 2 * kPi * i / n, v = 2 * kPi * j / m;
        return Vec3((R + r * std::cos(v)) * std::cos(u), (R + r * std::cos(v)) * std::sin(u), r * std::sin(v));
    };
    std::vector<Poly> polys;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < m; ++j) {
            Poly q;
            q.pts = {at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)};
            q.group = 0;
            const Vec3 c = centroid(q.pts);
            q.out = c - Vec3(c.x, c.y, 0).normalized() * R;
            polys.push_back(std::move(q));
        }
    return polys;
}

bool fail(std::string* err, const char* msg) {
    if (err) *err = msg;
    return false;
}

} // namespace

const char* primitiveName(PrimitiveKind k) {
    switch (k) {
    case PrimitiveKind::Box: return "Box";
    case PrimitiveKind::Sphere: return "Sphere";
    case PrimitiveKind::GeoSphere: return "Geodesic Sphere";
    case PrimitiveKind::Cylinder: return "Cylinder";
    case PrimitiveKind::Cone: return "Cone";
    case PrimitiveKind::Torus: return "Torus";
    case PrimitiveKind::Tetrahedron: return "Tetrahedron";
    case PrimitiveKind::Octahedron: return "Octahedron";
    case PrimitiveKind::Dodecahedron: return "Dodecahedron";
    case PrimitiveKind::Icosahedron: return "Icosahedron";
    }
    return "Primitive";
}

PrimitiveFields primitiveFields(PrimitiveKind k) {
    PrimitiveFields f;
    switch (k) {
    case PrimitiveKind::Box:
        f.width = f.depth = f.height = true;
        break;
    case PrimitiveKind::Sphere:
        f.radius = f.segments = f.rings = f.smooth = true;
        break;
    case PrimitiveKind::GeoSphere:
        f.radius = f.frequency = f.smooth = true;
        break;
    case PrimitiveKind::Cylinder:
        f.radius = f.height = f.segments = f.smooth = true;
        break;
    case PrimitiveKind::Cone:
        f.radius = f.radius2 = f.height = f.segments = f.smooth = true;
        break;
    case PrimitiveKind::Torus:
        f.radius = f.radius2 = f.segments = f.rings = f.smooth = true;
        break;
    default: // Platonic solids
        f.radius = true;
        break;
    }
    return f;
}

bool buildPrimitive(const PrimitiveParams& p, Mesh& out, std::string* err) {
    const PrimitiveFields use = primitiveFields(p.kind);
    if (use.radius && !(p.radius > 0)) return fail(err, "The radius must be above zero.");
    if ((use.width && !(p.width > 0)) || (use.depth && !(p.depth > 0)) || (use.height && !(p.height > 0)))
        return fail(err, "The size must be above zero.");
    if (use.segments && (p.segments < 3 || p.segments > 512)) return fail(err, "Use 3 to 512 segments.");
    if (use.rings && (p.rings < (p.kind == PrimitiveKind::Torus ? 3 : 2) || p.rings > 256))
        return fail(err, p.kind == PrimitiveKind::Torus ? "Use 3 to 256 tube segments." : "Use 2 to 256 rings.");
    if (use.frequency && (p.frequency < 1 || p.frequency > 32)) return fail(err, "Use a subdivision of 1 to 32.");
    if (p.kind == PrimitiveKind::Cone && !(p.radius2 >= 0)) return fail(err, "The top radius cannot be negative.");
    if (p.kind == PrimitiveKind::Torus && !(p.radius2 > 0 && p.radius2 < p.radius))
        return fail(err, "The tube radius must be above zero and smaller than the ring radius.");

    std::vector<Poly> polys;
    switch (p.kind) {
    case PrimitiveKind::Box: polys = buildBox(p); break;
    case PrimitiveKind::Sphere: polys = buildSphere(p); break;
    case PrimitiveKind::GeoSphere: polys = buildGeoSphere(p); break;
    case PrimitiveKind::Cylinder: polys = buildCone(p.radius, p.radius, p.height, p.segments); break;
    case PrimitiveKind::Cone: polys = buildCone(p.radius, p.radius2, p.height, p.segments); break;
    case PrimitiveKind::Torus: polys = buildTorus(p); break;
    default:
        polys = convexHull(scaledTo(platonicPoints(p.kind), p.radius));
        restOnFace(polys);
        break;
    }

    // Stand it on the ground, its bounding box centred on the Z axis.
    Vec3 mn(1e300, 1e300, 1e300), mx(-1e300, -1e300, -1e300);
    for (const Poly& poly : polys)
        for (const Vec3& q : poly.pts)
            for (int i = 0; i < 3; ++i) {
                mn[i] = std::min(mn[i], q[i]);
                mx[i] = std::max(mx[i], q[i]);
            }
    const Vec3 lift(-(mn.x + mx.x) / 2, -(mn.y + mx.y) / 2, -mn.z);

    std::vector<int> faceGroup;
    std::vector<std::vector<std::pair<Id, Id>>> curves; // one per circle
    for (const Poly& poly : polys) {
        std::vector<Id> loop;
        for (const Vec3& q : poly.pts) {
            const Id v = out.addVertex(q + lift);
            if (loop.empty() || loop.back() != v) loop.push_back(v);
        }
        while (loop.size() > 1 && loop.front() == loop.back()) loop.pop_back();
        if (loop.size() < 3) continue;
        const Id f = out.addFaceRaw(loop, {}, poly.out);
        if (f == kNone) continue;
        if ((Id)faceGroup.size() <= f) faceGroup.resize(f + 1, -1);
        faceGroup[f] = poly.group;
        if (poly.curve) {
            curves.emplace_back();
            for (size_t i = 0; i < loop.size(); ++i) curves.back().push_back({loop[i], loop[(i + 1) % loop.size()]});
        }
    }
    if (out.aliveFaceCount() == 0) return fail(err, "Nothing to build.");

    // Soft edges inside curved surfaces; circle edges share a curve id.
    if (p.smooth && use.smooth)
        for (Id e = 0; e < (Id)out.edges.size(); ++e) {
            const auto& fs = out.edgeFaces(e);
            if (fs.size() == 2 && faceGroup[fs[0]] >= 0 && faceGroup[fs[0]] == faceGroup[fs[1]])
                out.edges[e].soft = out.edges[e].smooth = true;
        }
    for (const auto& circle : curves) {
        const Id curve = out.newCurveId();
        for (const auto& [a, b] : circle) {
            const Id e = out.findEdge(a, b);
            if (e != kNone) out.edges[e].curve = curve;
        }
    }
    out.touch();
    return true;
}

} // namespace gs
