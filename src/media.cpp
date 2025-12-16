#include "reed/media.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace reed {

std::string Media::get_extension(const std::string& path) {
  auto ext = fs::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  return ext;
}

std::string Media::get_basename(const std::string& path) {
  return fs::path(path).stem().string();
}

std::string Media::get_filename(const std::string& path) {
  return fs::path(path).filename().string();
}

MediaType Media::detect_type(const std::string& path) {
  auto ext = get_extension(path);

  if (ext == ".gif") {
    return MediaType::Gif;
  }

  if (ext == ".mp4" || ext == ".webm" || ext == ".mkv" || ext == ".avi" ||
      ext == ".mov") {
    return MediaType::Video;
  }

  if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" ||
      ext == ".webp") {
    return MediaType::Image;
  }

  return MediaType::Unknown;
}

std::string Media::get_converted_name(const std::string& original) {
  return get_basename(original) + ".mp4";
}

bool Media::is_ffmpeg_available() {
  return std::system("ffmpeg -version > /dev/null 2>&1") == 0;
}

bool Media::convert_gif_to_mp4(const std::string& input,
                               const std::string& output) {
  fs::create_directories(TMP_DIR);

  // Build ffmpeg command
  std::string cmd = "ffmpeg -y -i '";

  for (char c : input) {
    if (c == '\'') {
      cmd += "'\\''";
    } else {
      cmd += c;
    }
  }

  cmd += "' -movflags faststart -pix_fmt yuv420p ";
  cmd += "-vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\" '";

  for (char c : output) {
    if (c == '\'') {
      cmd += "'\\''";
    } else {
      cmd += c;
    }
  }

  cmd += "' > /dev/null 2>&1";

  int ret = std::system(cmd.c_str());
  return ret == 0 && fs::exists(output);
}

}  // namespace reed
