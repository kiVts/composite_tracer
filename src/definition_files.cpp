#include "definition_files.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace lt {

namespace fs = std::filesystem;

namespace {
void add_mod_folders(const fs::path& root, std::vector<fs::path>& out)
{
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        const fs::path p = entry.path() / "data" / "definitions";
        if (fs::is_directory(p, ec)) out.push_back(p);
    }
}
} // namespace

DefinitionFiles::DefinitionFiles(const fs::path& game_dir)
{
    // game: ...\steamapps\common\Stormworks -> workshop: ...\steamapps\workshop\content\573090
    add_mod_folders(game_dir.parent_path().parent_path() / "workshop" / "content" / "573090", folders_);
    wchar_t* appdata = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&appdata, &length, L"APPDATA") == 0 && appdata) {
        add_mod_folders(fs::path(appdata) / "Stormworks" / "data" / "mods", folders_);
        free(appdata);
    }
    folders_.push_back(game_dir / "rom" / "data" / "definitions");
}

std::string DefinitionFiles::read(const std::string& definition_name) const
{
    if (definition_name.empty() || definition_name.find_first_of("/\\:") != std::string::npos ||
        definition_name.find("..") != std::string::npos)
        return {};
    for (const auto& folder : folders_) {
        std::ifstream f(folder / (definition_name + ".xml"), std::ios::binary);
        if (!f) continue;
        std::ostringstream text;
        text << f.rdbuf();
        return text.str();
    }
    return {};
}

} // namespace lt
