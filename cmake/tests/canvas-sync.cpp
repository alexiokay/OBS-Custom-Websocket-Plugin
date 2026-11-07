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
        {
            const auto same = scaled_output_for_canvas({2560, 1440}, {2560, 1440}, {1440, 2560});
            require(same.width == 1440 && same.height == 2560, "unscaled canvas stays unscaled after rotating");
            const auto scaled = scaled_output_for_canvas({1920, 1080}, {1280, 720}, {1080, 1920});
            require(scaled.width == 720 && scaled.height == 1280, "scaled output keeps its quality and follows portrait");
            const auto upscale = scaled_output_for_canvas({1280, 720}, {1920, 1080}, {720, 1280});
            require(upscale.width == 720 && upscale.height == 1280, "output is never larger than the canvas");
            const auto odd = scaled_output_for_canvas({2560, 1440}, {1920, 1080}, {1000, 1777});
            require(odd.width % 2 == 0 && odd.height % 2 == 0, "output sizes stay even for encoders");
        }
        {
            const auto file = root / "output.ini";
            config_t* output_profile = nullptr;
            require(config_open(&output_profile, file.string().c_str(), CONFIG_OPEN_ALWAYS) == CONFIG_SUCCESS, "open output profile");
            const canvas_size output{720, 1280};
            const bool saved = save_canvas_profile(output_profile, canvas_size{1080, 1920}, &output);
            const bool persisted = config_get_uint(output_profile, "Video", "OutputCX") == 720 &&
                config_get_uint(output_profile, "Video", "OutputCY") == 1280;
            config_close(output_profile);
            require(saved && persisted, "base and output persist together");
        }
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
