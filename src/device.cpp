#include "reed/device.hpp"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <thread>

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

bool has_key(const picojson::value& v, const std::string& key) {
  if (!v.is<picojson::object>()) return false;
  return v.get<picojson::object>().count(key) > 0;
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

std::optional<std::string> Device::find_device(bool verbose) {
  namespace fs = std::filesystem;

  std::vector<std::string> candidates;

  // Scan /dev for ttyACM* devices
  for (const auto& entry : fs::directory_iterator("/dev")) {
    std::string name = entry.path().filename().string();
    if (name.rfind("ttyACM", 0) == 0) {
      candidates.push_back(entry.path().string());
    }
  }

  if (candidates.empty()) {
    if (verbose) {
      std::cerr << "No /dev/ttyACM* devices found\n";
    }
    return std::nullopt;
  }

  // Sort for consistent ordering (ttyACM0, ttyACM1, ...)
  std::sort(candidates.begin(), candidates.end());

  if (verbose) {
    std::cout << "Scanning " << candidates.size() << " device(s)...\n";
  }

  // Try each device
  for (const auto& port : candidates) {
    if (verbose) {
      std::cout << "  Trying " << port << "... ";
    }

    Device dev(port, false);
    if (!dev.connect()) {
      if (verbose) {
        std::cout << "failed to open\n";
      }
      continue;
    }

    auto info = dev.handshake();
    if (info && !info->product_id.empty() && info->product_id != "unknown") {
      if (verbose) {
        std::cout << "found " << info->product_id << "\n";
      }
      return port;
    }

    if (verbose) {
      std::cout << "no response\n";
    }
  }

  return std::nullopt;
}

Device::Device(const std::string& port, bool verbose)
    : port_(port), verbose_(verbose) {}

Device::~Device() {
  disconnect();
}

bool Device::connect() {
  fd_ = open(port_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd_ < 0) {
    if (verbose_) {
      std::cerr << "Failed to open " << port_ << ": " << strerror(errno)
                << "\n";
    }
    return false;
  }

  struct termios tty;
  memset(&tty, 0, sizeof(tty));

  if (tcgetattr(fd_, &tty) != 0) {
    if (verbose_) {
      std::cerr << "tcgetattr failed: " << strerror(errno) << "\n";
    }
    close(fd_);
    fd_ = -1;
    return false;
  }

  // 115200 baud
  cfsetospeed(&tty, B115200);
  cfsetispeed(&tty, B115200);

  // 8N1, no flow control
  tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
  tty.c_cflag &= ~(PARENB | PARODD | CSTOPB);
  tty.c_cflag &= ~CRTSCTS;
  tty.c_cflag |= CLOCAL | CREAD;

  // Raw mode
  tty.c_iflag &=
      ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  tty.c_oflag &= ~OPOST;
  tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

  // Non-blocking reads
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;

  if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
    if (verbose_) {
      std::cerr << "tcsetattr failed: " << strerror(errno) << "\n";
    }
    close(fd_);
    fd_ = -1;
    return false;
  }

  tcflush(fd_, TCIOFLUSH);

  if (verbose_) {
    std::cout << "Connected to " << port_ << "\n";
  }

  return true;
}

void Device::disconnect() {
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

std::vector<uint8_t> Device::read_response(int timeout_ms) {
  std::vector<uint8_t> response;

  struct pollfd pfd;
  pfd.fd = fd_;
  pfd.events = POLLIN;

  auto start = std::chrono::steady_clock::now();

  while (true) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    if (elapsed >= timeout_ms) {
      break;
    }

    int remaining = timeout_ms - static_cast<int>(elapsed);
    int ret = poll(&pfd, 1, remaining);

    if (ret > 0 && (pfd.revents & POLLIN)) {
      uint8_t buf[256];
      ssize_t n = read(fd_, buf, sizeof(buf));
      if (n > 0) {
        response.insert(response.end(), buf, buf + n);

        // Check if we have a complete frame
        if (response.size() >= 2 && response.front() == FRAME_MARKER &&
            response.back() == FRAME_MARKER) {
          break;
        }
      }
    } else if (ret < 0 && errno != EINTR) {
      break;
    }
  }

  return response;
}

std::optional<Response> Device::send_command(const std::string& request_state,
                                             const std::string& cmd_type,
                                             const std::string& content,
                                             bool wait_response) {
  if (fd_ < 0) {
    return std::nullopt;
  }

  ++seq_number_;
  auto frame = build_frame(request_state, cmd_type, content, "1", seq_number_);

  if (verbose_) {
    std::cout << "Sending: " << cmd_type << "\n";
    std::cout << "Frame hex: ";
    for (uint8_t b : frame) {
      std::cout << std::hex << std::uppercase << std::setfill('0')
                << std::setw(2) << static_cast<int>(b);
    }
    std::cout << std::dec << "\n";
  }

  ssize_t written = write(fd_, frame.data(), frame.size());
  if (written != static_cast<ssize_t>(frame.size())) {
    if (verbose_) {
      std::cerr << "Write failed\n";
    }
    return std::nullopt;
  }

  tcdrain(fd_);

  if (!wait_response) {
    return std::nullopt;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  auto response = read_response(1000);

  if (response.empty()) {
    if (verbose_) {
      std::cout << "No response received\n";
    }
    return std::nullopt;
  }

  if (verbose_) {
    std::cout << "Response hex: ";
    for (uint8_t b : response) {
      std::cout << std::hex << std::uppercase << std::setfill('0')
                << std::setw(2) << static_cast<int>(b);
    }
    std::cout << std::dec << "\n";
  }

  auto parsed = parse_response(response);
  if (verbose_ && parsed) {
    std::cout << "Parsed: " << parsed->raw << "\n";
  }

  return parsed;
}

std::optional<DeviceInfo> Device::handshake() {
  auto response = send_command("POST", "conn", "");

  if (!response || !response->json) {
    return std::nullopt;
  }

  DeviceInfo info;
  const auto& j = *response->json;

  info.product_id = get_string(j, "productId", "unknown");
  info.os = get_string(j, "OS", "unknown");
  info.serial = get_string(j, "sn", "unknown");

  if (has_key(j, "version")) {
    const auto& v = get_value(j, "version");
    info.app_version = get_string(v, "app", "unknown");
    info.firmware = get_string(v, "firmware", "unknown");
    info.hardware = get_string(v, "hardware", "unknown");
  }

  if (has_key(j, "attribute")) {
    const auto& attr_val = get_value(j, "attribute");
    if (attr_val.is<picojson::array>()) {
      for (const auto& attr : attr_val.get<picojson::array>()) {
        if (attr.is<std::string>()) {
          info.attributes.push_back(attr.get<std::string>());
        }
      }
    }
  }

  return info;
}

std::optional<Response> Device::set_screen_config(const ScreenConfig& config) {
  // Build media array
  picojson::array media_arr;
  for (const auto& m : config.media) {
    media_arr.push_back(picojson::value(m));
  }

  // Build filter object
  picojson::object filter;
  filter["value"] = picojson::value("");
  filter["opacity"] = picojson::value(0.0);

  // Build settings object
  picojson::object settings;
  settings["position"] = picojson::value("Top");
  settings["color"] = picojson::value("#FFFFFF");
  settings["align"] = picojson::value("Center");
  settings["badges"] = picojson::value(picojson::array());
  settings["filter"] = picojson::value(filter);

  // Build config object
  picojson::object cfg;
  cfg["Type"] = picojson::value("Custom");
  cfg["id"] = picojson::value("Customization");
  cfg["screenMode"] = picojson::value(config.screen_mode);
  cfg["ratio"] = picojson::value(config.ratio);
  cfg["playMode"] = picojson::value(config.play_mode);
  cfg["media"] = picojson::value(media_arr);
  cfg["settings"] = picojson::value(settings);
  cfg["sysinfoDisplay"] = picojson::value(picojson::array());

  std::string content = picojson::value(cfg).serialize();

  // Send twice (workaround for cached config)
  send_command("POST", "waterBlockScreenId", content);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  return send_command("POST", "waterBlockScreenId", content);
}

std::optional<Response> Device::set_brightness(int value) {
  picojson::object obj;
  obj["value"] = picojson::value(static_cast<double>(value));
  std::string content = picojson::value(obj).serialize();
  return send_command("POST", "brightness", content);
}

std::optional<Response> Device::delete_media(
    const std::vector<std::string>& files) {
  picojson::array file_arr;
  for (const auto& f : files) {
    file_arr.push_back(picojson::value(f));
  }
  picojson::object obj;
  obj["include"] = picojson::value(file_arr);
  std::string content = picojson::value(obj).serialize();
  return send_command("POST", "mediaDelete", content);
}

}  // namespace reed
