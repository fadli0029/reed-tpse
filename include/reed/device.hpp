#pragma once

#include <optional>
#include <string>
#include <vector>

#include "protocol.hpp"

namespace reed {

struct DeviceInfo {
  std::string product_id;
  std::string os;
  std::string serial;
  std::string app_version;
  std::string firmware;
  std::string hardware;
  std::vector<std::string> attributes;
};

struct ScreenConfig {
  std::vector<std::string> media;
  std::string screen_mode = "Full Screen";
  std::string ratio = "2:1";
  std::string play_mode = "Single";
};

class Device {
 public:
  explicit Device(const std::string& port, bool verbose = false);
  ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  // Auto-detect device by scanning /dev/ttyACM* and attempting handshake
  static std::optional<std::string> find_device(bool verbose = false);

  bool connect();
  void disconnect();
  bool is_connected() const { return fd_ >= 0; }
  const std::string& port() const { return port_; }

  std::optional<Response> send_command(const std::string& request_state,
                                       const std::string& cmd_type,
                                       const std::string& content = "",
                                       bool wait_response = true);

  std::optional<DeviceInfo> handshake();
  std::optional<Response> set_screen_config(const ScreenConfig& config);
  std::optional<Response> set_brightness(int value);
  std::optional<Response> delete_media(const std::vector<std::string>& files);

 private:
  std::string port_;
  bool verbose_;
  int fd_ = -1;
  int seq_number_ = 0;

  std::vector<uint8_t> read_response(int timeout_ms = 1000);
};

}  // namespace reed
