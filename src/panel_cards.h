#pragma once
// Report -> what the popup shows: a heading, a one-line summary and one list of cards. A card leads with its channel
// ("ON/OFF 1", "NUMBER 3") or its role ("FROM", "TO", "END"), then the name, a dim detail and an optional note. Cards
// that share a place (the panel a button is on, the controller a signal goes into) sit under one heading card.
// Pure functions so the offline panel test checks the exact strings the game shows.
#include "tracer.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace lt {

enum class CardTone { Channel, Source, End, Info, Heading };

struct Card {
    std::string badge, title, detail, note;
    CardTone tone = CardTone::Info;
    std::string group; // the heading this card sits under ("" = none)
};

namespace cards_detail {
inline std::string trim(std::string s)
{
    const auto first = s.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    s.erase(0, first);
    while (!s.empty() && (s.back() == ' ' || s.back() == ';')) s.pop_back();
    return s;
}
inline std::string upper(std::string s)
{
    for (auto& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}
inline std::string capital(std::string s)
{
    if (!s.empty()) s[0] = static_cast<char>(toupper(static_cast<unsigned char>(s[0])));
    return s;
}
inline std::string lower_first(std::string s)
{
    if (!s.empty()) s[0] = static_cast<char>(tolower(static_cast<unsigned char>(s[0])));
    return s;
}
inline bool starts(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }
inline bool ends(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
inline void erase_all(std::string& s, const std::string& what)
{
    for (size_t at; (at = s.find(what)) != std::string::npos;) s.erase(at, what.size());
}
// The "(...)" group that ends the text, if it is a whole bracket group preceded by a space; removed from the text.
inline std::string take_tail_group(std::string& text)
{
    if (text.empty() || text.back() != ')') return {};
    int depth = 0;
    for (size_t i = text.size(); i-- > 0;) {
        if (text[i] == ')') ++depth;
        else if (text[i] == '(' && --depth == 0) {
            if (i == 0 || text[i - 1] != ' ') return {};
            std::string group = text.substr(i + 1, text.size() - i - 2);
            text.erase(i - 1);
            return group;
        }
    }
    return {};
}
// "FIRE (Instrument Panel button) (while 'SW' (not linked) is on)" -> {"FIRE", {"Instrument Panel button",
// "while 'SW' (not linked) is on"}}: the top-level bracket groups at the end of the text.
inline std::pair<std::string, std::vector<std::string>> split_groups(const std::string& text)
{
    std::vector<std::string> groups;
    size_t end = text.size();
    while (end > 0 && text[end - 1] == ')') {
        int depth = 0;
        size_t open = std::string::npos;
        for (size_t i = end; i-- > 0;) {
            if (text[i] == ')') ++depth;
            else if (text[i] == '(' && --depth == 0) { open = i; break; }
        }
        if (open == std::string::npos || open == 0 || text[open - 1] != ' ') break;
        std::string group = text.substr(open + 1, end - open - 2);
        end = open - 1;
        // "... at (-5, 1, 4)": a position, kept whole as "at (-5, 1, 4)".
        const bool position = group.find_first_not_of("0123456789-, ") == std::string::npos &&
                              std::count(group.begin(), group.end(), ',') == 2;
        if (position && end >= 3 && text.compare(end - 3, 3, " at") == 0) {
            group = "at (" + group + ")";
            end -= 3;
        }
        groups.insert(groups.begin(), group);
    }
    std::string head = trim(text.substr(0, end));
    if (head.empty()) return {trim(text), {}};
    return {head, groups};
}
inline std::string join(const std::vector<std::string>& parts, const char* separator)
{
    std::string out;
    for (const auto& p : parts)
        if (!p.empty()) out += (out.empty() ? "" : separator) + p;
    return out;
}
// Signal types next to port names add nothing in a list that is already about one signal.
inline std::string tidy(std::string s)
{
    for (const char* t : {" (on/off)", " (number)", " (composite)"}) erase_all(s, t);
    return trim(s);
}
// A plain sentence: name first, then what follows ": " (or the bracket groups) as the detail.
inline Card from_text(const std::string& text)
{
    Card card;
    const auto [head, groups] = split_groups(text);
    const size_t colon = head.find(": ");
    if (colon != std::string::npos && colon < 70) {
        card.title = trim(head.substr(0, colon));
        card.detail = join({trim(head.substr(colon + 2)), join(groups, " - ")}, " - ");
    } else {
        card.title = head;
        card.detail = join(groups, " - ");
    }
    return card;
}
// One step of a source chain -> {name, what it is}; quoted = the name is a controller input's label.
struct Hop {
    std::string name, what;
    bool quoted = false;
};
inline Hop hop(const std::string& h)
{
    // "Lead ends in [MC] at OR #19: value is computed here" -> "Computed by OR #19", "in [MC]"
    const std::string computed = ": value is computed here";
    if (starts(h, "Lead ends in ") && ends(h, computed)) {
        const std::string body = h.substr(13, h.size() - 13 - computed.size());
        const size_t at = body.rfind(" at ");
        if (at != std::string::npos) return {"Computed by " + body.substr(at + 4), "in " + body.substr(0, at)};
    }
    if (starts(h, "set by a Lua script in ")) return {"Lua script", "in " + h.substr(23)};
    if (starts(h, "constant ") || h == "always on" || starts(h, "property ")) return {capital(h), ""};
    if (starts(h, "bit ")) {
        const size_t of = h.find(" of ");
        if (of != std::string::npos) {
            Hop inner = hop(h.substr(of + 4));
            inner.name = capital(h.substr(0, of + 4)) + (inner.quoted ? "'" + inner.name + "'" : inner.name);
            inner.quoted = false;
            return inner;
        }
    }
    if (!h.empty() && h[0] == '\'') {
        const size_t from = h.find("' from ");
        if (from != std::string::npos) return {h.substr(1, from - 1), "from " + tidy(h.substr(from + 7)), true};
        const std::string unlinked = "' (not linked)";
        if (ends(h, unlinked)) return {h.substr(1, h.size() - 1 - unlinked.size()), "its controller input isn't linked", true};
        if (h.size() >= 2 && h.back() == '\'' && h.find('\'', 1) == h.size() - 1) return {h.substr(1, h.size() - 2), "", true};
    }
    const Card c = from_text(h);
    return {c.title, tidy(c.detail)};
}
// A source as the tracer words it ("'FUEL' from Liquid Meter output 'Liquid Level' (number) at (1, 3, -17)",
// "... <- Lead ends in [MC] at OR #19: value is computed here (while 'SW' is on)") -> name, one line per step, note.
inline Card origin_card(std::string text)
{
    Card card;
    std::vector<std::string> notes;
    for (;;) {
        std::string copy = text;
        const std::string group = take_tail_group(copy);
        if (!(starts(group, "while ") || starts(group, "A; ") || starts(group, "B; ") || starts(group, "+"))) break;
        notes.insert(notes.begin(), group);
        text = copy;
    }
    std::vector<std::string> hops;
    for (size_t begin = 0;;) {
        const size_t arrow = text.find(" <- ", begin);
        hops.push_back(text.substr(begin, arrow == std::string::npos ? std::string::npos : arrow - begin));
        if (arrow == std::string::npos) break;
        begin = arrow + 4;
    }
    std::vector<std::string> lines;
    for (size_t i = 0; i < hops.size(); ++i) {
        const Hop h = hop(hops[i]);
        if (i == 0) {
            card.title = h.name;
            if (!h.what.empty()) lines.push_back(h.what);
        } else {
            // A later step: what feeds the controller output named on the line above.
            lines.push_back(join({h.quoted ? "'" + h.name + "'" : lower_first(h.name), h.what}, " "));
        }
    }
    card.detail = join(lines, "\n");
    for (auto& n : notes) {
        if (starts(n, "while ")) n = "Only while " + n.substr(6) + ".";
        else if (starts(n, "A; ") || starts(n, "B; ")) n = capital(n.substr(3)) + ".";
        else n += ".";
    }
    card.note = join(notes, " ");
    return card;
}
// A source named after the node the popup is about ("FORCE AUTOHOVER" on input FORCE AUTOHOVER) is named by where it
// comes from instead: "from [MAINBOARD] output 'X' at (...)" -> "[MAINBOARD] output 'X'", "at (...)".
inline void name_by_place(Card& c, const std::string& subject)
{
    if (subject.empty() || c.title != subject || !starts(c.detail, "from ")) return;
    const size_t nl = c.detail.find('\n');
    std::string place = c.detail.substr(5, nl == std::string::npos ? std::string::npos : nl - 5);
    const std::string rest = nl == std::string::npos ? "" : c.detail.substr(nl + 1);
    std::string where;
    const size_t at = place.rfind(" at (");
    if (at != std::string::npos) {
        where = place.substr(at + 1);
        place.erase(at);
    }
    c.title = place;
    c.detail = join({where, rest}, "\n");
}
inline bool is_channel(const std::string& badge) { return starts(badge, "ON/OFF ") || starts(badge, "NUMBER "); }
// A row's own label: "On 1" -> "ON/OFF 1", "Num 3" -> "NUMBER 3", "From" -> "FROM", "" -> "INFO".
inline std::string badge(const Row& row)
{
    if (starts(row.caption, "On ")) return "ON/OFF " + row.caption.substr(3);
    if (starts(row.caption, "Num ")) return "NUMBER " + row.caption.substr(4);
    return row.caption.empty() ? std::string("INFO") : upper(row.caption);
}
inline CardTone tone(const Row& row, const std::string& card_badge)
{
    if (is_channel(card_badge)) return CardTone::Channel;
    if (row.caption == "End") return CardTone::End;
    if (row.caption.empty()) return CardTone::Info;
    return CardTone::Source;
}
} // namespace cards_detail

// Every card of the report, in order, with a heading card wherever the place changes.
inline std::vector<Card> panel_cards(const Report& report)
{
    using namespace cards_detail;
    std::vector<Card> out;
    std::string group;
    auto add = [&](Card c, const std::string& card_group) {
        if (card_group != group) {
            group = card_group;
            if (!group.empty()) out.push_back({"", group, "", "", CardTone::Heading, group});
        }
        c.group = group;
        out.push_back(std::move(c));
    };
    for (const auto& row : report.rows) {
        if (row.section != RowSection::Trace) continue;
        const std::string b = badge(row);
        if (row.cards.empty()) {
            size_t begin = 0;
            for (;;) {
                const size_t nl = row.text.find('\n', begin);
                const std::string line = trim(row.text.substr(begin, nl == std::string::npos ? std::string::npos : nl - begin));
                if (!line.empty()) {
                    Card c = row.caption == "End" || row.caption.empty() ? from_text(line) : origin_card(line);
                    c.badge = b;
                    c.tone = tone(row, b);
                    add(std::move(c), "");
                }
                if (nl == std::string::npos) break;
                begin = nl + 1;
            }
            continue;
        }
        for (const auto& rc : row.cards) {
            Card c;
            if (rc.title.empty()) {
                c = origin_card(rc.detail);
                name_by_place(c, report.subject);
            } else {
                c.title = rc.title;
                c.detail = rc.detail;
            }
            c.title = trim(c.title);
            if (!rc.context.empty()) c.detail += (c.detail.empty() ? "" : "\n") + rc.context;
            c.note = join({trim(c.note), trim(rc.note)}, " ");
            c.badge = rc.badge.empty() ? b : upper(rc.badge);
            c.tone = tone(row, c.badge);
            add(std::move(c), rc.group);
        }
    }
    return out;
}

inline std::string panel_heading(const Report& report)
{
    if (!report.subject.empty()) return report.subject;
    const std::string prefix = "Link trace: ";
    std::string t = report.title.compare(0, prefix.size(), prefix) == 0 ? report.title.substr(prefix.size()) : report.title;
    return cards_detail::split_groups(t).first;
}

// "Composite input - 3 channels carry a signal - 1 lead ends"
inline std::string panel_summary(const Report& report)
{
    std::string kind = report.role;
    if (kind.empty()) {
        const auto groups = cards_detail::split_groups(report.title).second;
        kind = groups.empty() ? std::string("Link trace") : groups.back();
    }
    if (!kind.empty()) kind[0] = static_cast<char>(toupper(static_cast<unsigned char>(kind[0])));
    std::vector<std::string> parts{kind};
    if (report.channels)
        parts.push_back(std::to_string(report.channels) + (report.channels == 1 ? " channel carries a signal" : " channels carry a signal"));
    if (report.ends) parts.push_back(std::to_string(report.ends) + (report.ends == 1 ? " lead ends" : " leads end"));
    return cards_detail::join(parts, " - ");
}

// Page ranges [first, last) so each page's cards (plus gaps) fit the height. A card taller than a page gets one alone.
// headings[i]: card i is a heading; a page never ends on one (it moves to the next page with its cards).
// carry[i]: height of the heading repeated on top of a page that starts at card i (0 = none).
inline std::vector<std::pair<size_t, size_t>> paginate(const std::vector<float>& heights, float budget, float gap,
                                                       const std::vector<bool>& headings = {}, const std::vector<float>& carry = {})
{
    auto heading = [&](size_t i) { return i < headings.size() && headings[i]; };
    auto repeated = [&](size_t i) { return i < carry.size() && carry[i] > 0 ? carry[i] + gap : 0.f; };
    std::vector<std::pair<size_t, size_t>> pages;
    size_t first = 0;
    float used = 0;
    for (size_t i = 0; i < heights.size(); ++i) {
        const float add = (i == first ? 0 : gap) + heights[i];
        if (i > first && used + add > budget) {
            size_t cut = i;
            while (cut - 1 > first && heading(cut - 1)) --cut;
            pages.emplace_back(first, cut);
            first = cut;
            used = repeated(cut);
            for (size_t k = cut; k <= i; ++k) used += (k == cut ? 0 : gap) + heights[k];
        } else {
            used += add;
        }
    }
    pages.emplace_back(first, heights.size());
    return pages;
}

} // namespace lt
