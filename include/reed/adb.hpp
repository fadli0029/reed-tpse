#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace reed {

struct StorageInfo {
  uint64_t total_kb = 0;
  uint64_t used_kb = 0;
  uint64_t available_kb = 0;
  std::string mount_point;
};

class Adb {
 public:
  static constexpr const char* MEDIA_PATH = "/sdcard/pcMedia/";

  static bool is_device_connected();
  static bool push(const std::string& local_path,
                   const std::string& remote_name);
  static bool pull(const std::string& remote_name, const std::string& local_path);
  static std::optional<std::vector<std::string>> list_media();
  /** Same directory listing but includes dotfiles (for cleaning hidden composed MP4s). */
  static std::optional<std::vector<std::string>> list_media_all();
  static std::optional<StorageInfo> get_media_storage_info();
  static bool remove(const std::string& filename);

 private:
  static std::optional<std::string> run_command(
      const std::vector<std::string>& args);
};

}  // namespace reed
