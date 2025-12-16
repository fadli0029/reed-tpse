#pragma once

#include <string>

namespace reed {

enum class MediaType { Unknown, Video, Gif, Image };

class Media {
 public:
  static constexpr const char* TMP_DIR = "/tmp/reed-tpse/";

  static MediaType detect_type(const std::string& path);
  static std::string get_extension(const std::string& path);
  static std::string get_basename(const std::string& path);
  static std::string get_filename(const std::string& path);
  static std::string get_converted_name(const std::string& original);
  static bool convert_gif_to_mp4(const std::string& input,
                                 const std::string& output);
  static bool is_ffmpeg_available();
};

}  // namespace reed
