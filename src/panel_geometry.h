#pragma once
#include <algorithm>

namespace lt {
struct PanelGeometry {
    // badge_width fits "ON/OFF 26-32" (0.231 wide in the game font).
    static constexpr float width = 1.5f, padding = .07f, title_top = .05f, line_gap = .012f, body_gap = .05f,
        row_gap = .035f, heading_gap = .015f, line_inner_gap = .006f, badge_width = .26f, badge_height = .085f,
        text_gap = .035f, footer_gap = .045f, button_height = .085f, top = .12f, screen_bottom = .08f, min_body = .2f,
        max_body = .95f, line_height = .048f;
    static constexpr float title_offset = (badge_height - line_height) / 2; // name centred on the badge
    static constexpr float text_x = padding + badge_width + text_gap;
    static constexpr float text_width = width - text_x - padding;
    static constexpr float title_width = width - 2 * padding - .12f; // room for the close button
    static constexpr float heading_width = width - 2 * padding;

    float header = 0, footer_top = 0, height = 0;

    PanelGeometry(float title_height, float summary_height, float body_height, bool paging)
    {
        header = title_top + title_height + line_gap + summary_height + body_gap;
        footer_top = header + std::max(0.f, body_height) + footer_gap;
        height = paging ? footer_top + button_height + padding : header + std::max(0.f, body_height) + padding;
    }
    // Height left for cards on a screen of this height (native roots can report no size: be conservative then).
    static float body_budget(float screen_height, float title_height, float summary_height)
    {
        const float available = screen_height > .7f ? screen_height - top - screen_bottom : 1.35f;
        const PanelGeometry empty(title_height, summary_height, 0, true);
        return std::clamp(available - empty.height, min_body, max_body);
    }
};
} // namespace lt
