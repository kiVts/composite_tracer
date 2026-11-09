// The link tracer popup in the microcontroller editor, built from the game's own widgets (SDK v3 native UI).
// One list, channel first: each card is a coloured badge (the channel, or FROM / TO / END), the name, a dim detail
// and an optional note; headings name the place the cards below share. The background is sized to what is shown;
// paging appears only when the cards don't fit.
#include "editor.h"
#include "panel_cards.h"
#include "panel_geometry.h"
#include "sdk_v3.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace {
using Geometry = lt::PanelGeometry;
constexpr size_t kSlots = 12; // cards per page at most (headings count, one slot kept for a repeated heading)
// Native colours are AABBGGRR.
constexpr uint32_t kPanel = 0xf5222324, kBright = 0xffffffff, kDim = 0xffb0b0b0, kNote = 0xff5ab4e6,
    kHeading = 0xffe0c080, kButton = 0xff3a3c3d;
constexpr uint32_t kTone[4] = {
    0xff7a5a2e, // channel: blue
    0xff3e6b3e, // source / destination: green
    0xff38389a, // lead ends: red
    0xff4a4a4a, // info: grey
};
constexpr float kSpacing = 1.f;

struct Slot {
    std::array<swml_ui_handle, 4> badge{}; // coloured box, one per tone; only the card's tone is shown
    swml_ui_handle badge_text = 0;         // "ON/OFF 1": a plain label over the box (a button caption may not draw)
    swml_ui_handle title = 0, detail = 0, note = 0;
    swml_ui_handle heading = 0;            // full-width accent line for heading cards
};
struct Measured {
    float title = 0, detail = 0, note = 0, total = 0;
};
struct Panel {
    swml_ui_handle container = 0, background = 0, title = 0, summary = 0, close = 0, previous = 0, next = 0, page_label = 0;
    std::array<Slot, kSlots> slots{};
    uint64_t selection = 0, revision = 0;
    size_t page = 0;
    bool dismissed = false;
    float budget = -1;
    std::vector<lt::Card> cards;
    std::vector<std::pair<size_t, size_t>> pages;
};
std::map<swml_ui_handle, Panel> panels;

bool heading(const lt::Card& card) { return card.tone == lt::CardTone::Heading; }

swml_ui_desc label(swml_ui_rect rect, const char* text, uint32_t colour)
{
    auto d = swml3::ui::description(SWML_UI_LABEL, rect, text);
    d.text_align = SWML_UI_TOP_LEFT;
    d.font_size = kSpacing;
    d.foreground = colour;
    return d;
}
swml_ui_desc block(swml_ui_rect rect, uint32_t colour)
{
    auto d = swml3::ui::description(SWML_UI_BUTTON, rect, "");
    d.background = d.hover = d.pressed = colour;
    d.font_size = kSpacing;
    return d;
}
swml_ui_desc button(swml_ui_rect rect, const char* text, uint32_t colour = kButton)
{
    auto d = swml3::ui::description(SWML_UI_BUTTON, rect, text);
    d.background = colour;
    d.font_size = kSpacing;
    return d;
}

bool create(swml_ui_handle root, Panel& p)
{
    bool ok = true;
    auto add = [&](swml_ui_handle parent, const swml_ui_desc& d, swml_ui_handle& out) {
        if (ok) ok = swml3::ui::create(parent, d, out) == SWML_OK;
    };
    auto c = swml3::ui::description(SWML_UI_CONTAINER, {-.06f, -Geometry::top, Geometry::width, .8f, SWML_UI_TOP_RIGHT});
    add(root, c, p.container);
    if (!ok) return false;
    add(p.container, block({0, 0, Geometry::width, .8f, SWML_UI_TOP_LEFT}, kPanel), p.background);
    add(p.container, label({Geometry::padding, -Geometry::title_top, Geometry::title_width, 0, SWML_UI_TOP_LEFT}, "Link trace", kBright), p.title);
    add(p.container, label({Geometry::padding, -.15f, Geometry::title_width, 0, SWML_UI_TOP_LEFT}, "", kDim), p.summary);
    add(p.container, button({-Geometry::padding + .02f, -.04f, .09f, .085f, SWML_UI_TOP_RIGHT}, "X"), p.close);
    for (auto& s : p.slots) {
        for (size_t t = 0; t < s.badge.size(); ++t)
            add(p.container, block({Geometry::padding, -.4f, Geometry::badge_width, Geometry::badge_height, SWML_UI_TOP_LEFT}, kTone[t]), s.badge[t]);
        {
            // Fixed height (not auto) so the text centres in the box; created after the boxes so it draws on top.
            auto d = label({Geometry::padding, -.4f, Geometry::badge_width, Geometry::badge_height, SWML_UI_TOP_LEFT}, "", kBright);
            d.text_align = SWML_UI_CENTER;
            add(p.container, d, s.badge_text);
        }
        add(p.container, label({Geometry::text_x, -.4f, Geometry::text_width, 0, SWML_UI_TOP_LEFT}, "", kBright), s.title);
        add(p.container, label({Geometry::text_x, -.45f, Geometry::text_width, 0, SWML_UI_TOP_LEFT}, "", kDim), s.detail);
        add(p.container, label({Geometry::text_x, -.5f, Geometry::text_width, 0, SWML_UI_TOP_LEFT}, "", kNote), s.note);
        add(p.container, label({Geometry::padding, -.4f, Geometry::heading_width, 0, SWML_UI_TOP_LEFT}, "", kHeading), s.heading);
    }
    add(p.container, button({Geometry::padding, -.6f, .26f, Geometry::button_height, SWML_UI_TOP_LEFT}, "Previous"), p.previous);
    add(p.container, button({-Geometry::padding, -.6f, .26f, Geometry::button_height, SWML_UI_TOP_RIGHT}, "Next"), p.next);
    {
        auto d = label({0, -.6f, .4f, Geometry::button_height, SWML_UI_TOP_CENTER}, "", kDim);
        d.text_align = SWML_UI_CENTER;
        add(p.container, d, p.page_label);
    }
    if (!ok) {
        swml3::ui::destroy(p.container);
        p = {};
    }
    return ok;
}

swml_ui_rect rect_of(swml_ui_handle h)
{
    swml_ui_info info{sizeof(info)};
    swml3::api()->ui_inspect(swml_self(), h, &info);
    return info.rect;
}
float height_of(swml_ui_handle h) { return rect_of(h).height; }

// Moves a widget, keeping its size (a wrapped label's height is the native measured one).
void move(swml_ui_handle h, float x, float y)
{
    auto r = rect_of(h);
    if (std::abs(r.x - x) < 1e-6f && std::abs(r.y - y) < 1e-6f) return;
    r.x = x;
    r.y = y;
    swml3::ui::rect(h, r);
}
void resize(swml_ui_handle h, const swml_ui_rect& wanted)
{
    const auto r = rect_of(h);
    if (std::abs(r.x - wanted.x) > 1e-6f || std::abs(r.y - wanted.y) > 1e-6f || std::abs(r.width - wanted.width) > 1e-6f ||
        std::abs(r.height - wanted.height) > 1e-6f || r.anchor != wanted.anchor)
        swml3::ui::rect(h, wanted);
}

// Puts the card's text in the slot and returns the native wrapped heights.
Measured fill(Slot& s, const lt::Card& card)
{
    Measured m;
    if (heading(card)) {
        swml3::ui::text(s.heading, card.title.c_str());
        m.title = m.total = height_of(s.heading);
        return m;
    }
    swml3::ui::text(s.title, card.title.c_str());
    swml3::ui::text(s.detail, card.detail.c_str());
    swml3::ui::text(s.note, card.note.c_str());
    m.title = height_of(s.title);
    m.detail = card.detail.empty() ? 0 : height_of(s.detail);
    m.note = card.note.empty() ? 0 : height_of(s.note);
    // The name is centred on the badge; detail and note follow under it.
    m.total = Geometry::title_offset + m.title + (m.detail > 0 ? Geometry::line_inner_gap + m.detail : 0) +
              (m.note > 0 ? Geometry::line_inner_gap + m.note : 0);
    m.total = std::max(m.total, Geometry::badge_height);
    return m;
}
void hide(Slot& s)
{
    for (auto b : s.badge) swml3::ui::visible(b, false);
    for (auto h : {s.badge_text, s.title, s.detail, s.note, s.heading}) swml3::ui::visible(h, false);
}
void show(Slot& s, const lt::Card& card, const Measured& m, float top)
{
    if (heading(card)) {
        hide(s);
        swml3::ui::visible(s.heading, true);
        move(s.heading, Geometry::padding, -top);
        return;
    }
    swml3::ui::visible(s.heading, false);
    const auto tone = static_cast<size_t>(card.tone);
    for (size_t t = 0; t < s.badge.size(); ++t) {
        swml3::ui::visible(s.badge[t], t == tone);
        if (t == tone) move(s.badge[t], Geometry::padding, -top);
    }
    swml3::ui::text(s.badge_text, card.badge.c_str());
    swml3::ui::visible(s.badge_text, true);
    move(s.badge_text, Geometry::padding, -top);
    swml3::ui::visible(s.title, true);
    move(s.title, Geometry::text_x, -(top + Geometry::title_offset));
    float y = top + Geometry::title_offset + m.title;
    swml3::ui::visible(s.detail, m.detail > 0);
    if (m.detail > 0) {
        y += Geometry::line_inner_gap;
        move(s.detail, Geometry::text_x, -y);
        y += m.detail;
    }
    swml3::ui::visible(s.note, m.note > 0);
    if (m.note > 0) move(s.note, Geometry::text_x, -(y + Geometry::line_inner_gap));
}

void paginate(Panel& p, const lt::editor::View& view, float budget)
{
    p.cards = lt::panel_cards(view.report);
    if (p.cards.empty()) p.cards.push_back({"INFO", "Nothing to show.", "", "", lt::CardTone::Info});
    std::vector<float> heights, carry;
    std::vector<bool> headings;
    std::map<std::string, float> heading_height;
    for (const auto& card : p.cards) {
        heights.push_back(fill(p.slots[0], card).total); // measure in the first slot
        headings.push_back(heading(card));
        if (heading(card)) heading_height[card.group] = heights.back();
        carry.push_back(!heading(card) && !card.group.empty() ? heading_height[card.group] : 0.f);
    }
    p.pages.clear();
    for (const auto& [first, last] : lt::paginate(heights, budget, Geometry::row_gap, headings, carry))
        for (size_t s = first; s < last; s += kSlots - 1) p.pages.emplace_back(s, std::min(last, s + kSlots - 1));
    p.budget = budget;
}

void ui(const swml_ui_event* event, void*)
{
    if (event->kind == SWML_UI_UNMOUNT) {
        panels.erase(event->root);
        lt::editor::reset();
        return;
    }
    lt::editor::refresh();
    const auto view = lt::editor::view();
    auto it = panels.find(event->root);
    if (it == panels.end()) {
        if (view.report.title.empty()) return;
        Panel panel;
        if (!create(event->root, panel)) return;
        it = panels.emplace(event->root, panel).first;
    }
    auto& p = it->second;
    bool changed = p.revision != view.revision;
    if (p.selection != view.selection) {
        p.selection = view.selection;
        p.page = 0;
        p.dismissed = false;
        p.pages.clear();
        changed = true;
    }
    if (event->kind == SWML_UI_CLICK) {
        if (event->element == p.close) p.dismissed = true;
        else if (event->element == p.previous && p.page) --p.page;
        else if (event->element == p.next) ++p.page;
        changed = true;
    }
    const bool visible = !p.dismissed && !view.report.title.empty();
    swml3::ui::visible(p.container, visible);
    if (!visible) return;
    const std::string title = lt::panel_heading(view.report), summary = lt::panel_summary(view.report);
    swml3::ui::text(p.title, title.c_str());
    swml3::ui::text(p.summary, summary.c_str());
    const float title_h = height_of(p.title), summary_h = height_of(p.summary);
    const float budget = Geometry::body_budget(height_of(event->root), title_h, summary_h);
    const bool resized = std::abs(budget - p.budget) > 1e-5f;
    if (!changed && !resized && !p.pages.empty()) return;
    if (p.revision != view.revision || p.pages.empty() || resized) paginate(p, view, budget);
    p.page = std::min(p.page, p.pages.size() - 1);

    // Header
    const bool paging = p.pages.size() > 1;
    const Geometry head(title_h, summary_h, 0, paging);
    move(p.summary, Geometry::padding, -(Geometry::title_top + title_h + Geometry::line_gap));

    // Cards of this page; a heading sits closer to the cards it names than to the ones above it.
    // A page that starts inside a group repeats its heading.
    const auto [first, last] = p.pages[p.page];
    std::vector<lt::Card> shown;
    if (!heading(p.cards[first]) && !p.cards[first].group.empty())
        shown.push_back({"", p.cards[first].group, "", "", lt::CardTone::Heading, p.cards[first].group});
    shown.insert(shown.end(), p.cards.begin() + first, p.cards.begin() + last);
    float y = head.header;
    for (size_t i = 0; i < kSlots; ++i) {
        auto& slot = p.slots[i];
        if (i >= shown.size()) {
            hide(slot);
            continue;
        }
        const auto& card = shown[i];
        const Measured m = fill(slot, card);
        show(slot, card, m, y);
        y += m.total + (heading(card) ? Geometry::heading_gap : Geometry::row_gap);
    }
    const float body = y - head.header - Geometry::row_gap;

    // Frame and footer
    const Geometry frame(title_h, summary_h, body, paging);
    auto container = rect_of(p.container);
    container.height = frame.height;
    resize(p.container, container);
    resize(p.background, {0, 0, Geometry::width, frame.height, SWML_UI_TOP_LEFT});
    for (auto h : {p.previous, p.next, p.page_label}) swml3::ui::visible(h, paging);
    if (paging) {
        move(p.previous, Geometry::padding, -frame.footer_top);
        move(p.next, -Geometry::padding, -frame.footer_top);
        move(p.page_label, 0, -frame.footer_top);
        swml3::ui::enabled(p.previous, p.page > 0);
        swml3::ui::enabled(p.next, p.page + 1 < p.pages.size());
        const std::string count = std::to_string(p.page + 1) + " / " + std::to_string(p.pages.size());
        swml3::ui::text(p.page_label, count.c_str());
    }
    p.revision = view.revision;
}

bool setup()
{
    const auto* api = swml3::api();
    if (!api || !(api->capabilities() & SWML_V3_CAP_UI_APPLICATION_CONTEXT) ||
        !(api->capabilities() & SWML_V3_CAP_UI_TEXT_AUTO_HEIGHT) ||
        !(api->capabilities() & SWML_V3_CAP_UI_BUTTON_RESIZE)) {
        mod_status("off: updated SDK native UI core required");
        return false;
    }
    // Enable the SDK before installing our only hook. Failed bindings leave no active consumer.
    if (api->ui_enable(swml_self()) != SWML_OK) {
        mod_status("off: native UI bindings unavailable");
        return false;
    }
    swml_handle watch = 0;
    const auto result = swml3::ui::watch("c_ui_microprocessor_editor_interface", ui, nullptr, watch);
    if (result != SWML_OK) {
        mod_status("off: native editor watch: %s", api->result_name(result));
        return false;
    }
    // This parent keeps updating while the MC screen is hidden: drop the vehicle snapshot as soon as the editor
    // goes away, even when the same screen object / controller is reused after rewiring.
    swml_handle parent_watch = 0;
    const auto parent_result = swml3::ui::watch("c_ui_vehicle_editor_interface", [](const swml_ui_event* e, void*) {
        if (e->kind == SWML_UI_UNMOUNT) lt::editor::reset();
        else lt::editor::refresh();
    }, nullptr, parent_watch);
    if (parent_result != SWML_OK) {
        mod_status("off: native editor lifetime watch: %s", api->result_name(parent_result));
        return false;
    }
    if (!lt::editor::install()) {
        mod_status("off: logic editor contract changed; see link_tracer.log");
        return false;
    }
    return true;
}
ModSetup registration{setup};
} // namespace
