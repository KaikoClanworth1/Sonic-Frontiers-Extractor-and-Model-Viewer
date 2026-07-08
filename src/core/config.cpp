#include "config.h"
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

namespace sf {

std::string Config::path() {
    const char* appdata = getenv("APPDATA");
    std::string dir = appdata ? std::string(appdata) + "\\SonicFrontiersViewer" : ".";
#ifdef _WIN32
    _mkdir(dir.c_str());
#endif
    return dir + "\\config.ini";
}

void Config::load() {
    std::ifstream f(path());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (!v.empty() && v.back() == '\r') v.pop_back();
        if (k == "game_folder") game_folder = v;
        else if (k == "ui_scale") ui_scale = (float)atof(v.c_str());
        else if (k == "last_export_dir") last_export_dir = v;
    }
}

void Config::save() const {
    std::ofstream f(path());
    if (!f) return;
    f << "game_folder=" << game_folder << "\n";
    f << "ui_scale=" << ui_scale << "\n";
    f << "last_export_dir=" << last_export_dir << "\n";
}

} // namespace sf
