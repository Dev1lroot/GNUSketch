// SPDX-License-Identifier: GPL-3.0-or-later
// Forma - Solid tools: Union, Intersect, Subtract, Trim and Split on
// component instances.
#pragma once

#include "Tool.h"
#include "core/Solids.h"

#include <map>

namespace forma {

bool isSolidTool(ToolId id);
SolidOp solidToolOp(ToolId id);

// Knife: click a flat face of the top-level geometry or a flat component (the
// knife), then any component to cut along it (it need not be a solid).
//
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
    bool busy() const override { return first_ != kNone || knifeFace_ != kNone; }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void buildPreview(Preview& pv) override;

private:
    // Whether the instance's definition is a solid (cached per mesh revision).
    bool solid(Id inst, std::string* why = nullptr);
    void apply(const std::vector<Id>& order);
    bool knife() const { return id_ == ToolId::SolidKnife; }
    // Whether the instance is flat enough to be a knife (cached per mesh revision).
    bool flat(Id inst);
    void pickKnife(const MouseEvent& e);
    void cut(Id target);

    ToolId id_;
    Id first_ = kNone, hover_ = kNone;
    Id knifeFace_ = kNone, hoverFace_ = kNone; // a top-level face as the knife
    QString message_;
    struct Cached {
        uint64_t revision = 0;
        bool solid = false;
        std::string why;
    };
    std::map<Id, Cached> cache_, flatCache_; // by definition
};

} // namespace forma
