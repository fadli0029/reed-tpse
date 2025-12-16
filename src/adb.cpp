#include "reed/adb.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <sstream>

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

bool Adb::remove(const std::string& filename) {
  std::string remote_path = std::string(MEDIA_PATH) + filename;
  auto result = run_command({"shell", "rm", remote_path});

  return result && result->find("No such file") == std::string::npos;
}

}  // namespace reed
