#include "reed/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "reed/picojson.h"

namespace fs = std::filesystem;

namespace reed {

namespace {

std::string get_string(const picojson::value& v, const std::string& key,
                       const std::string& def = "") {
  if (!v.is<picojson::object>()) return def;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<std::string>()) return def;
  return it->second.get<std::string>();
}

int get_int(const picojson::value& v, const std::string& key, int def = 0) {
  if (!v.is<picojson::object>()) return def;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<double>()) return def;
  return static_cast<int>(it->second.get<double>());
}

const picojson::value& get_value(const picojson::value& v,
                                 const std::string& key) {
  static picojson::value null_val;
  if (!v.is<picojson::object>()) return null_val;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end()) return null_val;
  return it->second;
}

}  // namespace

std::string ConfigManager::get_config_dir() {
  const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
  if (xdg_config && *xdg_config) {
    return std::string(xdg_config) + "/reed-tpse";
  }

  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::string(home) + "/.config/reed-tpse";
  }

  return ".config/reed-tpse";
}

std::string ConfigManager::get_state_dir() {
  const char* xdg_state = std::getenv("XDG_STATE_HOME");
  if (xdg_state && *xdg_state) {
    return std::string(xdg_state) + "/reed-tpse";
  }

  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::string(home) + "/.local/state/reed-tpse";
  }

  return ".local/state/reed-tpse";
}

std::string ConfigManager::get_config_path() {
  return get_config_dir() + "/config.json";
}

std::string ConfigManager::get_state_path() {
  return get_state_dir() + "/display.json";
}

std::optional<Config> ConfigManager::load_config() {
  std::string path = get_config_path();

  if (!fs::exists(path)) {
    return Config{};
  }

  std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }

  std::ostringstream ss;
  ss << file.rdbuf();

  picojson::value json;
  std::string err = picojson::parse(json, ss.str());
  if (!err.empty()) {
    return std::nullopt;
  }

  Config config;
  config.port = get_string(json, "port", "");
  config.brightness = get_int(json, "brightness", 100);
  config.keepalive_interval = get_int(json, "keepalive_interval", 10);

  return config;
}

bool ConfigManager::save_config(const Config& config) {
  std::string dir = get_config_dir();
  fs::create_directories(dir);

  std::string path = get_config_path();
  std::ofstream file(path);
  if (!file) {
    return false;
  }

  picojson::object obj;
  obj["port"] = picojson::value(config.port);
  obj["brightness"] = picojson::value(static_cast<double>(config.brightness));
  obj["keepalive_interval"] =
      picojson::value(static_cast<double>(config.keepalive_interval));

  file << picojson::value(obj).serialize() << "\n";
  return file.good();
}

std::optional<DisplayState> ConfigManager::load_state() {
  std::string path = get_state_path();

  if (!fs::exists(path)) {
    return std::nullopt;
  }

  std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }

  std::ostringstream ss;
  ss << file.rdbuf();

  picojson::value json;
  std::string err = picojson::parse(json, ss.str());
  if (!err.empty()) {
    return std::nullopt;
  }

  DisplayState state;

  const auto& media_val = get_value(json, "media");
  if (media_val.is<picojson::array>()) {
    for (const auto& m : media_val.get<picojson::array>()) {
      if (m.is<std::string>()) {
        state.media.push_back(m.get<std::string>());
      }
    }
  }

  state.ratio = get_string(json, "ratio", "2:1");
  state.screen_mode = get_string(json, "screen_mode", "Full Screen");
  state.play_mode = get_string(json, "play_mode", "Single");
  state.brightness = get_int(json, "brightness", 100);

  return state;
}

bool ConfigManager::save_state(const DisplayState& state) {
  std::string dir = get_state_dir();
  fs::create_directories(dir);

  std::string path = get_state_path();
  std::ofstream file(path);
  if (!file) {
    return false;
  }

  picojson::array media_arr;
  for (const auto& m : state.media) {
    media_arr.push_back(picojson::value(m));
  }

  picojson::object obj;
  obj["media"] = picojson::value(media_arr);
  obj["ratio"] = picojson::value(state.ratio);
  obj["screen_mode"] = picojson::value(state.screen_mode);
  obj["play_mode"] = picojson::value(state.play_mode);
  obj["brightness"] = picojson::value(static_cast<double>(state.brightness));

  file << picojson::value(obj).serialize() << "\n";
  return file.good();
}

}  // namespace reed
