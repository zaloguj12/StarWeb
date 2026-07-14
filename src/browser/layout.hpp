#pragma once
#include "types.hpp"
#include <vector>

// Flexbox layout via Meta's Yoga engine. When the renderer hits a `display: flex`
// node it hands the direct children here to get their positions and sizes back.

struct FlexRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// Lays out the renderable direct children of `container`. Rects are relative to the
// container's content origin; the container's own size comes back in out_width/height.
void compute_flex_layout(DomNode& container,
                         const CssStyle& container_style,
                         float content_width,
                         Tab& tab,
                         std::vector<DomNode*>& out_children,
                         std::vector<FlexRect>& out_rects,
                         float& out_width,
                         float& out_height);
