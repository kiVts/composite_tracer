#include "tracer.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>

namespace lt {

namespace {

constexpr int kMaxDepth = 24;

std::string fmt(const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

Vec3 read_vec(const xml::Element* e)
{
    if (!e) return {};
    return {e->integer("x"), e->integer("y"), e->integer("z")};
}

// Column-major 3x3 from r="a,b,c,d,e,f,g,h,i"; empty = no rotation given.
bool read_rotation(const std::string& r, double m[9])
{
    if (r.empty()) return false;
    const char* p = r.c_str();
    for (int i = 0; i < 9; ++i) {
        char* end = nullptr;
        m[i] = strtod(p, &end);
        if (end == p) return false;
        p = end;
        if (*p == ',') ++p;
    }
    return true;
}

Vec3 rotate(const double m[9], const Vec3& v)
{
    auto r = [](double x) { return static_cast<int>(x < 0 ? x - 0.5 : x + 0.5); };
    return {r(m[0] * v.x + m[3] * v.y + m[6] * v.z), r(m[1] * v.x + m[4] * v.y + m[7] * v.z),
            r(m[2] * v.x + m[5] * v.y + m[8] * v.z)};
}

// "Axis 1 [$[action_left]]/[$[action_right]]" -> "Axis 1 left/right"
std::string clean_label(std::string label)
{
    for (size_t at = label.find("[$["); at != std::string::npos; at = label.find("[$[", at)) {
        const size_t end = label.find("]]", at);
        if (end == std::string::npos) break;
        std::string action = label.substr(at + 3, end - at - 3);
        if (action.compare(0, 7, "action_") == 0) action.erase(0, 7);
        std::replace(action.begin(), action.end(), '_', ' ');
        label.replace(at, end + 2 - at, action);
        at += action.size();
    }
    while (!label.empty() && (label.back() == ' ' || label.back() == ':')) label.pop_back();
    return label;
}

std::string quoted(const std::string& s) { return "'" + s + "'"; }
std::string bracket(const std::string& s) { return "[" + (s.empty() ? std::string("unnamed") : s) + "]"; }

} // namespace

const char* block_name(int type)
{
    static const char* const names[] = {"NOT", "AND", "OR", "XOR", "NAND", "NOR", "Add", "Subtract", "Multiply", "Divide",
        "Function (3 inputs)", "Clamp", "Threshold", "Memory Register", "Abs", "Constant Number", "Constant On",
        "Greater Than", "Less Than", "Property Slider", "Property Dropdown", "Numerical Junction",
        "Numerical Switchbox", "PID Controller", "SR Latch", "JK Flip Flop", "Capacitor", "Blinker", "Push to Toggle",
        "Composite Read (on/off)", "Composite Write (on/off)", "Composite Read (number)", "Composite Write (number)",
        "Property Toggle", "Property Number", "Delta", "Function (8 inputs)", "Up/Down Counter", "Modulo",
        "PID Controller (advanced)", "Composite Write (number, large)", "Composite Write (on/off, large)", "Equal",
        "Property Output (number)", "Property Output (on/off)", "Function (1 input)", "Logic Function (4 inputs)",
        "Logic Function (8 inputs)", "Pulse", "Timer (TON)", "Timer (TOF)", "Timer (RTO)", "Timer (RTF)",
        "Composite Switchbox", "Number to Composite Binary", "Composite Binary to Number", "Lua Script",
        "Video Switchbox", "Property Text", "Audio Switchbox"};
    return type >= 0 && type < static_cast<int>(sizeof(names) / sizeof(names[0])) ? names[type] : "block";
}

// ---------------------------------------------------------------------------------------------- definitions

const Definitions::Def& Definitions::get(const std::string& definition_name)
{
    auto it = cache_.find(definition_name);
    if (it != cache_.end()) return it->second;
    Def def;
    const std::string text = loader_ ? loader_(definition_name) : std::string();
    if (!text.empty()) {
        auto doc = xml::parse(text);
        const xml::Element* root = doc ? doc->child("definition") : nullptr;
        if (root) {
            def.found = true;
            def.name = root->text("name");
            if (const auto* nodes = root->child("logic_nodes")) {
                for (const auto* n : nodes->all("logic_node")) {
                    Def::Node node;
                    node.label = clean_label(n->text("label"));
                    node.description = clean_label(n->text("description"));
                    node.type = n->integer("type");
                    node.input = n->integer("mode") == 1;
                    node.position = read_vec(n->child("position"));
                    def.nodes.push_back(std::move(node));
                }
            }
        }
    }
    return cache_.emplace(definition_name, std::move(def)).first->second;
}

// ---------------------------------------------------------------------------------------------- vehicle model

std::shared_ptr<McDef> parse_mc(const xml::Element& mp, std::shared_ptr<xml::Element> document)
{
    auto def = std::make_shared<McDef>();
    def->document = std::move(document);
    def->name = mp.text("name");
    if (const auto* nodes = mp.child("nodes")) {
        for (const auto* n : nodes->all("n")) {
            const auto* node = n->child("node");
            if (!node) continue;
            McNodeDef d;
            d.component = n->integer("component_id", -1);
            d.label = node->text("label");
            d.description = node->text("description");
            d.type = node->integer("type");
            d.input = node->integer("mode") == 1;
            const auto* pos = node->child("position");
            d.x = pos ? pos->integer("x") : 0;
            d.z = pos ? pos->integer("z") : 0;
            def->nodes.push_back(std::move(d));
        }
    }
    const auto* group = mp.child("group");
    if (!group) return def;
    auto read_list = [&](const char* list, bool bridge) {
        const auto* items = group->child(list);
        if (!items) return;
        for (const auto* c : items->all("c")) {
            const auto* object = c->child("object");
            if (!object) continue;
            McComponent comp;
            comp.type = c->integer("type");
            comp.id = object->integer("id", -1);
            comp.bridge = bridge;
            comp.object = object;
            for (const auto& child : object->children) {
                const std::string& name = child->name;
                if (name.size() < 3 || name.compare(0, 2, "in") != 0) continue;
                const int from = child->integer("component_id", -1);
                if (from < 0) continue;
                comp.inputs[name] = {from, child->integer("node_index")};
            }
            if (comp.id >= 0) def->components[comp.id] = std::move(comp);
        }
    };
    read_list("components", false);
    read_list("components_bridge", true);
    for (const auto& [id, c] : def->components)
        for (const auto& [input, source] : c.inputs)
            def->consumers[{source.component, source.node}].push_back({id, input});
    return def;
}

namespace {

void collect(const xml::Element& e, const char* name, const char* stop, std::vector<const xml::Element*>& out)
{
    for (const auto& c : e.children) {
        if (c->name == name) out.push_back(c.get());
        else if (c->name != stop) collect(*c, name, stop, out);
    }
}

void add_mc_nodes(Part& part, const McDef& mc, const std::string& r)
{
    double m[9];
    const bool rotated = read_rotation(r, m);
    for (const auto& n : mc.nodes) {
        PartNode node;
        node.label = n.label;
        node.description = n.description;
        node.type = n.type;
        node.input = n.input;
        node.mc_component = n.component;
        Vec3 local{n.x, 0, n.z};
        if (rotated) local = rotate(m, local);
        node.voxel = {part.vp.x + local.x, part.vp.y + local.y, part.vp.z + local.z};
        part.nodes.push_back(std::move(node));
    }
}

} // namespace

bool Vehicle::load(std::string_view vehicle_xml, Definitions& definitions, std::string* error)
{
    parts.clear();
    links.clear();
    by_voxel_.clear();
    std::string why;
    std::unique_ptr<xml::Element> doc = xml::parse(vehicle_xml, &why);
    if (!doc) {
        if (error) *error = "vehicle XML: " + why;
        return false;
    }
    document = std::move(doc);
    const xml::Element* root = document->child("vehicle");
    if (!root) {
        if (error) *error = "no <vehicle> element";
        return false;
    }
    std::vector<const xml::Element*> components;
    collect(*root, "c", "microprocessor_definition", components);
    for (const auto* c : components) {
        const auto* o = c->child("o");
        const auto* vp = o ? o->child("vp") : nullptr;
        if (!vp) continue;
        Part part;
        part.definition = c->text("d", "01_block");
        part.custom_name = o->text("custom_name");
        part.vp = read_vec(vp);
        const std::string r = o->text("r");
        for (const auto& child : o->children) {
            if (child->name.compare(0, 8, "display_") != 0) continue;
            Display d;
            d.index = atoi(child->name.c_str() + 8);
            d.type = child->integer("type", 1); // native serializer default is Dial; explicit 0 is unused
            d.channel = child->integer("channel");
            d.toggle = child->integer("mode") != 0;
            d.segments_on_off = child->integer("mode2") != 0;
            d.rotation = child->integer("rot");
            // The native XML loader upgrades these legacy choices to the current enums.
            if (d.type == 6 || d.type == 7 || d.type == 9) {
                d.rotation = d.type == 6 ? 3 : d.type == 7 ? 1 : 2;
                d.type = 8;
            } else if (d.type == 10) { d.type = 5; d.segments_on_off = true; }
            d.name = child->text("name");
            part.displays.push_back(std::move(d));
        }
        std::stable_sort(part.displays.begin(), part.displays.end(), [](const Display& a, const Display& b) { return a.index < b.index; });
        if (part.definition == "microprocessor") {
            const auto* mp = o->child("microprocessor_definition");
            if (!mp) continue;
            part.mc = parse_mc(*mp, document);
            part.name = part.mc->name;
            add_mc_nodes(part, *part.mc, r);
        } else {
            const auto& def = definitions.get(part.definition);
            if (def.nodes.empty()) continue;
            part.name = def.name.empty() ? part.definition : def.name;
            double m[9];
            const bool rotated = read_rotation(r, m);
            const bool wheel = part.definition.find("wheel") != std::string::npos;
            const std::string t = c->text("t");
            for (const auto& n : def.nodes) {
                PartNode node;
                node.label = n.label;
                node.description = n.description;
                node.type = n.type;
                node.input = n.input;
                Vec3 q = rotated ? rotate(m, n.position) : n.position;
                if (!rotated) { // saved without r=: the default orientation (x, y, z) -> (-y, -z, x)
                    q = (wheel && t == "2") ? Vec3{q.y, -q.z, -q.x} : Vec3{-q.y, -q.z, q.x};
                }
                node.voxel = {part.vp.x + q.x, part.vp.y + q.y, part.vp.z + q.z};
                part.nodes.push_back(std::move(node));
            }
        }
        parts.push_back(std::move(part));
    }
    for (int p = 0; p < static_cast<int>(parts.size()); ++p)
        for (int n = 0; n < static_cast<int>(parts[p].nodes.size()); ++n)
            by_voxel_.emplace(parts[p].nodes[n].voxel, std::make_pair(p, n));
    std::vector<const xml::Element*> link_elements;
    collect(*root, "logic_node_link", "microprocessor_definition", link_elements);
    for (const auto* l : link_elements) {
        Link link;
        link.type = l->integer("type");
        link.out = read_vec(l->child("voxel_pos_0"));
        link.in = read_vec(l->child("voxel_pos_1"));
        links.push_back(link);
    }
    return true;
}

int Vehicle::find_mc(const Vec3& vp) const
{
    int found = -1;
    for (int i = 0; i < static_cast<int>(parts.size()); ++i)
        if (parts[i].mc && parts[i].vp == vp) {
            if (found >= 0) return -1; // do not silently trace another body's controller
            found = i;
        }
    return found;
}

int Vehicle::find_mc(const std::string& name) const
{
    int found = -1;
    for (int i = 0; i < static_cast<int>(parts.size()); ++i) {
        if (!parts[i].mc || parts[i].mc->name != name) continue;
        if (found >= 0) return -1;
        found = i;
    }
    return found;
}

bool Vehicle::override_mc(const Vec3& vp, std::string_view mc_xml, std::string* error)
{
    const int index = find_mc(vp);
    if (index < 0) {
        if (error) *error = "no microcontroller at that position";
        return false;
    }
    std::string why;
    std::shared_ptr<xml::Element> doc(xml::parse(mc_xml, &why).release());
    const xml::Element* mp = doc ? doc->child("microprocessor") : nullptr;
    if (!mp) {
        if (error) *error = doc ? "no <microprocessor> element" : "MC XML: " + why;
        return false;
    }
    Part& part = parts[index];
    auto live = parse_mc(*mp, doc);
    // Saved nodes keep their voxels (the links point there); a node only the live copy has gets none.
    for (auto& node : part.nodes) {
        const auto it = std::find_if(live->nodes.begin(), live->nodes.end(),
                                     [&](const McNodeDef& n) { return n.component == node.mc_component; });
        if (it != live->nodes.end()) node.label = it->label;
    }
    part.mc = std::move(live);
    part.name = part.mc->name;
    return true;
}

std::pair<int, int> Vehicle::source_of(int part, int node) const
{
    if (part < 0 || part >= static_cast<int>(parts.size()) || node < 0 || node >= static_cast<int>(parts[part].nodes.size()))
        return {-1, -1};
    const PartNode& in = parts[part].nodes[node];
    for (const auto& link : links) {
        if (!(link.in == in.voxel) || link.type != in.type) continue;
        std::pair<int, int> best{-1, -1};
        auto range = by_voxel_.equal_range(link.out);
        for (auto it = range.first; it != range.second; ++it) {
            const PartNode& out = parts[it->second.first].nodes[it->second.second];
            if (out.type != link.type) continue;
            if (out.input) continue;
            if (best.first >= 0 && best != it->second) return {-1, -1}; // ambiguous voxel across bodies
            best = it->second;
        }
        if (best.first >= 0) return best;
    }
    return {-1, -1};
}

std::vector<Vehicle::Target> Vehicle::targets_of(int part, int node) const
{
    std::vector<Target> result;
    if (part < 0 || part >= static_cast<int>(parts.size()) || node < 0 || node >= static_cast<int>(parts[part].nodes.size())) return result;
    const auto& out = parts[part].nodes[node];
    if (out.input) return result;
    for (const auto& link : links) {
        if (!(link.out == out.voxel) || link.type != out.type) continue;
        Target target{-1, -1, link.in};
        const auto range = by_voxel_.equal_range(link.in);
        for (auto it = range.first; it != range.second; ++it) {
            const auto& in = parts[it->second.first].nodes[it->second.second];
            if (!in.input || in.type != link.type) continue;
            if (target.part >= 0) { target.part = target.node = -1; break; }
            target.part = it->second.first; target.node = it->second.second;
        }
        const auto duplicate = std::find_if(result.begin(), result.end(), [&](const Target& t) {
            return t.part == target.part && t.node == target.node && t.voxel == target.voxel;
        });
        if (duplicate == result.end()) result.push_back(target);
    }
    return result;
}

// ---------------------------------------------------------------------------------------------- tracing

namespace {

std::string part_name(const Part& part)
{
    if (!part.custom_name.empty() && part.custom_name != part.name)
        return quoted(part.custom_name) + " (" + part.name + ")";
    return part.mc ? bracket(part.name) : part.name;
}

const char* signal_type(int type)
{
    switch (type) {
    case kOnOff: return "on/off";
    case kNumber: return "number";
    case kComposite: return "composite";
    case kElectric: return "electric power";
    case kVideo: return "video";
    case kAudio: return "audio";
    default: return "unknown signal type";
    }
}

std::string port_label(const PartNode& node)
{
    return std::string(node.type == kElectric ? "power connection " : node.input ? "input " : "output ") +
        quoted(node.label) + " (" + signal_type(node.type) + ")";
}

std::string endpoint(const Part& part, const PartNode& node)
{
    return part_name(part) + " " + port_label(node) + fmt(" at (%d, %d, %d)", part.vp.x, part.vp.y, part.vp.z);
}

bool display_output(const Display& d) { return d.type == 3 || d.type == 8 || d.type == 11; }

std::string display_type(const Display& d)
{
    switch (d.type) {
    case 0: return "Unused";
    case 1: return "Dial";
    case 2: return "Indicator";
    case 3: return d.toggle ? "Toggle Button" : "Push Button (momentary)";
    case 4: return "Digital Display";
    case 5: return "Seven Segment";
    case 8: return std::string(d.toggle ? "Toggle Arrow Button" : "Push Arrow Button (momentary)") +
        " (" + (d.rotation == 0 ? "up" : d.rotation == 1 ? "right" : d.rotation == 2 ? "down" : "left") + ")";
    case 11: return "Flip Switch (toggle)";
    case 12: return "Bar Segment";
    case 13: return "Radial Segment";
    default: return fmt("Unknown instrument type %d", d.type);
    }
}

std::string display_channels(const Display& d)
{
    const bool bits = (d.type == 5 || d.type == 12 || d.type == 13) && d.segments_on_off;
    if (bits) {
        std::string text = "on/off channels ";
        const int count = d.type == 5 ? 7 : 8;
        for (int i = 0; i < count; ++i) text += (i ? ", " : "") + std::to_string(((d.channel + i) & 31) + 1);
        return text + " (individual segments)";
    }
    return std::string(display_output(d) || d.type == 2 ? "on/off channel " : "number channel ") +
        std::to_string((d.channel & 31) + 1);
}

std::string display_description(const Display& d)
{
    return fmt("Slot %d", d.index) + (d.name.empty() ? " (unnamed)" : " " + quoted(d.name)) + ": " +
        display_type(d) + (d.type ? "; " + display_channels(d) : "");
}

std::vector<Row> panel_details(const Part& part)
{
    int inputs = 0, outputs = 0, unused = 0;
    for (const auto& d : part.displays) {
        if (!d.type) ++unused;
        else if (display_output(d)) ++outputs;
        else if (d.type == 1 || d.type == 2 || d.type == 4 || d.type == 5 || d.type == 12 || d.type == 13) ++inputs;
    }
    std::vector<Row> rows{{"Panel", part_name(part) + fmt(" at (%d, %d, %d): %d input instrument(s), %d output control(s), %d unused slot(s)",
        part.vp.x, part.vp.y, part.vp.z, inputs, outputs, unused)}};
    rows.back().section = RowSection::Details;
    std::string counts;
    for (const auto& [n, word] : {std::pair<int, const char*>{inputs, "input"}, {outputs, "output"}, {unused, "unused slot"}})
        if (n) counts += (counts.empty() ? "" : ", ") + fmt("%d %s%s", n, word, n == 1 ? "" : "s");
    rows.back().cards.push_back({"PANEL", part_name(part), counts.empty() ? std::string("no instruments") : counts,
        fmt("at (%d, %d, %d)", part.vp.x, part.vp.y, part.vp.z), ""});
    std::string ports;
    for (const auto& n : part.nodes) ports += (ports.empty() ? "" : "; ") + port_label(n);
    if (!ports.empty()) {
        rows.push_back({"Ports", ports, RowSection::Ports});
        for (const auto& n : part.nodes) rows.back().cards.push_back({n.type==kElectric ? "POWER" : n.input ? "IN" : "OUT",
            n.label, signal_type(n.type), "", ""});
    }
    for (const auto& d : part.displays) {
        std::string shared;
        if (display_output(d)) {
            int count = 0, last = d.index;
            for (const auto& other : part.displays) if (display_output(other) && (other.channel & 31) == (d.channel & 31)) {
                ++count; last = std::max(last, other.index);
            }
            if (count > 1) shared = d.index == last ? "; shared channel: this slot wins" :
                fmt("; overwritten by output slot %d on this channel", last);
        }
        const std::string direction = !d.type ? "Unused" : display_output(d) ? "Output" :
            (d.type == 1 || d.type == 2 || d.type == 4 || d.type == 5 || d.type == 12 || d.type == 13) ? "Input" : "Unknown";
        rows.push_back({!d.type ? "Unused" : display_output(d) ? "Output" :
            (d.type == 1 || d.type == 2 || d.type == 4 || d.type == 5 || d.type == 12 || d.type == 13) ? "Input" : "Unknown",
            display_description(d) + (!d.type ? "" : display_output(d) ? "; writes to 'Out Signal'" :
                (d.type == 1 || d.type == 2 || d.type == 4 || d.type == 5 || d.type == 12 || d.type == 13) ? "; reads from 'In Signal'" : "; direction unknown") + shared,
                RowSection::Details});
        std::string note;
        if (!shared.empty()) {
            int last = d.index;
            for (const auto& other : part.displays)
                if (display_output(other) && (other.channel & 31) == (d.channel & 31)) last = std::max(last, other.index);
            note = d.index == last ? "Shares its channel with another slot; this slot wins."
                                   : fmt("Overwritten by slot %d on the same channel.", last);
        }
        rows.back().cards.push_back({(!d.type ? "EMPTY " : display_output(d) ? "OUT " : "IN ") + std::to_string(d.index),
            d.name.empty() ? "Unnamed instrument" : d.name,
            display_type(d) + (d.type ? " - " + display_channels(d) : ""),
            !d.type ? "" : display_output(d) ? "writes to Out Signal" : "reads from In Signal", note});
    }
    return rows;
}

// 0..31 on/off 1..32, 32..63 number 1..32
struct Composite {
    std::vector<std::string> channel[64];
    std::vector<std::string> wholes; // sources whose channels aren't known individually
    std::vector<std::string> ends;   // leads that could not be followed
    std::vector<Row> details;
    std::map<std::string,RowCard> origin_cards;
};

void add_unique(std::vector<std::string>& to, const std::string& s)
{
    if (std::find(to.begin(), to.end(), s) == to.end()) to.push_back(s);
}

// " (while 'SW' is on)" -> "Only while 'SW' is on."
std::string condition_note(std::string c)
{
    while (!c.empty() && c.front() == ' ') c.erase(0, 1);
    if (c.size() > 2 && c.front() == '(' && c.back() == ')') c = c.substr(1, c.size() - 2);
    if (!c.empty()) c[0] = static_cast<char>(toupper(static_cast<unsigned char>(c[0])));
    if (c.compare(0, 5, "While") == 0) c = "Only while" + c.substr(5);
    return c.empty() || c.back() == '.' ? c : c + ".";
}

void merge(Composite& into, const Composite& from, const std::string& condition)
{
    for (const auto& row : from.details)
        if (std::none_of(into.details.begin(), into.details.end(), [&](const Row& r) { return r.caption == row.caption && r.text == row.text; }))
            into.details.push_back(row);
    for (int c = 0; c < 64; ++c)
        for (const auto& o : from.channel[c]) {
            add_unique(into.channel[c], o + condition);
            const auto card = from.origin_cards.find(o);
            if (card != from.origin_cards.end()) {
                auto copy = card->second;
                if (!condition.empty()) copy.note += (copy.note.empty() ? "" : " ") + condition_note(condition);
                into.origin_cards[o+condition] = std::move(copy);
            }
        }
    for (const auto& w : from.wholes) add_unique(into.wholes, w + condition);
    for (const auto& e : from.ends) add_unique(into.ends, e);
}

std::string channel_name(int c) { return c < 32 ? fmt("on/off %d", c + 1) : fmt("number %d", c - 31); }
// The popup's channel label: 0..31 -> "ON/OFF 1".."ON/OFF 32", 32..63 -> "NUMBER 1".."NUMBER 32"; none -> "TO".
std::string channel_badge(int c) { return c < 0 ? std::string("TO") : c < 32 ? fmt("ON/OFF %d", c + 1) : fmt("NUMBER %d", c - 31); }
// The channel(s) an instrument shows, as a badge: "NUMBER 1", "ON/OFF 2", "ON/OFF 1-7" (individual segments).
std::string display_badge(const Display& d)
{
    const int first = (d.channel & 31) + 1;
    if ((d.type == 5 || d.type == 12 || d.type == 13) && d.segments_on_off) {
        const int last = first + (d.type == 5 ? 7 : 8) - 1;
        return last <= 32 ? fmt("ON/OFF %d-%d", first, last) : fmt("ON/OFF %d+", first);
    }
    return fmt(display_output(d) || d.type == 2 ? "ON/OFF %d" : "NUMBER %d", first);
}
bool display_input(const Display& d)
{
    return d.type == 1 || d.type == 2 || d.type == 4 || d.type == 5 || d.type == 12 || d.type == 13;
}
// Does this instrument read channel c (0..63) of In Signal?
bool display_reads(const Display& d, int c)
{
    if (!display_input(d)) return false;
    const int first = d.channel & 31;
    if ((d.type == 5 || d.type == 12 || d.type == 13) && d.segments_on_off) {
        const int count = d.type == 5 ? 7 : 8;
        return c < 32 && ((c - first) & 31) < count;
    }
    return c == (d.type == 2 ? first : 32 + first);
}

struct LuaWrites {
    std::set<int> numbers, bools;
    bool dynamic = false;
};

// setNumber(k, ...) / setNumber(k+1, ...) inside "for k=a,b do": channels a(+1)..b(+1). arg = start of the argument.
bool loop_channels(const std::string& script, size_t call, size_t arg, std::set<int>& out)
{
    size_t e = arg;
    while (e < script.size() && (isalnum(static_cast<unsigned char>(script[e])) || script[e] == '_')) ++e;
    if (e == arg || isdigit(static_cast<unsigned char>(script[arg]))) return false;
    const std::string var = script.substr(arg, e - arg);
    int add = 0;
    while (e < script.size() && script[e] == ' ') ++e;
    if (e < script.size() && (script[e] == '+' || script[e] == '-')) {
        const int sign = script[e] == '+' ? 1 : -1;
        ++e;
        while (e < script.size() && script[e] == ' ') ++e;
        if (e >= script.size() || !isdigit(static_cast<unsigned char>(script[e]))) return false;
        add = sign * atoi(script.c_str() + e);
        while (e < script.size() && isdigit(static_cast<unsigned char>(script[e]))) ++e;
        while (e < script.size() && script[e] == ' ') ++e;
    }
    if (e >= script.size() || script[e] != ',') return false;
    // The nearest "for <var>=a,b" before the call.
    for (size_t f = script.rfind("for", call); f != std::string::npos; f = f ? script.rfind("for", f - 1) : std::string::npos) {
        size_t k = f + 3;
        if (f > 0 && (isalnum(static_cast<unsigned char>(script[f - 1])) || script[f - 1] == '_')) continue;
        while (k < script.size() && script[k] == ' ') ++k;
        if (script.compare(k, var.size(), var) != 0) continue;
        k += var.size();
        while (k < script.size() && script[k] == ' ') ++k;
        if (k >= script.size() || script[k] != '=') continue;
        char* end = nullptr;
        const long a = strtol(script.c_str() + k + 1, &end, 10);
        if (end == script.c_str() + k + 1) return false;
        while (*end == ' ') ++end;
        if (*end != ',') return false;
        const char* bstart = end + 1;
        const long b = strtol(bstart, &end, 10);
        if (end == bstart || a < -64 || a > 64 || b < -64 || b > 64) return false;
        while (*end == ' ') ++end;
        if (std::strncmp(end, "do", 2) != 0) return false; // no variable bounds or non-unit step
        // A closed loop earlier in the script does not bind a later call. Be
        // conservative about nested blocks rather than inventing channel numbers.
        const size_t body = static_cast<size_t>(end - script.c_str()) + 2;
        for (size_t token = body; token < call;) {
            if (!isalpha(static_cast<unsigned char>(script[token])) && script[token] != '_') { ++token; continue; }
            size_t last = token + 1;
            while (last < call && (isalnum(static_cast<unsigned char>(script[last])) || script[last] == '_')) ++last;
            if (script.compare(token, last - token, "end") == 0 || script.compare(token, last - token, "until") == 0)
                return false;
            token = last;
        }
        for (long ch = a + add; ch <= b + add; ++ch)
            if (ch >= 1 && ch <= 32) out.insert(static_cast<int>(ch));
        return true;
    }
    return false;
}

// output.setNumber(3, ...) / o.setBool(1, ...) with aliases such as "o=output" or "local out = output".
// Ignore comments and quoted/long strings: examples or disabled calls are not
// outputs. Preserve positions so the small literal-loop analysis sees code only.
std::string lua_code(std::string script)
{
    auto blank = [&](size_t a, size_t b) { for (size_t i = a; i < b; ++i) script[i] = ' '; };
    for (size_t i = 0; i < script.size();) {
        const size_t start = i;
        bool comment = script.compare(i, 2, "--") == 0;
        if (comment) i += 2;
        if (i >= script.size()) { blank(start, i); break; }
        if (script[i] == '[') {
            size_t j = i + 1;
            while (j < script.size() && script[j] == '=') ++j;
            if (j < script.size() && script[j] == '[') {
                const std::string close = "]" + std::string(j - i - 1, '=') + "]";
                const size_t end = script.find(close, j + 1);
                i = end == std::string::npos ? script.size() : end + close.size();
                blank(start, i); continue;
            }
        }
        if (comment) {
            const size_t end = script.find('\n', i);
            i = end == std::string::npos ? script.size() : end;
            blank(start, i); continue;
        }
        if (script[i] == '\'' || script[i] == '"') {
            const char quote = script[i++];
            while (i < script.size()) {
                const char c = script[i++];
                if (c == '\\' && i < script.size()) ++i;
                else if (c == quote) break;
            }
            blank(start, i); continue;
        }
        if (isspace(static_cast<unsigned char>(script[i]))) script[i] = ' ';
        ++i;
    }
    return script;
}

LuaWrites lua_writes(const std::string& source)
{
    const std::string script = lua_code(source);
    LuaWrites w;
    std::vector<std::string> names{"output"};
    for (size_t at = script.find("output"); at != std::string::npos; at = script.find("output", at + 6)) {
        // "<alias> = output" (not "output.") : walk back over spaces and '=' to an identifier
        const size_t after = at + 6;
        if (after < script.size() && (isalnum(static_cast<unsigned char>(script[after])) || script[after] == '_' ||
                                      script[after] == '.'))
            continue;
        size_t i = at;
        while (i > 0 && script[i - 1] == ' ') --i;
        if (i == 0 || script[i - 1] != '=' || (i > 1 && (script[i - 2] == '=' || script[i - 2] == '~'))) continue;
        --i;
        while (i > 0 && script[i - 1] == ' ') --i;
        size_t end = i;
        while (i > 0 && (isalnum(static_cast<unsigned char>(script[i - 1])) || script[i - 1] == '_')) --i;
        if (i < end) names.push_back(script.substr(i, end - i));
    }
    for (const auto& name : names) {
        for (const char* method : {".setNumber", ".setBool"}) {
            const std::string needle = name + method;
            for (size_t at = script.find(needle); at != std::string::npos; at = script.find(needle, at + 1)) {
                if (at > 0 && (isalnum(static_cast<unsigned char>(script[at - 1])) || script[at - 1] == '_' ||
                               script[at - 1] == '.'))
                    continue;
                size_t i = at + needle.size();
                while (i < script.size() && script[i] == ' ') ++i;
                if (i >= script.size() || script[i] != '(') continue;
                ++i;
                while (i < script.size() && script[i] == ' ') ++i;
                size_t digits = i;
                while (digits < script.size() && isdigit(static_cast<unsigned char>(script[digits]))) ++digits;
                size_t j = digits;
                while (j < script.size() && script[j] == ' ') ++j;
                auto& set = method[4] == 'N' ? w.numbers : w.bools;
                if (digits > i && j < script.size() && script[j] == ',') {
                    const int ch = atoi(script.c_str() + i);
                    if (ch >= 1 && ch <= 32) set.insert(ch);
                } else if (!loop_channels(script, at, i, set)) {
                    w.dynamic = true;
                }
            }
        }
    }
    return w;
}

// Per-channel text from a part node's description: "(On/Off 31 : Trigger)", "(Value 1 : A/D)", "[1] x position".
bool description_channels(const std::string& text, const std::string& suffix, Composite& out)
{
    bool any = false;
    auto trim = [](std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == ',' || s.back() == '.')) s.pop_back();
        size_t b = 0;
        while (b < s.size() && s[b] == ' ') ++b;
        return clean_label(s.substr(b));
    };
    for (size_t at = text.find('('); at != std::string::npos; at = text.find('(', at + 1)) {
        const size_t close = text.find(')', at);
        if (close == std::string::npos) break;
        const std::string inner = text.substr(at + 1, close - at - 1);
        const size_t colon = inner.find(':');
        if (colon == std::string::npos) continue;
        std::string head = inner.substr(0, colon);
        head.erase(0, head.find_first_not_of(' ') == std::string::npos ? head.size() : head.find_first_not_of(' '));
        int base = -1;
        if (head.compare(0, 6, "On/Off") == 0) base = 0;
        else if (head.compare(0, 5, "Value") == 0 || head.compare(0, 6, "Number") == 0) base = 32;
        if (base < 0) continue;
        const size_t digit = head.find_first_of("0123456789");
        if (digit == std::string::npos) continue;
        const int ch = atoi(head.c_str() + digit);
        if (ch < 1 || ch > 32) continue;
        const bool range = head.find('+') != std::string::npos;
        std::string what = trim(inner.substr(colon + 1));
        if (range) what += fmt(" (from %s on)", channel_name(base + ch - 1).c_str());
        add_unique(out.channel[base + ch - 1], what + suffix);
        any = true;
    }
    for (size_t at = text.find('['); at != std::string::npos; at = text.find('[', at + 1)) {
        if (at + 1 >= text.size() || !isdigit(static_cast<unsigned char>(text[at + 1]))) continue;
        const size_t close = text.find(']', at);
        if (close == std::string::npos) break;
        const int ch = atoi(text.c_str() + at + 1);
        size_t end = text.find_first_of(",[", close + 1);
        if (end == std::string::npos) end = text.size();
        const std::string what = trim(text.substr(close + 1, end - close - 1));
        if (ch < 1 || ch > 32 || what.empty()) continue;
        add_unique(out.channel[32 + ch - 1], what + suffix);
        any = true;
    }
    // "On/Off 1-8 : Target 1 Found, Target 2 Found, etc... Values 1-32 : Target 1 Distance, ..., etc..." (radars):
    // a list, continued by the step of the first number in its items when it ends in "etc".
    for (const auto& [word, base] : {std::pair<const char*, int>{"On/Off ", 0}, {"Values ", 32}}) {
        for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
            const char* p = text.c_str() + at + strlen(word);
            char* end = nullptr;
            const long first = strtol(p, &end, 10);
            if (end == p || *end != '-') continue;
            const char* q = end + 1;
            const long last = strtol(q, &end, 10);
            if (end == q || first < 1 || last > 32 || last < first) continue;
            const size_t colon = text.find(':', end - text.c_str());
            if (colon == std::string::npos || colon - (end - text.c_str()) > 3) continue;
            size_t stop = text.find("etc", colon);
            const size_t next_on = text.find("On/Off ", colon), next_values = text.find("Values ", colon);
            const size_t next = std::min(next_on, next_values);
            const bool etc = stop != std::string::npos && stop < next;
            if (!etc) stop = std::min(next, text.size());
            std::vector<std::string> items;
            std::string list = text.substr(colon + 1, stop - colon - 1);
            for (size_t s = 0; s <= list.size();) {
                size_t comma = list.find(',', s);
                if (comma == std::string::npos) comma = list.size();
                std::string item = trim(list.substr(s, comma - s));
                if (!item.empty()) items.push_back(item);
                s = comma + 1;
            }
            if (items.empty()) continue;
            // Period: the first item whose number differs from item 0's ("Target 1 ..." -> "Target 2 ...").
            auto number_of = [](const std::string& s, size_t& pos) {
                pos = s.find_first_of("0123456789");
                return pos == std::string::npos ? -1 : atoi(s.c_str() + pos);
            };
            size_t pos0 = 0;
            const int n0 = number_of(items[0], pos0);
            size_t period = items.size();
            for (size_t k = 1; k < items.size(); ++k) {
                size_t pk = 0;
                if (number_of(items[k], pk) != n0) {
                    period = k;
                    break;
                }
            }
            for (long ch = first; ch <= last; ++ch) {
                const size_t k = static_cast<size_t>(ch - first);
                std::string item;
                if (k < items.size()) item = items[k];
                else if (etc && n0 >= 0) {
                    item = items[k % period];
                    size_t pos = 0;
                    const int n = number_of(item, pos);
                    const size_t len = item.find_first_not_of("0123456789", pos) - pos;
                    item.replace(pos, len, std::to_string(n + static_cast<int>(k / period)));
                } else break;
                add_unique(out.channel[base + ch - 1], item + suffix);
                any = true;
            }
        }
    }
    return any;
}

// Composite outputs whose layout the game doesn't describe in the definition. Monitor touch: as the game's Lua
// examples and the players' scripts read it (inputX = input.getNumber(3), inputY = input.getNumber(4)).
bool known_channels(const Part& part, const PartNode& node, const std::string& suffix, Composite& out)
{
    if (part.name.compare(0, 7, "Monitor") == 0 && node.label == "Touch Output") {
        const char* numbers[] = {"screen width", "screen height", "touch 1 X", "touch 1 Y", "touch 2 X", "touch 2 Y"};
        for (int i = 0; i < 6; ++i) out.channel[32 + i].push_back(numbers[i] + suffix);
        out.channel[0].push_back("touch 1 pressed" + suffix);
        out.channel[1].push_back("touch 2 pressed" + suffix);
        return true;
    }
    return false;
}

class Tracer {
public:
    explicit Tracer(const Vehicle& v) : v_(v) {}

    // The composite output (component, node index) inside the MC of part p.
    Composite mc_output(int p, int component, int node, int depth)
    {
        Composite out;
        const Part& part = v_.parts[p];
        const std::string mc = bracket(part.name);
        if (++steps_ > 2048) {
            out.ends.push_back("Lead ends in " + mc + ": trace limit reached in a branching circuit");
            return out;
        }
        if (depth > kMaxDepth) {
            out.ends.push_back("Lead ends in " + mc + ": the chain is too long to follow");
            return out;
        }
        const auto key = std::make_tuple(p, component, node);
        if (active_.count(key)) {
            out.ends.push_back("Lead loops back on itself in " + mc);
            return out;
        }
        const auto it = part.mc->components.find(component);
        if (it == part.mc->components.end()) {
            out.ends.push_back("Lead ends in " + mc + ": a link points at a missing block");
            return out;
        }
        active_.insert(key);
        const McComponent& c = it->second;
        const auto input = [&](const char* name) -> const McInput* {
            const auto i = c.inputs.find(name);
            return i == c.inputs.end() ? nullptr : &i->second;
        };
        if (c.bridge) {
            if (c.type == 4) out = mc_input_node(p, c.id, depth);
            else out.ends.push_back("Lead ends in " + mc + ": a link points at an output node");
        } else {
            switch (c.type) {
            case 53: { // composite switchbox
                const McInput* a = input("in1");
                const McInput* b = input("in2");
                const McInput* s = input("in3");
                const std::string sw = s ? scalar(p, *s, depth + 1) : "switch input (unlinked)";
                if (a && b && a->component == b->component && a->node == b->node) {
                    out = mc_output(p, a->component, a->node, depth + 1);
                    break; // identical branches carry the same source in either state
                }
                if (!a && !b) {
                    out.ends.push_back("Lead ends in " + mc + " due to a switchbox: neither A nor B is linked");
                    break;
                }
                if (a) merge(out, mc_output(p, a->component, a->node, depth + 1),
                             s ? " (while " + sw + " is on)" : " (A; needs switch on, control is unlinked)");
                if (b) merge(out, mc_output(p, b->component, b->node, depth + 1),
                             s ? " (while " + sw + " is off)" : " (B; switch control is unlinked/off)");
                break;
            }
            case 40:
            case 41: { // composite write, large
                if (const McInput* inc = input("inc")) out = mc_output(p, inc->component, inc->node, depth + 1);
                const int base = c.type == 40 ? 32 : 0;
                const int count = std::clamp(c.object->integer("count", 32), 0, 32);
                const int offset = c.object->integer("offset", 0);
                if (offset < 0) out.ends.push_back("Lead ends in " + mc + ": Composite Write picks channels at run time");
                for (int k = 0; k < count; ++k) {
                    const McInput* in = input(fmt("in%d", k + 1).c_str());
                    if (!in) continue;
                    if (offset < 0) {
                        add_unique(out.wholes, scalar(p, *in, depth + 1) + fmt(" (write input %d; its channel is set at run time)", k + 1));
                        continue;
                    }
                    auto& slot = out.channel[base + ((offset + k) & 31)];
                    slot.clear();
                    slot.push_back(scalar(p, *in, depth + 1));
                }
                break;
            }
            case 30:
            case 32: { // composite write
                if (const McInput* pass = input("in1")) out = mc_output(p, pass->component, pass->node, depth + 1);
                const int ch = c.object->integer("i", 0);
                const McInput* value = input("in2");
                if (ch < 0) {
                    if (value) add_unique(out.wholes, scalar(p, *value, depth + 1) + " (written to a channel set at run time)");
                    out.ends.push_back("Lead ends in " + mc + ": Composite Write picks a channel at run time");
                    break;
                }
                auto& slot = out.channel[(c.type == 32 ? 32 : 0) + (ch & 31)];
                slot.clear();
                if (value) slot.push_back(scalar(p, *value, depth + 1));
                break;
            }
            case 54: { // number to composite binary
                const McInput* in = input("in1");
                if (in) {
                    const auto value = scalar(p, *in, depth + 1);
                    for (int bit = 0; bit < 32; ++bit) out.channel[bit].push_back(fmt("bit %d of ", bit + 1) + value);
                }
                else out.ends.push_back("Lead ends in " + mc + ": its Number to Composite Binary has no number linked");
                break;
            }
            case 56: { // Lua script, composite output = node 0
                if (node != 0) {
                    out.ends.push_back("Lead ends in " + mc + ": a link points at a Lua script's video output");
                    break;
                }
                const LuaWrites w = lua_writes(c.object->text("script"));
                const std::string by = "set by a Lua script in " + mc;
                for (int ch : w.bools) out.channel[ch - 1].push_back(by);
                for (int ch : w.numbers) out.channel[31 + ch].push_back(by);
                if (w.dynamic) out.ends.push_back("Lead ends in " + mc + " due to a Lua script: it picks some channels at run time");
                else if (w.bools.empty() && w.numbers.empty())
                    out.ends.push_back("Lead ends in " + mc + ": this Lua script's output channels cannot be determined statically");
                break;
            }
            default:
                out.ends.push_back(fmt("Lead ends in %s at a %s (#%d): it doesn't output a composite", mc.c_str(),
                                       block_name(c.type), c.id));
            }
        }
        active_.erase(key);
        bool limited = false;
        for (auto& channel : out.channel) {
            if (channel.size() > 8) { channel.resize(8); limited = true; }
            for (auto& origin : channel) if (origin.size() > 512) {
                size_t end = 512;
                while (end && (static_cast<unsigned char>(origin[end]) & 0xc0) == 0x80) --end;
                origin.resize(end); origin += "..."; limited = true;
            }
        }
        if (limited) add_unique(out.ends, "Lead ends in " + mc + ": alternate paths are too numerous or long; details shortened");
        return out;
    }

    // A composite input node (bridge type 4) of the MC in part p: follow its vehicle link out.
    Composite mc_input_node(int p, int bridge_id, int depth)
    {
        Composite out;
        const Part& part = v_.parts[p];
        int node = -1;
        for (int n = 0; n < static_cast<int>(part.nodes.size()); ++n)
            if (part.nodes[n].mc_component == bridge_id) node = n;
        const std::string label = node_label(part, bridge_id);
        if (node < 0) {
            out.ends.push_back("Input " + quoted(label) + " of " + bracket(part.name) +
                               " is new: links to it exist only after you leave the microcontroller editor");
            return out;
        }
        if (part.nodes[node].type != kComposite || !part.nodes[node].input) {
            out.ends.push_back("Input " + quoted(label) + " of " + bracket(part.name) + ": type or direction changed; vehicle links refresh after leaving the controller editor");
            return out;
        }
        const auto [sp, sn] = v_.source_of(p, node);
        if (sp < 0) {
            const auto& input = part.nodes[node];
            const bool linked = std::any_of(v_.links.begin(), v_.links.end(), [&](const Link& l) {
                return l.in == input.voxel && l.type == input.type;
            });
            out.ends.push_back(linked ? "Lead ends at input " + quoted(label) + " of " + bracket(part.name) +
                ": connected source node is missing or ambiguous (check part definitions)" :
                "Input " + quoted(label) + " of " + bracket(part.name) + " isn't linked on the vehicle");
            return out;
        }
        return part_output(sp, sn, depth + 1);
    }

    // A composite output node of a vehicle part.
    Composite part_output(int p, int n, int depth)
    {
        Composite out;
        const Part& part = v_.parts[p];
        const PartNode& node = part.nodes[n];
        if (part.mc) {
            const auto it = part.mc->components.find(node.mc_component);
            if (it == part.mc->components.end() || !it->second.bridge || it->second.type != 5) {
                out.ends.push_back("Lead ends in " + bracket(part.name) + ": output " + quoted(node.label) + " not found");
                return out;
            }
            const auto in = it->second.inputs.find("in1");
            if (in == it->second.inputs.end()) {
                out.ends.push_back("Lead ends in " + bracket(part.name) + ": nothing inside is linked to output " + quoted(node.label));
                return out;
            }
            return mc_output(p, in->second.component, in->second.node, depth + 1);
        }
        const std::string where = bracket(part.name);
        if (!part.displays.empty() && node.label == "Out Signal") {
            const auto key = std::make_tuple(p, n, -1);
            if (depth > kMaxDepth || ++steps_ > 2048 || active_.count(key)) {
                out.ends.push_back("Lead ends at " + where + ": panel pass-through loops or exceeds the trace limit");
                return out;
            }
            active_.insert(key);
            for (int input = 0; input < static_cast<int>(part.nodes.size()); ++input) {
                if (!part.nodes[input].input || part.nodes[input].type != kComposite) continue;
                const auto [sp, sn] = v_.source_of(p, input);
                if (sp >= 0) out = part_output(sp, sn, depth + 1);
                else if (std::any_of(v_.links.begin(), v_.links.end(), [&](const Link& l) {
                    return l.type == kComposite && l.in == part.nodes[input].voxel;
                })) out.ends.push_back("Lead ends at " + where + ": panel input source is missing or ambiguous");
            }
            const auto details = panel_details(part);
            out.details.insert(out.details.end(), details.begin(), details.end());
            for (const auto& d : part.displays) {
                if (!display_output(d)) continue;
                const int ch = d.channel & 31;
                out.channel[ch].clear(); // native slot order: the last button assigned to a channel wins
                const std::string name = d.name.empty() ? fmt("button %d", d.index) : d.name;
                const std::string origin = name + " (" + part.name + " button) - output: " + display_type(d) +
                    fmt("; slot %d; writes on/off channel %d", d.index, ch + 1) +
                    (part.custom_name.empty() ? "" : " on " + quoted(part.custom_name));
                add_unique(out.channel[ch], origin);
                std::string shared;
                for (const auto& other : part.displays) if (display_output(other) && other.index!=d.index && (other.channel&31)==ch)
                    shared += (shared.empty() ? "Also assigned: " : ", ") + (other.name.empty() ? fmt("slot %d",other.index) : other.name);
                if (!shared.empty()) shared += fmt(". Slot %d takes priority.",d.index);
                out.origin_cards[origin] = {"", name, display_type(d) + fmt(" - slot %d", d.index), "", shared,
                                            "From " + part_name(part)};
            }
            active_.erase(key);
            if (std::all_of(std::begin(out.channel), std::end(out.channel), [](const auto& c) { return c.empty(); }) &&
                out.wholes.empty() && out.ends.empty())
                out.ends.push_back("Lead ends at " + where + ": no buttons or linked composite input supply its Out Signal");
            return out;
        }
        // Data that comes in from outside the vehicle: connectors, winches / hooks and radio receivers.
        const bool receive = node.label.find("Receive") != std::string::npos || node.label.find("Recv") != std::string::npos ||
                             node.description.find("receive from the connected") != std::string::npos;
        if (receive) {
            const bool radio = node.description.find("received composite") != std::string::npos ||
                               part.definition.compare(0, 3, "rx_") == 0;
            out.ends.push_back("Lead ends at " + where + " " + quoted(node.label) +
                               (radio ? ": it arrives by radio" : ": it comes from whatever is connected to it"));
            return out;
        }
        const std::string suffix = " (" + part.name + ")";
        if (!known_channels(part, node, suffix, out) && !description_channels(node.description, suffix, out))
            out.wholes.push_back(part.name + " " + quoted(node.label) + " (a game part; its channels aren't listed)");
        return out;
    }

    // Short description of what feeds a number / on-off input.
    std::string scalar(int p, const McInput& in, int depth)
    {
        const Part& part = v_.parts[p];
        const auto key = std::make_tuple(p, in.component, in.node);
        if (depth > kMaxDepth || ++steps_ > 2048) return "Lead ends in " + bracket(part.name) + ": scalar trace limit reached";
        if (scalar_active_.count(key)) return "Lead loops back on itself in " + bracket(part.name);
        scalar_active_.insert(key);
        struct Guard { decltype(scalar_active_)& set; decltype(key) key; ~Guard() { set.erase(key); } } guard{scalar_active_, key};
        const auto it = part.mc->components.find(in.component);
        if (it == part.mc->components.end()) return "Lead ends in " + bracket(part.name) + ": a link points at a missing block";
        const McComponent& c = it->second;
        if (c.bridge) {
            const std::string label = quoted(node_label(part, c.id));
            if (c.type != 0 && c.type != 2) return label;
            int node = -1;
            for (int n = 0; n < static_cast<int>(part.nodes.size()); ++n)
                if (part.nodes[n].mc_component == c.id) node = n;
            if (node < 0) return label + " (new input)";
            if (!part.nodes[node].input || part.nodes[node].type != (c.type == 2 ? kNumber : kOnOff))
                return "Input " + label + " of " + bracket(part.name) + ": type or direction changed; vehicle links refresh after leaving the controller editor";
            const auto [sp, sn] = v_.source_of(p, node);
            if (sp < 0) {
                const auto& input = part.nodes[node];
                const bool linked = std::any_of(v_.links.begin(), v_.links.end(), [&](const Link& l) { return l.in == input.voxel && l.type == input.type; });
                return linked ? "Lead ends in " + bracket(part.name) + ": connected source node is missing or ambiguous" : label + " (not linked)";
            }
            const Part& src = v_.parts[sp];
            const PartNode& out = src.nodes[sn];
            const std::string from = label + " from " + endpoint(src, out);
            if (!src.mc) return from;
            const auto output = src.mc->components.find(out.mc_component);
            if (output == src.mc->components.end()) return from + ": output block is missing";
            if (!output->second.bridge || output->second.type != (c.type == 2 ? 3 : 1)) return from + ": output block changed signal type";
            const auto feed = output->second.inputs.find("in1");
            if (feed == output->second.inputs.end()) return from + " (nothing linked inside)";
            return from + " <- " + scalar(sp, feed->second, depth + 1);
        }
        const xml::Element* o = c.object;
        switch (c.type) {
        case 15: {
            const auto* n = o->child("n");
            return "constant " + (n ? n->text("value", n->text("text", "0")) : std::string("0"));
        }
        case 16: return "always on";
        case 21: { // numerical junction: in1 = value, in2 = control; output 0 on, output 1 off
            const auto value = c.inputs.find("in1");
            if (value == c.inputs.end()) return "Lead ends in " + bracket(part.name) + ": Numerical Junction has no value linked";
            return scalar(p, value->second, depth + 1) + (in.node == 0 ? " (junction on branch)" : " (junction off branch)");
        }
        case 22: { // numerical switchbox: in1 A, in2 B, in3 on/off control (native serializers/tick audited)
            std::string sources;
            const auto a = c.inputs.find("in1"), b = c.inputs.find("in2");
            if (a != c.inputs.end()) sources = scalar(p, a->second, depth + 1) + " (A; switch on)";
            if (b != c.inputs.end()) sources += (sources.empty() ? "" : " | ") + scalar(p, b->second, depth + 1) + " (B; switch off)";
            return sources.empty() ? "Lead ends in " + bracket(part.name) + " due to a switchbox: neither A nor B is linked" : sources;
        }
        case 19: case 20: case 33: case 34: case 58:
            return "property " + quoted(o->text("n", block_name(c.type)));
        case 29:
        case 31: {
            const auto src = c.inputs.find("in1");
            const int ch = o->integer("i", 0);
            const int base = c.type == 31 ? 32 : 0;
            if (src == c.inputs.end()) return fmt("%s #%d (no composite linked)", block_name(c.type), c.id);
            if (ch < 0) return fmt("a %s channel picked at run time", c.type == 31 ? "number" : "on/off");
            const Composite comp = mc_output(p, src->second.component, src->second.node, depth + 1);
            const auto& origins = comp.channel[base + (ch & 31)];
            if (origins.empty()) {
                if (!comp.ends.empty()) return comp.ends[0];
                if (!comp.wholes.empty()) return comp.wholes[0];
                return "an empty " + channel_name(base + (ch & 31));
            }
            return origins.size() == 1 ? origins[0] : origins[0] + fmt(" (+%zu more)", origins.size() - 1);
        }
        default:
            return fmt("Lead ends in %s at %s #%d: value is computed here", bracket(part.name).c_str(), block_name(c.type), c.id);
        }
    }

    static std::string node_label(const Part& part, int bridge_id)
    {
        for (const auto& n : part.mc->nodes)
            if (n.component == bridge_id) return n.label;
        return fmt("node #%d", bridge_id);
    }

private:
    const Vehicle& v_;
    std::set<std::tuple<int, int, int>> active_;
    std::set<std::tuple<int, int, int>> scalar_active_;
    size_t steps_ = 0;
};

void render(const Composite& comp, int first, int last, Report& r, size_t max_rows)
{
    max_rows = std::max(size_t(1), max_rows);
    size_t hidden = 0;
    auto push = [&](std::string caption, std::string text, RowSection section = RowSection::Trace, std::vector<RowCard> cards = {}) {
        if (r.rows.size() + 1 >= max_rows) {
            ++hidden;
            return;
        }
        r.rows.push_back({std::move(caption), std::move(text), section, std::move(cards)});
    };
    for (int c = first; c <= last; ++c) {
        if (comp.channel[c].empty()) continue;
        ++r.channels;
        std::string text;
        std::vector<RowCard> cards;
        // Each alternative stays a separate paragraph in the native report.
        // A shared channel is not a single name separated by a wrapping pipe.
        for (const auto& o : comp.channel[c]) {
            text += (text.empty() ? "" : "\n") + o;
            const auto found = comp.origin_cards.find(o);
            // No card: leave the title empty and let the panel split "name (where) (when)" itself.
            RowCard card = found == comp.origin_cards.end() ? RowCard{"", "", o, "", ""} : found->second;
            card.badge = channel_badge(c);
            cards.push_back(std::move(card));
        }
        push(c < 32 ? fmt("On %d", c + 1) : fmt("Num %d", c - 31), text, RowSection::Trace, std::move(cards));
    }
    for (const auto& w : comp.wholes) {
        ++r.channels;
        push("From", w);
    }
    for (const auto& e : comp.ends) {
        ++r.ends;
        push("End", e);
    }
    for (const auto& detail : comp.details) push(detail.caption, detail.text, detail.section, detail.cards);
    if (hidden) r.rows.push_back({"", fmt("+%zu more lines", hidden)});
}

int bridge_kind(int type)
{
    return type < 2 ? kOnOff : type < 4 ? kNumber : kComposite;
}

// Forward traversal describes wiring/dependencies, not simulated signal values.
// Cross controller boundaries and known routers; name a computation/control
// input explicitly when it cannot be treated as a transparent signal path.
//
// Every row keeps its report text; rows the popup shows also carry a card: the channel (once a Composite Read picks
// one), the receiver, what the signal passed through, and a heading for where it entered (a controller input, a panel).
class Destinations {
public:
    struct Ctx {
        int channel = -1;  // 0..63 after a Composite Read with a fixed channel
        std::string group; // heading: where the signal entered
        std::string via;   // what it passed through on the way
        uint64_t mine = ~0ull; // composite channels (bit c = channel c) that still carry this signal
        bool quiet = false;    // reached only through a panel pass-through: say nothing when it leads nowhere
        bool carries(int c) const { return c < 0 || (mine >> c & 1); }
        Ctx through(const std::string& step) const { Ctx c = *this; c.via += (c.via.empty() ? "" : ", ") + step; return c; }
    };

    Destinations(const Vehicle& vehicle, Report& report, size_t max_rows) : v_(vehicle), report_(report), max_(std::max(size_t(2), max_rows)) {}

    void output(int p, int component, int node, int kind, int depth = 0, const std::string& path = "", Ctx ctx = {})
    {
        const auto& part = v_.parts[p];
        const std::string where = bracket(part.name);
        if (ctx.group.empty()) ctx.group = "Inside " + where;
        if (depth > kMaxDepth || ++steps_ > 2048 || report_.rows.size() + 1 >= max_) {
            if (!limited_) { limited_ = true; ++report_.ends; report_.rows.push_back({"End", "Lead ends in " + where + ": destination trace limit reached"}); }
            return;
        }
        const auto key = std::make_tuple(p, component, node, kind);
        if (active_.count(key)) { end("Lead loops back on itself in " + where + " while following destinations"); return; }
        active_.insert(key);
        const auto found = part.mc->consumers.find({component, node});
        if (found == part.mc->consumers.end()) {
            const auto self = part.mc->components.find(component);
            const std::string what = self == part.mc->components.end() ? fmt("block #%d", component) :
                self->second.bridge ? "input " + quoted(Tracer::node_label(part, component)) : fmt("%s #%d", block_name(self->second.type), component);
            row("To", where + fmt(" block #%d output %d has no linked consumers", component, node + 1) + path,
                ctx.quiet ? RowSection::Details : RowSection::Trace, {card(ctx, "Nothing uses it", "nothing is linked to " + what)});
        } else for (const auto& edge : found->second) {
            if (limited_) break;
            const auto target = part.mc->components.find(edge.component);
            if (target == part.mc->components.end()) { end("Lead ends in " + where + ": destination block is missing"); continue; }
            const auto& c = target->second;
            const std::string name = block_name(c.type) + fmt(" #%d", c.id);
            const std::string label = where + " " + name;
            const std::string via = path + " via " + name;
            if (c.bridge) {
                if (c.type == 1 || c.type == 3 || c.type == 5) {
                    if (bridge_kind(c.type) == kind) external(p, c.id, kind, depth + 1, path, ctx);
                    else end("Lead ends in " + where + ": output node has a different signal type");
                } else end("Lead ends in " + where + ": link points into an input node");
                continue;
            }
            if (c.type == 53 && kind == kComposite && (edge.input == "in1" || edge.input == "in2")) {
                const bool a = edge.input == "in1";
                output(p, c.id, 0, kind, depth + 1, via + (a ? " (A; switch on)" : " (B; switch off)"),
                       ctx.through(name + (a ? " (A, while on)" : " (B, while off)")));
            } else if ((c.type == 30 || c.type == 32) &&
                ((edge.input == "in1" && kind == kComposite) || (edge.input == "in2" && kind == (c.type == 30 ? kOnOff : kNumber)))) {
                const int ch = c.object->integer("i", 0);
                const int written = (c.type == 32 ? 32 : 0) + (ch & 31);
                Ctx next = ctx.through(edge.input == "in2" && ch >= 0 ? name + " (as " + channel_name(written) + ")" :
                                       name + " (rewrites a channel)");
                next.channel = -1;
                if (edge.input == "in2") next.mine = ch >= 0 ? 1ull << written : 0;
                else if (ch >= 0) next.mine &= ~(1ull << written); // in2 (or 0 when unlinked) replaces it
                if (!next.mine) continue;
                output(p, c.id, 0, kComposite, depth + 1, via + " (channels rewritten)", next);
            } else if ((c.type == 40 || c.type == 41) &&
                ((edge.input == "inc" && kind == kComposite) || (kind == (c.type == 40 ? kNumber : kOnOff) &&
                  edge.input.size() > 2 && std::isdigit(static_cast<unsigned char>(edge.input[2])) &&
                  std::atoi(edge.input.c_str() + 2) <= std::clamp(c.object->integer("count", 32), 0, 32)))) {
                std::string step = name + " (rewrites channels)";
                const int offset = c.object->integer("offset", 0);
                const int base = c.type == 40 ? 32 : 0;
                const int count = std::clamp(c.object->integer("count", 32), 0, 32);
                Ctx next = ctx;
                if (edge.input != "inc" && offset >= 0) {
                    const int written = base + ((offset + std::atoi(edge.input.c_str() + 2) - 1) & 31);
                    step = name + " (as " + channel_name(written) + ")";
                    next.mine = 1ull << written;
                } else if (edge.input != "inc") {
                    next.mine = c.type == 40 ? ~0ull << 32 : ~0ull >> 32;
                } else if (offset >= 0) {
                    for (int k = 0; k < count; ++k)
                        if (c.inputs.count(fmt("in%d", k + 1))) next.mine &= ~(1ull << (base + ((offset + k) & 31)));
                }
                next = next.through(step);
                next.channel = -1;
                if (!next.mine) continue;
                output(p, c.id, 0, kComposite, depth + 1, via + " (channels rewritten)", next);
            } else if ((c.type == 29 || c.type == 31) && kind == kComposite && edge.input == "in1") {
                const int channel = c.object->integer("i");
                const std::string read = channel < 0 ? " (channel chosen at run time)" :
                    " (" + channel_name((c.type == 31 ? 32 : 0) + (channel & 31)) + ")";
                row("To", label + read + path, RowSection::Details);
                if (channel >= 0 && !ctx.carries((c.type == 31 ? 32 : 0) + (channel & 31))) continue;
                Ctx next = channel < 0 ? ctx.through(name + " (channel picked at run time)") : ctx;
                next.channel = channel < 0 ? -1 : (c.type == 31 ? 32 : 0) + (channel & 31);
                next.quiet = false;
                output(p, c.id, 0, c.type == 31 ? kNumber : kOnOff, depth + 1, via + read, next);
            } else if (c.type == 22 && kind == kNumber && (edge.input == "in1" || edge.input == "in2")) {
                const bool a = edge.input == "in1";
                output(p, c.id, 0, kind, depth + 1, via + (a ? " (A; switch on)" : " (B; switch off)"),
                       ctx.through(name + (a ? " (A, while on)" : " (B, while off)")));
            } else if (c.type == 21 && kind == kNumber && edge.input == "in1") {
                output(p, c.id, 0, kind, depth + 1, via + " (on branch)", ctx.through(name + " (on branch)"));
                output(p, c.id, 1, kind, depth + 1, via + " (off branch)", ctx.through(name + " (off branch)"));
            } else {
                const bool control = (c.type == 53 || c.type == 22) && edge.input == "in3";
                row("To", label + " " + quoted(edge.input) + (control ? " (switch control; value is not passed through)" :
                    " (lead ends here: signal is processed by this block)") + path, RowSection::Trace,
                    {card(ctx, name, control ? "switches between its A and B" : "uses it on " + quoted(edge.input))});
            }
        }
        active_.erase(key);
    }

    void external(int p, int bridge, int kind, int depth = 0, const std::string& path = "", const Ctx& ctx = {})
    {
        const auto& part = v_.parts[p];
        int node = -1;
        for (int i = 0; i < static_cast<int>(part.nodes.size()); ++i)
            if (part.nodes[i].mc_component == bridge && !part.nodes[i].input && part.nodes[i].type == kind) node = i;
        const std::string source = part_name(part) + " " + quoted(Tracer::node_label(part, bridge));
        if (node < 0) {
            row("To", source + ": new output; vehicle links become available after leaving the controller editor", RowSection::Trace,
                {card(ctx, "New output " + quoted(Tracer::node_label(part, bridge)), "its links show after you leave the controller editor")});
            return;
        }
        Ctx out = ctx;
        out.group.clear();
        part_targets(p, node, kind, depth, path, out);
    }

    void part_targets(int p, int node, int kind, int depth, const std::string& path, const Ctx& ctx = {})
    {
        const auto& part = v_.parts[p];
        const std::string source = endpoint(part, part.nodes[node]);
        const auto key = std::make_tuple(p, node, kind);
        if (depth > kMaxDepth || ++steps_ > 2048 || panel_active_.count(key)) {
            end("Lead loops back on itself or exceeds the destination trace limit at " + part_name(part)); return;
        }
        panel_active_.insert(key);
        struct Guard { decltype(panel_active_)& set; decltype(key) key; ~Guard() { set.erase(key); } } guard{panel_active_, key};
        const auto targets = v_.targets_of(p, node);
        if (targets.empty()) {
            row("To", source + " is not linked on the vehicle" + path, ctx.quiet ? RowSection::Details : RowSection::Trace,
                {card(ctx, "Not linked on the vehicle", part_name(part) + " output " + quoted(part.nodes[node].label))});
            return;
        }
        for (const auto& target : targets) {
            if (limited_) break;
            if (target.part < 0) {
                end("Lead ends after " + source + fmt(": destination at (%d, %d, %d) is missing or ambiguous",
                    target.voxel.x, target.voxel.y, target.voxel.z));
                continue;
            }
            const auto& receiver = v_.parts[target.part];
            const auto& input = receiver.nodes[target.node];
            const std::string text = endpoint(receiver, input) + path;
            if (receiver.mc) {
                row("To", text, RowSection::Details);
                const auto bridge_in = receiver.mc->components.find(input.mc_component);
                if (bridge_in == receiver.mc->components.end() || !bridge_in->second.bridge ||
                    bridge_in->second.type != (kind == kComposite ? 4 : kind == kNumber ? 2 : 0)) {
                    end("Lead ends in " + bracket(receiver.name) + ": receiving input block is missing or changed");
                    continue;
                }
                Ctx inside = ctx;
                inside.group = "Into " + part_name(receiver) + " input " + quoted(input.label) +
                               (ctx.via.empty() ? "" : " via " + ctx.via);
                inside.via.clear();
                output(target.part, input.mc_component, 0, kind, depth + 1, path, inside);
            } else if (!receiver.displays.empty() && kind == kComposite) {
                // A panel: list the instruments that show channels of it.
                row("To", text, RowSection::Details);
                for (const auto& detail : panel_details(receiver)) row(detail.caption, detail.text, RowSection::Details);
                Ctx shown = ctx;
                shown.group = "Shown on " + part_name(receiver);
                std::vector<RowCard> readers;
                for (const auto& d : receiver.displays) {
                    if (!display_input(d) || (ctx.channel >= 0 && !display_reads(d, ctx.channel))) continue;
                    bool carried = false;
                    for (int ch = 0; ch < 64 && !carried; ++ch) carried = ctx.carries(ch) && display_reads(d, ch);
                    if (!carried) continue;
                    RowCard rc = card(shown, d.name.empty() ? std::string("Unnamed instrument") : d.name,
                                      display_type(d) + fmt(" - slot %d", d.index));
                    rc.badge = display_badge(d);
                    readers.push_back(std::move(rc));
                }
                if (readers.empty()) readers.push_back(card(shown, "No instrument shows it", "input " + quoted(input.label)));
                row("Shown", text + " (instruments)", ctx.quiet ? RowSection::Details : RowSection::Trace, std::move(readers));
                Ctx pass = ctx.through(part_name(receiver) + "'s Out Signal");
                pass.quiet = true;
                for (const auto& d : receiver.displays)
                    if (display_output(d)) pass.mine &= ~(1ull << (d.channel & 31));
                for (int out = 0; out < static_cast<int>(receiver.nodes.size()) && pass.mine; ++out) {
                    const auto& next = receiver.nodes[out];
                    if (!next.input && next.type == kComposite && next.label == "Out Signal")
                        part_targets(target.part, out, kind, depth + 1, path + " (via panel composite pass-through; button channels overwritten)", pass);
                }
            } else {
                if (!receiver.displays.empty())
                    for (const auto& detail : panel_details(receiver)) row(detail.caption, detail.text, RowSection::Details);
                row("To", text, RowSection::Trace,
                    {card(ctx, part_name(receiver), "input " + quoted(input.label) +
                          fmt(" at (%d, %d, %d)", receiver.vp.x, receiver.vp.y, receiver.vp.z))});
            }
        }
    }

private:
    static RowCard card(const Ctx& ctx, std::string title, std::string detail)
    {
        return {channel_badge(ctx.channel), std::move(title), std::move(detail), ctx.via.empty() ? "" : "via " + ctx.via, "", ctx.group};
    }
    void row(const std::string& caption, const std::string& text, RowSection section = RowSection::Trace, std::vector<RowCard> cards = {})
    {
        if (limited_) return;
        if (report_.rows.size() + 1 >= max_) {
            limited_ = true; ++report_.ends;
            report_.rows.push_back({"End", "Destination list shortened: too many connected branches"}); return;
        }
        if (std::none_of(report_.rows.begin(), report_.rows.end(), [&](const Row& r) { return r.caption == caption && r.text == text; }))
            report_.rows.push_back({caption, text, section, std::move(cards)});
    }
    void end(const std::string& text) { if (!limited_) { ++report_.ends; row("End", text); } }
    const Vehicle& v_;
    Report& report_;
    size_t max_, steps_ = 0;
    bool limited_ = false;
    std::set<std::tuple<int, int, int, int>> active_;
    std::set<std::tuple<int, int, int>> panel_active_;
};

} // namespace

bool trace_supported(const McComponent& c)
{
    return c.bridge ? c.type >= 0 && c.type <= 5 :
        c.type == 29 || c.type == 31 || c.type == 30 || c.type == 32 || c.type == 40 || c.type == 41;
}

Report trace_component(const Vehicle& vehicle, int part, int component_id, size_t max_rows)
{
    Report r;
    if (part < 0 || part >= static_cast<int>(vehicle.parts.size()) || !vehicle.parts[part].mc) return r;
    const Part& p = vehicle.parts[part];
    const auto it = p.mc->components.find(component_id);
    if (it == p.mc->components.end()) return r;
    const McComponent& c = it->second;
    if (c.bridge) {
        r.subject = Tracer::node_label(p,c.id);
        r.role = std::string(signal_type(c.type<2 ? kOnOff : c.type<4 ? kNumber : kComposite)) +
            ((c.type&1) ? " output" : " input");
    }
    Tracer tracer(vehicle);
    if (c.bridge && (c.type == 0 || c.type == 2)) {
        r.title = "Link trace: " + Tracer::node_label(p, c.id) + (c.type == 2 ? " (number input)" : " (on/off input)");
        const std::string from = tracer.scalar(part, {c.id, 0}, 0);
        r.rows.push_back({"From", part_name(p) + ": " + from, RowSection::Trace, {{"FROM", "", from, "", ""}}});
        return r;
    }
    if ((c.bridge && (c.type == 1 || c.type == 3 || c.type == 5)) ||
        (!c.bridge && (c.type == 30 || c.type == 32 || c.type == 40 || c.type == 41))) {
        r.title = "Link trace: " + (c.bridge ? Tracer::node_label(p, c.id) +
            " (" + signal_type(bridge_kind(c.type)) + " output)" : std::string(block_name(c.type)) + fmt(" #%d", c.id));
        if (!c.bridge || c.type == 5) {
            const auto feed = c.inputs.find("in1");
            const Composite comp = c.bridge ? (feed == c.inputs.end() ? Composite{} :
                tracer.mc_output(part, feed->second.component, feed->second.node, 0)) : tracer.mc_output(part, c.id, 0, 0);
            render(comp, 0, 63, r, max_rows);
            if (r.rows.empty()) r.rows.push_back({"From", "No channel sources are linked inside this output."});
        } else {
            const auto feed = c.inputs.find("in1");
            r.rows.push_back({"From", feed == c.inputs.end() ? "Nothing is linked inside this output." : tracer.scalar(part, feed->second, 0)});
            if (feed != c.inputs.end()) r.rows.back().cards.push_back({"FROM", "", r.rows.back().text, "", ""});
        }
        Destinations destinations(vehicle, r, max_rows);
        if (c.bridge) destinations.external(part, c.id, bridge_kind(c.type));
        else destinations.output(part, c.id, 0, kComposite);
        return r;
    }
    if (c.bridge && c.type == 4) {
        r.title = "Link trace: " + Tracer::node_label(p, c.id) + " (composite input)";
        const Composite comp = tracer.mc_input_node(part, c.id, 0);
        render(comp, 0, 63, r, max_rows);
        if (r.rows.empty()) r.rows.push_back({"", "Nothing arrives on any channel."});
        return r;
    }
    if (!c.bridge && (c.type == 29 || c.type == 31)) {
        const int base = c.type == 31 ? 32 : 0;
        const int ch = c.object->integer("i", 0);
        r.title = ch < 0 ? std::string("Link trace: channel from node") : "Link trace: " + channel_name(base + (ch & 31));
        const auto src = c.inputs.find("in1");
        if (src == c.inputs.end()) {
            r.rows.push_back({"", "No composite is linked to this read."});
            return r;
        }
        const Composite comp = tracer.mc_output(part, src->second.component, src->second.node, 0);
        if (ch < 0) render(comp, base, base + 31, r, max_rows); // every channel the node could pick
        else {
            render(comp, base + (ch & 31), base + (ch & 31), r, max_rows);
            if (r.channels == 0 && r.ends == 0)
                r.rows.insert(r.rows.begin(), {"", "Nothing arrives on " + channel_name(base + (ch & 31)) + "."});
        }
        if (r.rows.empty()) r.rows.push_back({"", "Nothing arrives on any channel."});
        return r;
    }
    return r;
}

} // namespace lt
