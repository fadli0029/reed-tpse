#include "reed/adb.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>

namespace reed {

namespace {
struct PipeCloser {
  void operator()(FILE* f) const {
    if (f) pclose(f);
  }
};
}  // namespace

std::optional<std::string> Adb::run_command(
    const std::vector<std::string>& args) {
  std::string cmd = "adb";
  for (const auto& arg : args) {
    cmd += " ";
    // Shell escape
    if (arg.find(' ') != std::string::npos ||
        arg.find('\'') != std::string::npos) {
      cmd += "'";
      for (char c : arg) {
        if (c == '\'') {
          cmd += "'\\''";
        } else {
          cmd += c;
        }
      }
      cmd += "'";
    } else {
      cmd += arg;
    }
  }
  cmd += " 2>&1";

  std::array<char, 4096> buffer;
  std::string result;

  std::unique_ptr<FILE, PipeCloser> pipe(popen(cmd.c_str(), "r"));
  if (!pipe) {
    return std::nullopt;
  }

  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }

  return result;
}

bool Adb::is_device_connected() {
  auto result = run_command({"devices"});
  if (!result) {
    return false;
  }

  std::istringstream iss(*result);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.find("\tdevice") != std::string::npos) {
      return true;
    }
  }

  return false;
}

bool Adb::push(const std::string& local_path, const std::string& remote_name) {
  std::string remote_path = std::string(MEDIA_PATH) + remote_name;
  auto result = run_command({"push", local_path, remote_path});

  if (!result) {
    return false;
  }

  return result->find("pushed") != std::string::npos ||
         result->find("1 file") != std::string::npos;
}

bool Adb::pull(const std::string& remote_name, const std::string& local_path) {
  std::string remote_path = std::string(MEDIA_PATH) + remote_name;
  auto result = run_command({"pull", remote_path, local_path});
  if (!result) {
    return false;
  }
  return result->find("pulled") != std::string::npos ||
         result->find("1 file") != std::string::npos;
}

std::optional<std::vector<std::string>> Adb::list_media() {
  auto result = run_command({"shell", "ls", "-1", MEDIA_PATH});

  if (!result) {
    return std::nullopt;
  }

  if (result->find("No such file") != std::string::npos ||
      result->find("error:") != std::string::npos) {
    return std::vector<std::string>{};
  }

  std::vector<std::string> files;
  std::istringstream iss(*result);
  std::string line;

  while (std::getline(iss, line)) {
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
      line.pop_back();
    }
    if (!line.empty()) {
      files.push_back(line);
    }
  }

  return files;
}

std::optional<std::vector<std::string>> Adb::list_media_all() {
  auto result = run_command({"shell", "ls", "-1A", MEDIA_PATH});

  if (!result) {
    return std::nullopt;
  }

  if (result->find("No such file") != std::string::npos ||
      result->find("error:") != std::string::npos) {
    return std::vector<std::string>{};
  }

  std::vector<std::string> files;
  std::istringstream iss(*result);
  std::string line;

  while (std::getline(iss, line)) {
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
      line.pop_back();
    }
    if (!line.empty()) {
      files.push_back(line);
    }
  }

  return files;
}

std::optional<StorageInfo> Adb::get_media_storage_info() {
  auto result = run_command({"shell", "df", "-k", "/sdcard"});
  if (!result) {
    return std::nullopt;
  }
  if (result->find("No such file") != std::string::npos ||
      result->find("error:") != std::string::npos) {
    return std::nullopt;
  }

  std::istringstream iss(*result);
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(iss, line)) {
    if (!line.empty()) lines.push_back(line);
  }
  if (lines.size() < 2) {
    return std::nullopt;
  }

  auto parse_line = [](const std::string& input) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::istringstream lss(input);
    std::string tok;
    while (lss >> tok) out.push_back(tok);
    return out;
  };

  StorageInfo info;
  auto cols = parse_line(lines.back());
  if (cols.size() < 4) {
    return std::nullopt;
  }

  try {
    if (cols.size() >= 6) {
      info.total_kb = std::stoull(cols[1]);
      info.used_kb = std::stoull(cols[2]);
      info.available_kb = std::stoull(cols[3]);
      info.mount_point = cols.back();
    } else if (cols.size() >= 5) {
      info.total_kb = std::stoull(cols[1]);
      info.used_kb = std::stoull(cols[2]);
      info.available_kb = std::stoull(cols[3]);
      info.mount_point = cols[4];
    } else {
      return std::nullopt;
    }
  } catch (...) {
    return std::nullopt;
  }

  return info;
}

bool Adb::remove(const std::string& filename) {
  if (filename.empty()) return false;
  // Basename only; path in MEDIA_PATH is fixed.
  if (filename.find('/') != std::string::npos) return false;

  std::string remote_path = std::string(MEDIA_PATH) + filename;
  // -f: no error if missing; still fails for busy/read-only with a message.
  auto result = run_command({"shell", "rm", "-f", remote_path});
  if (!result) return false;

  const std::string& out = *result;
  auto has = [&](const char* s) { return out.find(s) != std::string::npos; };
  if (has("error:") || has("Permission denied") || has("Read-only") ||
      has("No such file") || has("cannot remove") || has("not permitted") ||
      has("Is a directory")) {
    return false;
  }
  return true;
}

}  // namespace reed
