#pragma once

#include <optional>
#include <string>
#include <vector>

namespace reed {

class Adb {
 public:
  static constexpr const char* MEDIA_PATH = "/sdcard/pcMedia/";

  static bool is_device_connected();
  static bool push(const std::string& local_path,
                   const std::string& remote_name);
  static std::optional<std::vector<std::string>> list_media();
  static bool remove(const std::string& filename);

 private:
  static std::optional<std::string> run_command(
      const std::vector<std::string>& args);
};

}  // namespace reed
