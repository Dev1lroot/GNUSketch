// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Solid tools: Union, Intersect, Subtract, Trim and Split on
// component instances.
#pragma once

#include "Tool.h"
#include "core/Solids.h"

#include <map>

namespace gs {

bool isSolidTool(ToolId id);
SolidOp solidToolOp(ToolId id);

// Click the first solid, then the second. For Subtract and Trim the first one
// is the cutter and the second the solid it is cut out of. A component that
// is selected when the tool starts counts as the first click; Union and
// Intersect apply to all selected components at once.
class SolidTool : public Tool {
public:
    SolidTool(Viewport* vp, ToolId id) : Tool(vp), id_(id) {}
    ToolId id() const override { return id_; }
    QString status() const override;
    void activate() override;
    void reset() override;
    bool busy() const override { return first_ != kNone; }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void buildPreview(Preview& pv) override;

private:
    // Whether the instance's definition is a solid (cached per mesh revision).
    bool solid(Id inst, std::string* why = nullptr);
    void apply(const std::vector<Id>& order);

    ToolId id_;
    Id first_ = kNone, hover_ = kNone;
    QString message_;
    struct Cached {
        uint64_t revision = 0;
        bool solid = false;
        std::string why;
    };
    std::map<Id, Cached> cache_; // by definition
};

} // namespace gs
