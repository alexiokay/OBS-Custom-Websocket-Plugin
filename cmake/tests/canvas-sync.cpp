#include "canvas_profile.hpp"
#include "sources/overlay_source.h"
#include <filesystem>
#include <chrono>
#include <iostream>
#include <stdexcept>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("vd-canvas-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "profile");
    config_t* profile = nullptr;
    try {
        const auto file = root / "profile" / "basic.ini";
        require(config_open(&profile, file.string().c_str(), CONFIG_OPEN_ALWAYS) == CONFIG_SUCCESS, "open profile");
        config_set_uint(profile, "Video", "OutputCX", 1280);
        config_set_uint(profile, "Video", "OutputCY", 720);
        require(save_canvas_profile(profile, 1080, 1920), "persist portrait base canvas");
        config_close(profile); profile = nullptr;
        require(config_open(&profile, file.string().c_str(), CONFIG_OPEN_EXISTING) == CONFIG_SUCCESS, "reopen profile");
        require(config_get_uint(profile, "Video", "BaseCX") == 1080 && config_get_uint(profile, "Video", "BaseCY") == 1920, "base survives restart");
        require(config_get_uint(profile, "Video", "OutputCX") == 1280 && config_get_uint(profile, "Video", "OutputCY") == 720, "output stays unchanged");
        std::filesystem::rename(root / "profile", root / "moved");
        require(!save_canvas_profile(profile, 1920, 1080), "failed write is reported");
        require(config_get_uint(profile, "Video", "BaseCX") == 1080 && config_get_uint(profile, "Video", "BaseCY") == 1920, "failed write retains prior profile intent");
        config_close(profile); profile = nullptr;
        require(!save_canvas_profile(nullptr, 1920, 1080), "missing profile is rejected");
        vortideck_overlay_viewport viewport;
        require(!viewport.matches(), "missing source is unconfirmed");
        viewport.observe(1920, 1080, 1920, 1080);
        require(viewport.matches(), "first source matches");
        viewport.observe(1280, 720, 1920, 1080);
        require(!viewport.matches() && viewport.count == 2 && viewport.matching_count == 1, "second source mismatch cannot be masked");
        std::filesystem::remove_all(root);
        std::cout << "Canvas profile persistence, write failure and all-source confirmation passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (profile) config_close(profile);
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
