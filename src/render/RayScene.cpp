// SPDX-License-Identifier: GPL-3.0-or-later
#include "RayScene.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace gs {

void RayScene::build(const std::vector<Vec3>& mn, const std::vector<Vec3>& mx, std::vector<Node>& nodes, std::vector<int>& order,
                     int leafSize) {
    nodes.clear();
    const int n = (int)mn.size();
    order.resize(n);
    std::iota(order.begin(), order.end(), 0);
    if (n == 0) return;
    std::vector<Vec3> c(n);
    for (int i = 0; i < n; ++i) c[i] = (mn[i] + mx[i]) * 0.5;
    struct Job {
        int node, first, count;
    };
    nodes.push_back({});
    std::vector<Job> jobs{{0, 0, n}};
    while (!jobs.empty()) {
        const Job j = jobs.back();
        jobs.pop_back();
        Vec3 bmn(1e300, 1e300, 1e300), bmx(-1e300, -1e300, -1e300), cmn = bmn, cmx = bmx;
        for (int k = j.first; k < j.first + j.count; ++k) {
            const int i = order[k];
            for (int a = 0; a < 3; ++a) {
                bmn[a] = std::min(bmn[a], mn[i][a]);
                bmx[a] = std::max(bmx[a], mx[i][a]);
                cmn[a] = std::min(cmn[a], c[i][a]);
                cmx[a] = std::max(cmx[a], c[i][a]);
            }
        }
        nodes[j.node].mn = bmn;
        nodes[j.node].mx = bmx;
        int axis = 0;
        const Vec3 ext = cmx - cmn;
        if (ext.y > ext[axis]) axis = 1;
        if (ext.z > ext[axis]) axis = 2;
        if (j.count <= leafSize || ext[axis] <= 0) {
            nodes[j.node].a = j.first;
            nodes[j.node].count = j.count;
            continue;
        }
        const int mid = j.first + j.count / 2;
        std::nth_element(order.begin() + j.first, order.begin() + mid, order.begin() + j.first + j.count,
                         [&](int x, int y) { return c[x][axis] < c[y][axis]; });
        const int left = (int)nodes.size();
        nodes.push_back({});
        nodes.push_back({});
        nodes[j.node].a = left;
        nodes[j.node].count = 0;
        jobs.push_back({left, j.first, mid - j.first});
        jobs.push_back({left + 1, mid, j.first + j.count - mid});
    }
}

void RayScene::setMesh(const void* key, uint64_t revision, const std::vector<std::vector<float>>& triLists) {
    Blas& b = blas_[key];
    b.used = true;
    if (b.revision == revision && !b.nodes.empty()) return;
    b.revision = revision;
    std::vector<std::array<Vec3, 3>> tris;
    for (const auto& list : triLists)
        for (size_t i = 0; i + 9 <= list.size(); i += 9)
            tris.push_back({Vec3(list[i], list[i + 1], list[i + 2]), Vec3(list[i + 3], list[i + 4], list[i + 5]),
                            Vec3(list[i + 6], list[i + 7], list[i + 8])});
    std::vector<Vec3> mn(tris.size()), mx(tris.size());
    for (size_t i = 0; i < tris.size(); ++i)
        for (int a = 0; a < 3; ++a) {
            mn[i][a] = std::min({tris[i][0][a], tris[i][1][a], tris[i][2][a]});
            mx[i][a] = std::max({tris[i][0][a], tris[i][1][a], tris[i][2][a]});
        }
    std::vector<int> order;
    build(mn, mx, b.nodes, order, 4);
    b.tris.resize(tris.size());
    for (size_t k = 0; k < order.size(); ++k) b.tris[k] = tris[order[k]];
    if (b.nodes.empty()) b.nodes.push_back({Vec3(), Vec3(), 0, 0}); // nothing to hit
    dirty_ = true;
}

void RayScene::endMeshes() {
    for (auto it = blas_.begin(); it != blas_.end();) {
        if (!it->second.used) {
            it = blas_.erase(it);
            dirty_ = true;
        } else {
            it->second.used = false;
            ++it;
        }
    }
    if (dirty_) pack();
}

void RayScene::pack() {
    dirty_ = false;
    ++dataRev_;
    nodes_.clear();
    tris_.clear();
    int nodeBase = 0, triBase = 0;
    for (auto& [key, b] : blas_) {
        b.nodeOffset = nodeBase;
        b.triOffset = triBase;
        for (const Node& n : b.nodes) {
            const bool leaf = n.count > 0;
            const float a = (float)(leaf ? n.a + triBase : n.a + nodeBase);
            nodes_.insert(nodes_.end(), {(float)n.mn.x, (float)n.mn.y, (float)n.mn.z, a, (float)n.mx.x, (float)n.mx.y, (float)n.mx.z,
                                         (float)n.count});
        }
        for (const auto& t : b.tris) {
            const Vec3 e1 = t[1] - t[0], e2 = t[2] - t[0];
            tris_.insert(tris_.end(), {(float)t[0].x, (float)t[0].y, (float)t[0].z, 0.f, (float)e1.x, (float)e1.y, (float)e1.z, 0.f,
                                       (float)e2.x, (float)e2.y, (float)e2.z, 0.f});
        }
        nodeBase += (int)b.nodes.size();
        triBase += (int)b.tris.size();
    }
    // Buffer textures cannot be empty.
    if (nodes_.empty()) nodes_.assign(8, 0.f);
    if (tris_.empty()) tris_.assign(12, 0.f);
}

void RayScene::addInstance(const void* key, const Mat4& xf) {
    if (blas_.count(key)) inst_.push_back({key, xf});
}

void RayScene::buildTop() {
    // World boxes of the placed meshes.
    std::vector<Vec3> mn, mx;
    std::vector<int> valid;
    Vec3 smn(1e300, 1e300, 1e300), smx(-1e300, -1e300, -1e300);
    for (int i = 0; i < (int)inst_.size(); ++i) {
        const Blas& b = blas_.at(inst_[i].key);
        if (b.tris.empty()) continue;
        const Node& r = b.nodes[0];
        Vec3 lo(1e300, 1e300, 1e300), hi(-1e300, -1e300, -1e300);
        for (int k = 0; k < 8; ++k) {
            const Vec3 p = inst_[i].xf.transformPoint(Vec3((k & 1) ? r.mx.x : r.mn.x, (k & 2) ? r.mx.y : r.mn.y, (k & 4) ? r.mx.z : r.mn.z));
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], p[a]);
                hi[a] = std::max(hi[a], p[a]);
            }
        }
        mn.push_back(lo);
        mx.push_back(hi);
        valid.push_back(i);
        for (int a = 0; a < 3; ++a) {
            smn[a] = std::min(smn[a], lo[a]);
            smx[a] = std::max(smx[a], hi[a]);
        }
    }
    instCount_ = (int)valid.size();
    radius_ = valid.empty() ? 1.0 : std::max(0.01, (smx - smn).length() * 0.5);
    std::vector<Node> nodes;
    std::vector<int> order;
    build(mn, mx, nodes, order, 1);
    instData_.clear();
    for (int k : order) {
        const Inst& in = inst_[valid[k]];
        const Mat4 inv = in.xf.inverted();
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col) instData_.push_back((float)inv.at(row, col));
        instData_.insert(instData_.end(), {(float)blas_.at(in.key).nodeOffset, 0.f, 0.f, 0.f});
    }
    top_.clear();
    for (const Node& n : nodes)
        top_.insert(top_.end(), {(float)n.mn.x, (float)n.mn.y, (float)n.mn.z, (float)n.a, (float)n.mx.x, (float)n.mx.y, (float)n.mx.z,
                                 (float)n.count});
    if (top_.empty()) top_.assign(8, 0.f);
    if (instData_.empty()) instData_.assign(16, 0.f);
}

} // namespace gs
