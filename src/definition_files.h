#pragma once
// Finds part definition files the way the game's own folders lay them out: rom/data/definitions under the game,
// then Steam workshop mods (steamapps/workshop/content/573090/<id>/data/definitions) and local mods
// (%APPDATA%/Stormworks/data/mods/<name>/data/definitions). Mod folders win, as in tools/vehicle_graph.py.
#include <filesystem>
#include <string>
#include <vector>

namespace lt {

class DefinitionFiles {
public:
    // game_dir: the folder holding stormworks64.exe.
    explicit DefinitionFiles(const std::filesystem::path& game_dir);
    std::string read(const std::string& definition_name) const; // "" if not found
    const std::vector<std::filesystem::path>& folders() const { return folders_; }

private:
    std::vector<std::filesystem::path> folders_;
};

} // namespace lt
