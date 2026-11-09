#pragma once
// Audited against archived 0x6a9a9e20 and installed 0x6ae31989. Check these
// field contracts as well as signed function bindings; retain displacements.
namespace lt::editor {
inline constexpr char kEditorPattern[] = "83 B9 7C 0E 00 00 09 75 ?? 8B 85 B8 03 00 00 89 44 24 20 48 81 C1 60 27 00 00 E8 ?? ?? ?? ??";
inline constexpr char kOpenPattern[] = "4D 8D 86 B8 03 00 00 49 8B 96 F0 9B 00 00 48 81 C2 48 01 00 00 49 8D 8E 78 28 00 00 E8 ?? ?? ?? ??";
inline constexpr char kContextPattern[] = "49 8B 47 70 48 8D 98 70 B6 0B 00 48 8D B8 ?? ?? 00 00";
inline constexpr char kVehiclePattern[] = "49 8B BF C8 13 00 00 49 8B 5F 70";
}
