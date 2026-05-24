#pragma once

#include <optional>
#include <string>
#include <vector>

#include "panel.hpp"

namespace reed {

struct Config {
  std::string port;  // Empty = auto-detect
  int brightness = 75;  //default lower than max setting to reduce burn-in risk on display
  int keepalive_interval = 10;
};

struct DisplayState {
  std::vector<std::string> media;
  std::string ratio = panel::kDeviceJsonRatio;
  std::string screen_mode = "Full Screen";
  std::string play_mode = "Single";
  int brightness = 75;  // default lower than max setting to reduce burn-in risk on the display
  /** Hex RGB underlay for composed transparent GIFs/images, e.g. #1a1a2e */
  std::string compose_bg_color = "#000000";
};

class ConfigManager {
 public:
  static std::string get_config_dir();
  static std::string get_state_dir();
  static std::string get_config_path();
  static std::string get_state_path();

  static std::optional<Config> load_config();
  static bool save_config(const Config& config);

  static std::optional<DisplayState> load_state();
  static bool save_state(const DisplayState& state);
};

}  // namespace reed
