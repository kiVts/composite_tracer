// Native selection supplies editor/block identity. Native writers copy current
// vehicle/MC XML on their owning thread. Only copied XML reaches the worker.
#include "editor.h"
#include "definition_files.h"
#include "native_contracts.h"
#include "supported_blocks.h"
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "port_table.h"
#include "resolve.h"
#include <windows.h>
#include <condition_variable>
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace lt::editor {
namespace {
using U = uintptr_t;
template<class T> T& field(U p, size_t off) { return *reinterpret_cast<T*>(p + off); }
struct GameString { char* data = nullptr; uint32_t length = 0, capacity = 0; };
struct Writer { U buffer = 0; uint32_t capacity = 0, padding = 0; U stream = 0, document = 0; uint32_t mode = 0; bool defaults = true; };
struct Options { U definitions = 0, context = 0; uint32_t flags = 5; };
static_assert(sizeof(GameString) == 16 && sizeof(Writer) == 40 && sizeof(Options) == 24);
using Select = void (*)(U, U, U, U);
using SaveVehicle = void (*)(U, U, U, GameString*);
using Allocate = U (*)(size_t);
using Document = U (*)(U, bool);
using Ref = void (*)(U*, U);
using WriteMc = void (*)(Writer*, Options*, U, const char*, U);
using Print = void* (*)(U*, GameString*, U, int, U);
using Free = void (*)(void*);
Select original_select = nullptr;
SaveVehicle save_vehicle = nullptr;
Allocate allocate = nullptr;
Document document = nullptr;
Ref ref = nullptr;
WriteMc write_mc = nullptr;
Print print = nullptr;
Free release = nullptr;
U editor_vtable = 0;
std::array<U, std::size(kTraceClasses)> supported_vtables{};
uint32_t context_offset = 0, definitions_offset = 0;

// A game function this build's port data can't place switches tracing off, and the log names it.
U bound(const char* what, U address)
{
    const U live = game_live_if_mapped(address);
    if (!live)
        swlog("Link tracer: %s (%llx) not found on game build %08x; tracing stays off until Link Tracer or SWML is "
              "updated for it", what, static_cast<unsigned long long>(address), game_build());
    return live;
}
template<class T> T bind(const char* what, U address) { return reinterpret_cast<T>(bound(what, address)); }

struct Snapshot { uint64_t selection = 0, session = 0, request = 0; int component = -1; Vec3 vp; bool placed = false; std::string mc; std::shared_ptr<const std::string> vehicle; };
struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Snapshot> pending;
    View view;
    uint64_t session = 0, request = 0;
    U native = 0, block = 0, part = 0;
    ULONGLONG next_refresh = 0;
    std::shared_ptr<const std::string> vehicle;
    std::string last_mc;
};
// The loader pins mod DLLs. This state outlives the detached worker at shutdown.
State& state() { static auto* s = new State; return *s; }
Report message(const std::string& text)
{
    Report r;
    r.title = "Link trace";
    r.rows.push_back({"", text});
    return r;
}

void worker()
{
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    DefinitionFiles files(std::filesystem::path(exe).parent_path());
    Definitions defs([&](const std::string& name) { return files.read(name); });
    Vehicle vehicle;
    uint64_t session = 0;
    auto& s = state();
    for (;;) {
        Snapshot work;
        {
            std::unique_lock lock(s.mutex);
            s.wake.wait(lock, [&] { return s.pending.has_value(); });
            work = std::move(*s.pending);
            s.pending.reset();
        }
        Report report;
        try {
            std::string error;
            if (session != work.session) {
                if (!vehicle.load(*work.vehicle, defs, &error)) throw std::runtime_error(error);
                session = work.session;
            }
            int part = work.placed ? vehicle.find_mc(work.vp) : -1;
            if (!work.placed) {
                Vehicle standalone;
                std::shared_ptr<xml::Element> doc(xml::parse(work.mc, &error).release());
                const auto* root = doc ? doc->child("microprocessor") : nullptr;
                if (!root) throw std::runtime_error("cannot read standalone controller");
                Part live;
                live.mc = parse_mc(*root, doc); live.name = live.mc->name;
                standalone.parts.push_back(std::move(live));
                report = trace_component(standalone, 0, work.component);
            }
            else if (part < 0) report = message("Cannot identify this controller uniquely on the vehicle.");
            else if (!vehicle.override_mc(work.vp, work.mc, &error)) report = message("Cannot read the current controller: " + error);
            else report = trace_component(vehicle, part, work.component);
        } catch (const std::exception& e) {
            session = 0;
            report = message(std::string("Cannot follow this lead: ") + e.what());
        }
        std::lock_guard lock(s.mutex);
        if (s.view.selection != work.selection || s.session != work.session || s.request != work.request) continue;
        s.view.report = std::move(report);
        ++s.view.revision;
    }
}

struct Identity { U native = 0, block = 0, part = 0, world = 0, vehicle = 0; int id = -1; Vec3 vp; };
bool supported(U block)
{
    __try {
        if (!block) return false;
        const U table = field<U>(block, 0);
        for (U allowed : supported_vtables) if (table == allowed) return true;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Verify the parent and current selection before each refresh. Guarded reads
// contain no C++ objects requiring unwinding. No native pointer reaches a worker.
bool identity(U native, U block, Identity& out)
{
    __try {
        const U editor = native - 0x2760;
        if (!native || !block || field<U>(editor, 0) != editor_vtable || field<int>(editor, 0xe7c) != 9 ||
            field<uint32_t>(native, 0x458) != 1 || field<U>(native, 0x468) != block) return false;
        out.native = native; out.block = block;
        out.id = field<int>(block, 8);
        out.world = field<U>(editor, 0x70);
        out.vehicle = field<U>(editor, 0x13c8);
        out.part = field<U>(editor, 0x9bf0);
        if (!out.world || !out.vehicle) return false;
        if (out.part) out.vp = field<Vec3>(out.part, 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::string take(GameString& text)
{
    std::string copy;
    if (text.data && text.length <= 64 * 1024 * 1024) copy.assign(text.data, text.length);
    if (text.data) release(text.data);
    text = {};
    return copy;
}
std::string mc_xml(const Identity& id)
{
    Writer writer;
    Options options;
    const U created = document(allocate(0x4a0), true);
    ref(&writer.document, created);
    GameString text;
    options.definitions = id.world + definitions_offset;
    options.context = id.world + context_offset;
    write_mc(&writer, &options, writer.document, "microprocessor", id.native + 0x118);
    U result = 0;
    print(&result, &text, writer.document, 0, 0);
    ref(&writer.document, 0);
    return take(text);
}

void capture(bool force)
{
    auto& s = state();
    U native, block;
    {
        std::lock_guard lock(s.mutex);
        if (!s.native) return;
        native = s.native; block = s.block;
    }
    Identity id;
    if (!identity(native, block, id)) { reset(); return; }
    {
        std::lock_guard lock(s.mutex);
        if (!force && GetTickCount64() < s.next_refresh) return;
        s.next_refresh = GetTickCount64() + 1000;
    }
    try {
        auto mc = mc_xml(id);
        if (mc.empty()) return;
        std::shared_ptr<const std::string> vehicle;
        bool new_session;
        {
            std::lock_guard lock(s.mutex);
            new_session = !s.vehicle || s.part != id.part;
            if (!force && !new_session && s.last_mc == mc) return;
            vehicle = s.vehicle;
        }
        if (new_session) {
            GameString text;
            save_vehicle(id.vehicle, id.world + context_offset, id.world + definitions_offset, &text);
            vehicle = std::make_shared<const std::string>(take(text));
        }
        std::lock_guard lock(s.mutex);
        if (s.native != native || s.block != block) return;
        if (new_session) { ++s.session; s.vehicle = vehicle; s.part = id.part; }
        s.last_mc = mc;
        s.pending = Snapshot{s.view.selection, s.session, ++s.request, id.id, id.vp, id.part != 0, std::move(mc), std::move(vehicle)};
        s.wake.notify_one();
    } catch (const std::exception& e) {
        std::lock_guard lock(s.mutex);
        s.view.report = message(std::string("Cannot read editor links: ") + e.what());
        ++s.view.revision;
    }
}

void select(U native, U block, U a3, U a4)
{
    original_select(native, block, a3, a4); // all four native register arguments
    if (!block) { reset(); return; }
    if (!supported(block)) { reset(); return; }
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        s.native = native; s.block = block;
        ++s.view.selection;
        s.view.report = block ? message("Following links...") : Report{};
        ++s.view.revision;
    }
    if (block) capture(true);
}
} // namespace

void refresh() { capture(false); }
void update_view(View& last)
{
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (last.selection != s.view.selection || last.revision != s.view.revision) last = s.view;
}
void reset()
{
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.native = s.block = s.part = 0;
    s.vehicle.reset(); s.last_mc.clear(); s.pending.reset();
    ++s.session; ++s.view.selection; ++s.view.revision; s.view.report = {};
}
bool install()
{
    mod_ported_to(kPortToBuild);
    editor_vtable = native_find_vtable("c_game_state_vehicle_editor");
    for (size_t i = 0; i < supported_vtables.size(); ++i) {
        supported_vtables[i] = native_find_vtable(kTraceClasses[i]);
        if (!supported_vtables[i]) {
            swlog("Link tracer: block class %s not found; tracing off", kTraceClasses[i]);
            return false;
        }
    }
    const U context = native_find_pattern(kContextPattern);
    if (!editor_vtable || !context ||
        !native_find_pattern(kEditorPattern) || !native_find_pattern(kOpenPattern) ||
        !native_find_pattern(kVehiclePattern)) {
        swlog("Link tracer: logic editor layout not recognised on game build %08x; tracing off", game_build());
        return false;
    }
    // Off unless the port data and the editor code agree.
    definitions_offset = static_cast<uint32_t>(game_field("world", 0xbb670));
    context_offset = field<uint32_t>(context, 14);
    if (!definitions_offset || field<uint32_t>(context, 7) != definitions_offset ||
        context_offset < 0x1000 || context_offset > 0x10000) {
        swlog("Link tracer: world layout not recognised on game build %08x (definitions +%x, context +%x); tracing off",
              game_build(), field<uint32_t>(context, 7), context_offset);
        return false;
    }
    save_vehicle = bind<SaveVehicle>("vehicle writer", 0x1404c1050);
    allocate = bind<Allocate>("allocator", 0x140958c28);
    document = bind<Document>("XML document", 0x140190720);
    ref = bind<Ref>("XML reference", 0x1408d03d0);
    write_mc = bind<WriteMc>("controller writer", 0x140908dd0);
    print = bind<Print>("XML printer", 0x1409382f0);
    release = bind<Free>("free", 0x1409b2d50);
    // Every part of the controller writer and the XML printer must be found, not only their entries.
    const U fragments[] = {bound("controller writer part", 0x140908df5), bound("controller writer part", 0x140908ea6),
        bound("controller writer part", 0x140908eb6), bound("controller writer part", 0x140908f70),
        bound("controller writer part", 0x140908fd6), bound("controller writer part", 0x140908fdb),
        bound("XML printer part", 0x1409382ff), bound("XML printer part", 0x140938322),
        bound("XML printer part", 0x14093849a), bound("XML printer part", 0x1409384aa)};
    for (U address : fragments) if (!address) return false;
    if (!save_vehicle || !allocate || !document || !ref || !write_mc || !print || !release ||
        !bound("component serializer", 0x140371310) || !bound("block selection", 0x1407c7880))
        return false;
    if (!hook_game(0x1407c7880, reinterpret_cast<void*>(&select), reinterpret_cast<void**>(&original_select))) {
        swlog("Link tracer: block selection hook failed; tracing off");
        return false;
    }
    std::thread(worker).detach();
    swlog("Link tracer: native selection/serialization ready (context +%x)", context_offset);
    return true;
}
} // namespace lt::editor
