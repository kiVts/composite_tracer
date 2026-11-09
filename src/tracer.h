#pragma once
#include "xml_lite.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lt {

struct Vec3 {
    int x = 0, y = 0, z = 0;
    bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator<(const Vec3& o) const { return x != o.x ? x < o.x : y != o.y ? y < o.y : z < o.z; }
};

enum NodeType : int { kOnOff = 0, kNumber = 1, kElectric = 4, kComposite = 5, kVideo = 6, kAudio = 7 };

struct PartNode {
    std::string label, description;
    int type = 0;
    bool input = false;
    Vec3 voxel;
    int mc_component = -1; // microcontroller node: the bridge component it belongs to
};

struct McInput {
    int component = -1;
    int node = 0;
};

struct McComponent {
    int type = 0;
    int id = 0;
    bool bridge = false;
    std::map<std::string, McInput> inputs; // "in1", "inc", ... (linked inputs only)
    const xml::Element* object = nullptr;
};
struct McConsumer { int component = -1; std::string input; };

struct McNodeDef {
    std::string label, description;
    int type = 0;
    bool input = false;
    int x = 0, z = 0;
    int component = -1;
};

struct McDef {
    std::string name;
    std::vector<McNodeDef> nodes;
    std::map<int, McComponent> components;
    std::map<std::pair<int, int>, std::vector<McConsumer>> consumers; // source component/output -> linked inputs
    std::shared_ptr<xml::Element> document; // keeps `object` pointers alive
};

struct Display {
    int index = 0, type = 0, channel = 0;
    bool toggle = false, segments_on_off = false;
    int rotation = 0;
    std::string name;
};

struct Part {
    std::string definition, name; // name: the definition's display name, or the microcontroller's own name
    std::string custom_name; // vehicle-specific display label, kept separate from the part/controller name
    Vec3 vp;
    std::vector<PartNode> nodes;
    std::shared_ptr<McDef> mc;
    std::vector<Display> displays;
};

struct Link {
    int type = 0;
    Vec3 out, in;
};

// Part definitions by file name (e.g. "instrument_display"), from rom/data/definitions or mod folders.
class Definitions {
public:
    struct Def {
        bool found = false;
        std::string name;
        struct Node {
            std::string label, description;
            int type = 0;
            bool input = false;
            Vec3 position;
        };
        std::vector<Node> nodes;
    };
    using Loader = std::function<std::string(const std::string& definition_name)>;
    explicit Definitions(Loader loader) : loader_(std::move(loader)) {}
    const Def& get(const std::string& definition_name);
    size_t loaded() const { return cache_.size(); }

private:
    Loader loader_;
    std::map<std::string, Def> cache_;
};

class Vehicle {
public:
    struct Target { int part = -1, node = -1; Vec3 voxel; }; // unresolved/ambiguous link: part=-1
    bool load(std::string_view vehicle_xml, Definitions& definitions, std::string* error = nullptr);
    // The microcontroller editor works on a copy of the definition: use it for the MC part at vp. Its vehicle links
    // still go through the node voxels of the saved definition, matched to the live one by node component id.
    bool override_mc(const Vec3& vp, std::string_view mc_xml, std::string* error = nullptr);
    int find_mc(const Vec3& vp) const;
    int find_mc(const std::string& name) const; // -1 if none or not unique

    std::vector<Part> parts;
    std::vector<Link> links;
    std::shared_ptr<xml::Element> document;

    // Output node feeding the input node (part, node) through a vehicle link, as {part, node}; {-1, -1} if none.
    std::pair<int, int> source_of(int part, int node) const;
    std::vector<Target> targets_of(int part, int node) const; // all vehicle receivers, including unresolved links

private:
    std::multimap<Vec3, std::pair<int, int>> by_voxel_;
};

std::shared_ptr<McDef> parse_mc(const xml::Element& microprocessor, std::shared_ptr<xml::Element> document);

// Trace rows are what the popup shows; Details / Ports rows are kept for the report text only.
enum class RowSection { Trace, Details, Ports };
struct RowCard {
    std::string badge, title, detail, context, note;
    std::string group; // heading shown once above consecutive cards that share it ("From ...", "Shown on ...")
};
struct Row {
    std::string caption, text;
    RowSection section = RowSection::Trace;
    std::vector<RowCard> cards;
};
struct Report {
    std::string title;
    std::string subject, role;
    std::vector<Row> rows;
    int channels = 0; // channels that carry something
    int ends = 0;     // leads that could not be followed
};

// Sources for boolean/number/composite inputs and reads; channel contents and
// connected destinations for composite writes and boolean/number/composite outputs.
bool trace_supported(const McComponent& component);
Report trace_component(const Vehicle& vehicle, int part, int component_id, size_t max_rows = 256);

const char* block_name(int type);

} // namespace lt
