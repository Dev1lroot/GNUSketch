// SPDX-License-Identifier: GPL-3.0-or-later
#include "PackingSolver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <tuple>

namespace forma::packing {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr int kFits = -2, kOutside = -1;

Vec2 rotated(Vec2 p, double a) {
    const double c = std::cos(a), s = std::sin(a);
    return {p.x * c - p.y * s, p.x * s + p.y * c};
}

struct Box {
    double x0 = kInf, y0 = kInf, x1 = -kInf, y1 = -kInf;
    void add(Vec2 p) {
        x0 = std::min(x0, p.x), y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
    }
    void add(const Box& b) {
        x0 = std::min(x0, b.x0), y0 = std::min(y0, b.y0);
        x1 = std::max(x1, b.x1), y1 = std::max(y1, b.y1);
    }
    bool valid() const { return x0 <= x1 && y0 <= y1; }
    double w() const { return x1 - x0; }
    double h() const { return y1 - y0; }
    Box moved(Vec2 d) const { return {x0 + d.x, y0 + d.y, x1 + d.x, y1 + d.y}; }
    Box grown(double g) const { return {x0 - g, y0 - g, x1 + g, y1 + g}; }
    // Closer than `gap` (touching at gap is not overlapping).
    bool near(const Box& o, double gap) const { return x0 < o.x1 + gap && o.x0 < x1 + gap && y0 < o.y1 + gap && o.y0 < y1 + gap; }
};

// A convex part ready for separating axis tests.
struct Part {
    std::vector<Vec2> pts;
    std::vector<Vec2> axes; // unit edge normals
    Box box;
    Vec2 centroid;
};

Part makePart(const Convex& c) {
    Part p;
    p.pts = c;
    const int n = (int)c.size();
    const int edges = n == 2 ? 1 : n;
    for (int i = 0; i < edges; ++i) {
        const Vec2 e = c[(i + 1) % n] - c[i];
        const double len = e.length();
        if (len < 1e-14) continue;
        const Vec2 ax(-e.y / len, e.x / len);
        bool dup = false;
        for (const Vec2& a : p.axes) dup = dup || std::fabs(a.cross(ax)) < 1e-12;
        if (!dup) p.axes.push_back(ax);
    }
    Vec2 sum;
    for (const Vec2& q : c) {
        p.box.add(q);
        sum = sum + q;
    }
    if (n) p.centroid = sum * (1.0 / n);
    return p;
}

void project(const Part& p, Vec2 off, Vec2 ax, double& lo, double& hi) {
    lo = kInf, hi = -kInf;
    for (const Vec2& q : p.pts) {
        const double d = q.dot(ax);
        lo = std::min(lo, d), hi = std::max(hi, d);
    }
    const double o = off.dot(ax);
    lo += o, hi += o;
}

// Kept at least `gap` apart along some edge normal of either (touching at the gap is fine).
bool separated(const Part& a, Vec2 oa, const Part& b, Vec2 ob, double gap, double tol) {
    for (const Part* P : {&a, &b})
        for (const Vec2& ax : P->axes) {
            double la, ha, lb, hb;
            project(a, oa, ax, la, ha);
            project(b, ob, ax, lb, hb);
            if (ha + gap <= lb + tol || hb + gap <= la + tol) return true;
        }
    return false;
}

bool inPolygons(const std::vector<std::vector<std::vector<Vec2>>>& area, Vec2 p) {
    for (const auto& loops : area) {
        if (loops.empty()) continue;
        auto inLoop = [&](const std::vector<Vec2>& l) { return pointInPolygon2D((int)l.size(), [&](int i) { return l[i]; }, p); };
        if (!inLoop(loops[0])) continue;
        bool inHole = false;
        for (size_t h = 1; h < loops.size() && !inHole; ++h) inHole = inLoop(loops[h]);
        if (!inHole) return true;
    }
    return false;
}

// A uniform grid of buckets over a box (things listed in every cell their box touches).
struct Grid {
    Box box;
    double cell = 1;
    int nx = 1, ny = 1;
    std::vector<std::vector<int>> cells;
    void init(const Box& b, double cellSize) {
        box = b;
        cell = std::max(cellSize, std::max(b.w(), b.h()) / 256.0);
        if (!(cell > 0)) cell = 1;
        nx = std::clamp((int)std::ceil(b.w() / cell), 1, 512);
        ny = std::clamp((int)std::ceil(b.h() / cell), 1, 512);
        cells.assign((size_t)nx * ny, {});
    }
    void range(const Box& b, int& i0, int& j0, int& i1, int& j1) const {
        i0 = std::clamp((int)std::floor((b.x0 - box.x0) / cell), 0, nx - 1);
        i1 = std::clamp((int)std::floor((b.x1 - box.x0) / cell), 0, nx - 1);
        j0 = std::clamp((int)std::floor((b.y0 - box.y0) / cell), 0, ny - 1);
        j1 = std::clamp((int)std::floor((b.y1 - box.y0) / cell), 0, ny - 1);
    }
    void insert(int id, const Box& b) {
        int i0, j0, i1, j1;
        range(b, i0, j0, i1, j1);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) cells[(size_t)j * nx + i].push_back(id);
    }
};

// What every attempt shares.
struct Prepared {
    const Problem* p = nullptr;
    std::vector<std::pair<Vec2, Vec2>> border;
    std::vector<double> directions; // main directions of the border, best first
    std::vector<double> itemArea;
    double scale = 1, tol = 1e-9;
};

// One attempt's settings.
struct Config {
    double theta = 0; // gravity: -y of the frame turned by theta
    bool flip = false; // sweep from the other side
    std::vector<std::vector<char>> use; // per item, per angle: allowed in this attempt
    int key = 0;      // ranking of places: 0 lowest bottom, 1 lowest top
    double step = 1;  // grid fineness factor
    std::string fingerprint() const {
        std::string s = std::to_string(std::llround(std::fmod(theta + 8 * kPi, 2 * kPi) * 1e4)) + (flip ? "f" : "n") +
                        std::to_string(key) + ":" + std::to_string(std::llround(step * 100));
        for (const auto& u : use) {
            s += '|';
            for (char c : u) s += c ? '1' : '0';
        }
        return s;
    }
};

class Packer {
public:
    Packer(const Prepared& pr, const Config& c) : pr_(pr), p_(*pr.p), c_(c) {}

    Layout run(const std::atomic<bool>* stop, int resolution) {
        stop_ = stop;
        setUp(resolution);
        Layout out;
        out.counts.assign(p_.items.size(), 0);
        Box spread;
        for (int t = 0; t < (int)p_.items.size(); ++t) {
            std::vector<int> mine;
            for (int v = 0; v < (int)variants_.size(); ++v)
                if (variants_[v].item == t) mine.push_back(v);
            const int limit = p_.items[t].maxCount;
            while (!stopped() && (limit <= 0 || out.counts[t] < limit)) {
                int bestV = -1;
                Vec2 bestP;
                double k0 = kInf, k1 = kInf;
                for (int v : mine) {
                    Vec2 q;
                    if (!findNext(cursors_[v], q)) continue;
                    const Variant& V = variants_[v];
                    const double a = q.y + (c_.key == 0 ? V.box.y0 : V.box.y1), b = q.x + V.box.x0;
                    if (a < k0 - pr_.tol || (a <= k0 + pr_.tol && b < k1)) bestV = v, bestP = q, k0 = a, k1 = b;
                }
                if (bestV < 0) break;
                const Vec2 q = compact(variants_[bestV], bestP, cursors_[bestV].h);
                place(bestV, q);
                ++out.counts[t];
                const Variant& V = variants_[bestV];
                spread.add(V.box.moved(q));
                out.usedArea += pr_.itemArea[t];
                // back to the area's frame: q = F(R(-theta) x)  =>  x = R(theta) F(q)
                out.placements.push_back({t, rotated(flipped(q), c_.theta), V.angle});
            }
        }
        out.spread = spread.valid() ? spread.w() * spread.h() : 0;
        return out;
    }

private:
    struct Variant {
        int item = 0;
        double angle = 0;
        std::vector<Part> parts; // in the attempt's frame, around the origin
        Box box;
    };
    struct Solid {
        const std::vector<Part>* parts = nullptr;
        Vec2 off;
        Box box;
    };
    struct Cursor {
        double x0 = 0, y0 = 0, xMax = 0, yMax = 0, h = 1;
        int cols = 0, rows = 0, ix = 0, iy = 0;
        bool has = false; // `found` was a fit last time
        Vec2 found;
        double col(int i) const { return i >= cols - 1 ? xMax : x0 + i * h; }
        double row(int j) const { return j >= rows - 1 ? yMax : y0 + j * h; }
    };

    bool stopped() const { return stop_ && stop_->load(std::memory_order_relaxed); }
    Vec2 flipped(Vec2 q) const { return c_.flip ? Vec2(-q.x, q.y) : q; }
    // area frame -> attempt frame
    Vec2 toFrame(Vec2 x) const { return flipped(rotated(x, -c_.theta)); }
    Convex toFrame(const Convex& c) const {
        Convex o;
        o.reserve(c.size());
        for (const Vec2& q : c) o.push_back(toFrame(q));
        return o;
    }

    void setUp(int resolution) {
        tol_ = pr_.tol;
        spacing_ = std::max(0.0, p_.spacing);
        margin_ = std::max(0.0, p_.margin);
        // The area in the attempt's frame.
        area_.clear();
        for (const auto& loops : p_.area) {
            std::vector<std::vector<Vec2>> L;
            for (const auto& l : loops) L.push_back(toFrame(l));
            area_.push_back(std::move(L));
        }
        Box ab;
        for (const auto& loops : area_)
            if (!loops.empty())
                for (const Vec2& q : loops[0]) ab.add(q);
        areaBox_ = ab;
        // Variants: each item turned by each angle it may use in this attempt.
        double biggest = 0;
        for (int t = 0; t < (int)p_.items.size(); ++t) {
            const ItemType& it = p_.items[t];
            for (int a = 0; a < (int)it.angles.size(); ++a) {
                if (t < (int)c_.use.size() && a < (int)c_.use[t].size() && !c_.use[t][a]) continue;
                Variant V;
                V.item = t;
                V.angle = it.angles[a];
                for (const Convex& c : it.shape.parts) {
                    Convex r;
                    for (const Vec2& q : c) r.push_back(toFrame(rotated(q, V.angle)));
                    V.parts.push_back(makePart(r));
                    V.box.add(V.parts.back().box);
                }
                if (!V.box.valid()) continue;
                biggest = std::max(biggest, std::max(V.box.w(), V.box.h()));
                variants_.push_back(std::move(V));
            }
        }
        // Buckets for what copies must keep clear of.
        solidGrid_.init(areaBox_.grown(biggest + spacing_ + margin_), biggest + spacing_);
        for (const Convex& c : p_.obstacles) {
            obstacleParts_.push_back({makePart(toFrame(c))});
        }
        for (const auto& op : obstacleParts_) {
            if (!op[0].box.near(areaBox_, spacing_ + tol_)) continue;
            addSolid(&op, Vec2());
        }
        borderParts_.clear();
        for (const auto& [a, b] : pr_.border) borderParts_.push_back(makePart({toFrame(a), toFrame(b)}));
        borderGrid_.init(areaBox_.grown(margin_ + tol_), std::max(areaBox_.w(), areaBox_.h()) / 64.0);
        for (int i = 0; i < (int)borderParts_.size(); ++i) borderGrid_.insert(i, borderParts_[i].box.grown(margin_ + tol_));
        borderMark_.assign(borderParts_.size(), 0);
        // A grid of places per variant, as fine as the item's smaller side allows.
        cursors_.assign(variants_.size(), {});
        std::vector<double> minDim(p_.items.size(), kInf);
        for (const Variant& V : variants_) minDim[V.item] = std::min({minDim[V.item], V.box.w(), V.box.h()});
        for (int v = 0; v < (int)variants_.size(); ++v) {
            const Variant& V = variants_[v];
            Cursor& C = cursors_[v];
            double h = std::max(minDim[V.item], 1e-6 * pr_.scale) * c_.step / std::max(1, resolution);
            h = std::max(h, pr_.scale / 4000.0);
            C.x0 = areaBox_.x0 + margin_ - V.box.x0;
            C.y0 = areaBox_.y0 + margin_ - V.box.y0;
            C.xMax = areaBox_.x1 - margin_ - V.box.x1;
            C.yMax = areaBox_.y1 - margin_ - V.box.y1;
            if (C.xMax < C.x0 - tol_ || C.yMax < C.y0 - tol_) continue; // never fits
            C.xMax = std::max(C.xMax, C.x0), C.yMax = std::max(C.yMax, C.y0);
            // (at most a few million places to look at)
            const double cells = ((C.xMax - C.x0) / h + 1) * ((C.yMax - C.y0) / h + 1);
            if (cells > 3e6) h *= std::sqrt(cells / 3e6);
            C.h = h;
            auto count = [&](double span) {
                const int n = (int)std::floor(span / h + 1e-9) + 1;
                return span - (n - 1) * h > tol_ ? n + 1 : n; // and the far end exactly
            };
            C.cols = count(C.xMax - C.x0);
            C.rows = count(C.yMax - C.y0);
        }
    }

    void addSolid(const std::vector<Part>* parts, Vec2 off) {
        Solid s;
        s.parts = parts;
        s.off = off;
        for (const Part& q : *parts) s.box.add(q.box.moved(off));
        solids_.push_back(s);
        solidMark_.push_back(0);
        solidGrid_.insert((int)solids_.size() - 1, s.box.grown(spacing_ + tol_));
    }

    void place(int v, Vec2 q) { addSolid(&variants_[v].parts, q); }

    // kFits, kOutside (the border or out of the area), or the solid in the way.
    int test(const Variant& V, Vec2 q) {
        ++tests_;
        const Box b = V.box.moved(q);
        if (b.x0 < areaBox_.x0 + margin_ - tol_ || b.y0 < areaBox_.y0 + margin_ - tol_ || b.x1 > areaBox_.x1 - margin_ + tol_ ||
            b.y1 > areaBox_.y1 - margin_ + tol_)
            return kOutside;
        // other copies and obstacles
        ++stamp_;
        int i0, j0, i1, j1;
        solidGrid_.range(b.grown(spacing_), i0, j0, i1, j1);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                for (int sid : solidGrid_.cells[(size_t)j * solidGrid_.nx + i]) {
                    if (solidMark_[sid] == stamp_) continue;
                    solidMark_[sid] = stamp_;
                    const Solid& S = solids_[sid];
                    if (!b.near(S.box, spacing_ - tol_)) continue;
                    for (const Part& a : V.parts) {
                        const Box pa = a.box.moved(q);
                        if (!pa.near(S.box, spacing_ - tol_)) continue;
                        for (const Part& o : *S.parts)
                            if (pa.near(o.box.moved(S.off), spacing_ - tol_) && !separated(a, q, o, S.off, spacing_, tol_)) return sid;
                    }
                }
        // the border
        ++borderStamp_;
        near_.clear();
        borderGrid_.range(b.grown(margin_), i0, j0, i1, j1);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                for (int e : borderGrid_.cells[(size_t)j * borderGrid_.nx + i]) {
                    if (borderMark_[e] == borderStamp_) continue;
                    borderMark_[e] = borderStamp_;
                    if (b.near(borderParts_[e].box, margin_ - tol_)) near_.push_back(e);
                }
        // Nothing of the border near: the whole box is inside the area or outside it.
        if (near_.empty()) return inPolygons(area_, Vec2((b.x0 + b.x1) * 0.5, (b.y0 + b.y1) * 0.5)) ? kFits : kOutside;
        for (const Part& a : V.parts) {
            const Box pa = a.box.moved(q);
            for (int e : near_)
                if (pa.near(borderParts_[e].box, margin_ - tol_) && !separated(a, q, borderParts_[e], Vec2(), margin_, tol_)) return kOutside;
        }
        // No part crosses the border: each is inside or outside as a whole.
        for (const Part& a : V.parts)
            if (!inPolygons(area_, a.centroid + q)) return kOutside;
        return kFits;
    }

    // The next place (in the sweep: rows from the bottom, each from the left) where
    // the variant fits now. Places passed over never fit again.
    bool findNext(Cursor& C, Vec2& out) {
        const Variant& V = variants_[&C - cursors_.data()];
        if (C.has && test(V, C.found) == kFits) {
            out = C.found;
            return true;
        }
        C.has = false;
        while (C.iy < C.rows) {
            if (stopped()) return false;
            const double y = C.row(C.iy), yNext = C.iy + 1 < C.rows ? C.row(C.iy + 1) : kInf;
            while (C.ix < C.cols) {
                const double x = C.col(C.ix), xNext = C.ix + 1 < C.cols ? C.col(C.ix + 1) : kInf;
                const int r = test(V, {x, y});
                Vec2 hit;
                bool ok = r == kFits;
                if (ok) hit = {x, y};
                else if (r >= 0) {
                    // Just right of / above what is in the way, if that is in this cell:
                    // where copies placed against each other line up exactly.
                    const Box& bb = solids_[r].box;
                    const double sx = bb.x1 + spacing_ - V.box.x0, sy = bb.y1 + spacing_ - V.box.y0;
                    const bool inX = sx > x && sx < xNext && sx <= C.xMax + tol_, inY = sy > y && sy < yNext && sy <= C.yMax + tol_;
                    if (inX && test(V, {sx, y}) == kFits) ok = true, hit = {sx, y};
                    else if (inY && test(V, {x, sy}) == kFits) ok = true, hit = {x, sy};
                    else if (inX && inY && test(V, {sx, sy}) == kFits) ok = true, hit = {sx, sy};
                }
                if (ok) {
                    C.has = true;
                    C.found = hit;
                    out = hit;
                    return true;
                }
                ++C.ix;
            }
            C.ix = 0;
            ++C.iy;
        }
        return false;
    }

    // Pushed down, then left, while it keeps fitting, until it touches.
    Vec2 compact(const Variant& V, Vec2 q, double h) {
        for (int round = 0; round < 4; ++round) {
            bool moved = false;
            for (const Vec2 dir : {Vec2(0, -1), Vec2(-1, 0)}) {
                double lo = 0;
                int steps = 0;
                while (steps++ < 64 && test(V, q + dir * (lo + h)) == kFits) lo += h;
                double hi = lo + h;
                for (int i = 0; i < 26; ++i) {
                    const double mid = 0.5 * (lo + hi);
                    (test(V, q + dir * mid) == kFits ? lo : hi) = mid;
                }
                if (lo > tol_) {
                    q = q + dir * lo;
                    moved = true;
                }
            }
            if (!moved) break;
        }
        return q;
    }

    const Prepared& pr_;
    const Problem& p_;
    Config c_;
    const std::atomic<bool>* stop_ = nullptr;
    double tol_ = 1e-9, spacing_ = 0, margin_ = 0;
    std::vector<std::vector<std::vector<Vec2>>> area_;
    Box areaBox_;
    std::vector<Variant> variants_;
    std::vector<Cursor> cursors_;
    std::vector<std::vector<Part>> obstacleParts_;
    std::vector<Solid> solids_;
    std::vector<uint32_t> solidMark_;
    uint32_t stamp_ = 0;
    Grid solidGrid_;
    std::vector<Part> borderParts_;
    Grid borderGrid_;
    std::vector<uint32_t> borderMark_;
    uint32_t borderStamp_ = 0;
    std::vector<int> near_;
    long long tests_ = 0;
};

std::vector<double> borderDirections(const std::vector<std::pair<Vec2, Vec2>>& border, size_t most) {
    // Each edge's angle folded into [0, pi/2), weighted by its length; close ones together.
    std::vector<std::pair<double, double>> a; // angle, length
    for (const auto& [p, q] : border) {
        const Vec2 d = q - p;
        const double len = d.length();
        if (len < 1e-12) continue;
        double t = std::fmod(std::atan2(d.y, d.x) + 4 * kPi, kPi / 2);
        if (t > kPi / 2 - 1e-9) t = 0;
        a.push_back({t, len});
    }
    std::sort(a.begin(), a.end());
    const double near = 0.5 * kPi / 180;
    std::vector<std::pair<double, double>> groups; // weight, angle
    for (const auto& [t, len] : a) {
        bool joined = false;
        for (auto& [w, g] : groups) {
            double d = std::fabs(t - g);
            d = std::min(d, kPi / 2 - d);
            if (d < near) {
                w += len;
                joined = true;
                break;
            }
        }
        if (!joined) groups.push_back({len, t});
    }
    std::sort(groups.begin(), groups.end(), [](auto& x, auto& y) { return x.first > y.first; });
    std::vector<double> out;
    for (const auto& [w, g] : groups)
        if (out.size() < most) out.push_back(g);
    if (out.empty()) out.push_back(0);
    return out;
}

} // namespace

bool Layout::betterThan(const Layout& o) const {
    const size_t n = std::max(counts.size(), o.counts.size());
    for (size_t i = 0; i < n; ++i) {
        const int a = i < counts.size() ? counts[i] : 0, b = i < o.counts.size() ? o.counts[i] : 0;
        if (a != b) return a > b;
    }
    return spread < o.spread * (1 - 1e-9);
}

double area(const Convex& c) {
    if (c.size() < 3) return 0;
    return std::fabs(signedArea2D((int)c.size(), [&](int i) { return c[i]; }));
}

double area(const Shape& s) {
    double a = 0;
    for (const Convex& c : s.parts) a += area(c);
    return a;
}

Convex convexHull(std::vector<Vec2> pts) {
    std::sort(pts.begin(), pts.end(), [](const Vec2& a, const Vec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    pts.erase(std::unique(pts.begin(), pts.end(), [](const Vec2& a, const Vec2& b) { return a.x == b.x && a.y == b.y; }), pts.end());
    if (pts.size() < 3) return pts;
    Convex h(2 * pts.size());
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && (h[k - 1] - h[k - 2]).cross(pts[i] - h[k - 2]) <= 0) --k;
        h[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i-- > 0;) {
        while (k >= t && (h[k - 1] - h[k - 2]).cross(pts[i] - h[k - 2]) <= 0) --k;
        h[k++] = pts[i];
    }
    h.resize(k - 1);
    return h;
}

Shape hullShape(const Shape& s) {
    std::vector<Vec2> pts;
    for (const Convex& c : s.parts) pts.insert(pts.end(), c.begin(), c.end());
    Shape o;
    o.parts.push_back(convexHull(pts));
    return o;
}

Shape boxShape(const Shape& s) {
    Box b;
    for (const Convex& c : s.parts)
        for (const Vec2& q : c) b.add(q);
    Shape o;
    if (b.valid()) o.parts.push_back({{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}});
    return o;
}

double principalAngle(const Shape& s) {
    const Convex h = hullShape(s).parts[0];
    if (h.size() < 3) return 0;
    double best = kInf, angle = 0;
    for (size_t i = 0; i < h.size(); ++i) {
        const Vec2 e = h[(i + 1) % h.size()] - h[i];
        if (e.length() < 1e-14) continue;
        const double t = std::atan2(e.y, e.x);
        Box b;
        for (const Vec2& q : h) b.add(rotated(q, -t));
        const double a = b.w() * b.h();
        if (a < best * (1 - 1e-9)) best = a, angle = t;
    }
    angle = std::fmod(angle + 4 * kPi, kPi / 2);
    return angle > kPi / 2 - 1e-9 ? 0 : angle;
}

std::vector<std::pair<Vec2, Vec2>> areaBorder(const std::vector<std::vector<std::vector<Vec2>>>& area) {
    double scale = 0;
    for (const auto& loops : area)
        for (const auto& l : loops)
            for (const Vec2& q : l) scale = std::max({scale, std::fabs(q.x), std::fabs(q.y)});
    const double quantum = std::max(scale, 1.0) * 1e-9;
    using Key = std::tuple<long long, long long, long long, long long>;
    std::map<Key, std::pair<int, std::pair<Vec2, Vec2>>> seen;
    std::vector<Key> order;
    for (const auto& loops : area)
        for (const auto& l : loops)
            for (size_t i = 0; i < l.size(); ++i) {
                Vec2 a = l[i], b = l[(i + 1) % l.size()];
                long long ax = std::llround(a.x / quantum), ay = std::llround(a.y / quantum);
                long long bx = std::llround(b.x / quantum), by = std::llround(b.y / quantum);
                if (ax == bx && ay == by) continue;
                if (std::tie(bx, by) < std::tie(ax, ay)) std::swap(ax, bx), std::swap(ay, by);
                const Key k{ax, ay, bx, by};
                auto [it, fresh] = seen.try_emplace(k, 0, std::make_pair(a, b));
                if (fresh) order.push_back(k);
                ++it->second.first;
            }
    std::vector<std::pair<Vec2, Vec2>> out;
    for (const Key& k : order)
        if (const auto& e = seen[k]; e.first % 2 == 1) out.push_back(e.second);
    return out;
}

double mainDirection(const Problem& p) { return borderDirections(areaBorder(p.area), 1)[0]; }

std::vector<Convex> placedParts(const Shape& s, Vec2 pos, double angle) {
    std::vector<Convex> out;
    for (const Convex& c : s.parts) {
        Convex r;
        for (const Vec2& q : c) r.push_back(rotated(q, angle) + pos);
        out.push_back(std::move(r));
    }
    return out;
}

bool layoutValid(const Problem& p, const Layout& l, std::string* why) {
    auto fail = [&](const std::string& s) {
        if (why) *why = s;
        return false;
    };
    Box ab;
    for (const auto& loops : p.area)
        for (const auto& lp : loops)
            for (const Vec2& q : lp) ab.add(q);
    const double tol = std::max(ab.w(), ab.h()) * 1e-7;
    std::vector<Part> border;
    for (const auto& [a, b] : areaBorder(p.area)) border.push_back(makePart({a, b}));
    std::vector<std::vector<Part>> copies;
    for (size_t i = 0; i < l.placements.size(); ++i) {
        const Placement& pl = l.placements[i];
        if (pl.item < 0 || pl.item >= (int)p.items.size()) return fail("no such item");
        std::vector<Part> parts;
        for (const Convex& c : placedParts(p.items[pl.item].shape, pl.pos, pl.angle)) parts.push_back(makePart(c));
        for (const Part& a : parts) {
            if (!inPolygons(p.area, a.centroid)) return fail("copy " + std::to_string(i) + " is outside the area");
            for (const Part& e : border)
                if (!separated(a, Vec2(), e, Vec2(), p.margin, tol)) return fail("copy " + std::to_string(i) + " crosses the border");
            for (const Convex& o : p.obstacles)
                if (!separated(a, Vec2(), makePart(o), Vec2(), p.spacing, tol)) return fail("copy " + std::to_string(i) + " is on an obstacle");
        }
        for (size_t j = 0; j < copies.size(); ++j)
            for (const Part& a : parts)
                for (const Part& b : copies[j])
                    if (!separated(a, Vec2(), b, Vec2(), p.spacing, tol))
                        return fail("copies " + std::to_string(j) + " and " + std::to_string(i) + " overlap");
        copies.push_back(std::move(parts));
    }
    return true;
}

Layout solve(const Problem& p, const SolveOptions& opt, const ProgressFn& progress) {
    Layout best;
    best.counts.assign(p.items.size(), 0);
    best.spread = kInf;
    if (p.items.empty() || p.area.empty()) return best;

    Prepared pr;
    pr.p = &p;
    pr.border = areaBorder(p.area);
    pr.directions = borderDirections(pr.border, 3);
    Box ab;
    for (const auto& loops : p.area)
        for (const auto& l : loops)
            for (const Vec2& q : l) ab.add(q);
    pr.scale = std::max(ab.w(), ab.h());
    if (!(pr.scale > 0)) return best;
    pr.tol = pr.scale * 1e-9;
    for (const ItemType& it : p.items) pr.itemArea.push_back(area(it.shape));

    std::mt19937 rng(opt.seed);
    auto uni = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
    auto pick = [&](int n) { return (int)(rng() % (unsigned)std::max(1, n)); };

    // Default turns of an item: all of them, or (many allowed) those at right angles to the first.
    auto defaultUse = [&](const ItemType& it) {
        std::vector<char> u(it.angles.size(), 1);
        if (it.angles.size() > 8)
            for (size_t a = 0; a < it.angles.size(); ++a) {
                const double d = std::fmod(std::fabs(it.angles[a] - it.angles[0]) + 1e-9, kPi / 2);
                u[a] = d < 2e-9;
            }
        return u;
    };
    auto randomUse = [&](const ItemType& it) {
        std::vector<char> u(it.angles.size(), 0);
        const int n = (int)it.angles.size();
        const int k = 1 + pick(std::min(8, n));
        for (int i = 0; i < k; ++i) u[pick(n)] = 1;
        return u;
    };
    Config base;
    for (const ItemType& it : p.items) base.use.push_back(defaultUse(it));

    // First the promising ones: gravity along each main direction of the border
    // (towards each of its four sides), swept from either end.
    std::vector<Config> survey;
    for (double d : pr.directions)
        for (int k = 0; k < 4; ++k)
            for (int f = 0; f < 2; ++f) {
                Config c = base;
                c.theta = d + k * kPi / 2;
                c.flip = f;
                survey.push_back(c);
            }

    auto mutate = [&](const Config& from) {
        Config c = from;
        do {
            switch (pick(6)) {
            case 0: c.theta = pr.directions[pick((int)pr.directions.size())] + pick(4) * kPi / 2; break;
            case 1: c.flip = !c.flip; break;
            case 2: {
                const int t = pick((int)p.items.size());
                auto& u = c.use[t];
                if (u.size() < 2) break;
                if (rng() % 2) {
                    u = randomUse(p.items[t]);
                } else {
                    const int a = pick((int)u.size());
                    u[a] = !u[a];
                    if (std::none_of(u.begin(), u.end(), [](char x) { return x; })) u[a] = 1;
                }
                break;
            }
            case 3: c.key = 1 - c.key; break;
            case 4: c.step = std::clamp(c.step * std::array<double, 3>{0.7, 0.85, 1.25}[pick(3)], 0.35, 1.6); break;
            default: c.theta += uni(-6, 6) * kPi / 180; break;
            }
        } while (rng() % 10 < 3);
        return c;
    };
    auto randomConfig = [&] {
        Config c;
        c.theta = uni(0, 2 * kPi);
        c.flip = rng() % 2;
        for (const ItemType& it : p.items) c.use.push_back(rng() % 2 ? defaultUse(it) : randomUse(it));
        c.key = rng() % 2;
        c.step = uni(0.5, 1.3);
        return c;
    };

    const int total = std::max(1, opt.attempts);
    const int threads = std::max(1, opt.threads > 0 ? opt.threads : (int)std::thread::hardware_concurrency());
    const auto start = std::chrono::steady_clock::now();
    std::atomic<bool> stopNow{false};
    // (the caller's stop flag, the time, or the progress callback stop it)
    auto timeUp = [&] {
        if (opt.stop && opt.stop->load()) return true;
        if (opt.seconds > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > opt.seconds) return true;
        return false;
    };
    // A watcher so that a long attempt stops too.
    std::thread watcher([&] {
        while (!stopNow.load()) {
            if (timeUp()) {
                stopNow = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    Config bestCfg = base;
    std::set<std::string> tried;
    int done = 0;
    size_t next = 0;
    bool haveBest = false;
    while (done < total && !stopNow.load()) {
        std::vector<Config> batch;
        while ((int)batch.size() < threads && done + (int)batch.size() < total) {
            Config c;
            if (next < survey.size()) c = survey[next++];
            else {
                for (int retry = 0; retry < 12; ++retry) {
                    c = (rng() % 10 < 8) ? mutate(bestCfg) : randomConfig();
                    if (!tried.count(c.fingerprint())) break;
                }
            }
            tried.insert(c.fingerprint());
            batch.push_back(std::move(c));
        }
        std::vector<Layout> results(batch.size());
        std::vector<char> complete(batch.size(), 0);
        std::vector<std::thread> pool;
        for (size_t i = 0; i < batch.size(); ++i)
            pool.emplace_back([&, i] {
                Packer pk(pr, batch[i]);
                results[i] = pk.run(&stopNow, opt.resolution);
                complete[i] = 1;
            });
        for (std::thread& t : pool) t.join();
        // An attempt cut short by the time can still be better (what it holds is valid).
        for (size_t i = 0; i < batch.size(); ++i) {
            if (!haveBest || results[i].betterThan(best)) {
                best = std::move(results[i]);
                bestCfg = batch[i];
                haveBest = true;
            }
        }
        done += (int)batch.size();
        if (progress && !progress(done, total, best)) stopNow = true;
    }
    stopNow = true;
    watcher.join();
    if (!std::isfinite(best.spread)) best.spread = 0;
    return best;
}

} // namespace forma::packing
