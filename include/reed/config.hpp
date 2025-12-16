#pragma once

#include <optional>
#include <string>
#include <vector>

namespace reed {

struct Config {
  std::string port;  // Empty = auto-detect
  int brightness = 100;
  int keepalive_interval = 10;
};

struct DisplayState {
  std::vector<std::string> media;
  std::string ratio = "2:1";
  std::string screen_mode = "Full Screen";
  std::string play_mode = "Single";
  int brightness = 100;
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
