// Persisted app config (game folder, UI scale). Stored as a tiny key=value file.
#pragma once
#include <string>

namespace sf {

struct Config {
    std::string game_folder;   // path to ...\image\x64\raw (or the SonicFrontiers root)
    float ui_scale = 1.0f;
    std::string last_export_dir;

    static std::string path();          // %APPDATA%\SonicFrontiersViewer\config.ini
    void load();
    void save() const;
};

} // namespace sf
