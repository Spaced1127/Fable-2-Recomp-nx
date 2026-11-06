// The folder the game's disc files live in.
//
// The SD layout is self-contained:
//
//   Fable II/
//     Game_files/     default.xex, data/, nxeart/, $SystemUpdate/, ...
//     fable_2.nro
//     fable2_config.toml
//
// When Game_files/ exists beside the NRO it is the game root; otherwise the
// NRO's own folder is (a staging folder with the content copied directly
// beside the NRO). Saves, cache and logs stay beside the NRO.
#pragma once

#include <filesystem>

namespace fable2 {

inline std::filesystem::path g_game_root_path;

inline const std::filesystem::path& GameRoot() { return g_game_root_path; }

}  // namespace fable2
