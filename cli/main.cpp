#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>

#include "reed/adb.hpp"
#include "reed/config.hpp"
#include "reed/device.hpp"
#include "reed/media.hpp"

namespace fs = std::filesystem;

static std::atomic<bool> g_running{true};

static void signal_handler(int sig) {
  if (sig == SIGTERM || sig == SIGINT) {
    g_running = false;
  }
}

static void print_usage(const char* prog) {
  std::cout
      << "Usage: " << prog
      << " <command> [options]\n\n"
         "Commands:\n"
         "  info                    Show device info\n"
         "  upload <file>           Upload media file (converts GIF to MP4)\n"
         "  display <file...>       Set display to specified media files\n"
         "  brightness <0-100>      Set display brightness\n"
         "  list                    List media files on device\n"
         "  delete <file...>        Delete media files from device\n"
         "  daemon start            Start background daemon\n"
         "  daemon stop             Stop background daemon\n"
         "  daemon status           Show daemon status\n\n"
         "Options:\n"
         "  -p, --port <path>       Serial port (auto-detected if not "
         "specified)\n"
         "  -v, --verbose           Verbose output\n"
         "  --ratio <2:1|1:1>       Display ratio (default: 2:1)\n"
         "  --brightness <0-100>    Set brightness with display command\n"
         "  --keepalive             Stay running with keepalive (default: exit)\n"
         "  --foreground            Run daemon in foreground\n";
}

static int cmd_info(const std::string& port, bool verbose) {
  reed::Device device(port, verbose);

  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }

  auto info = device.handshake();
  if (!info) {
    std::cerr << "Failed to get device info\n";
    return 1;
  }

  std::cout << "Device Information:\n"
            << "  Product: " << info->product_id << "\n"
            << "  OS: " << info->os << "\n"
            << "  Serial: " << info->serial << "\n"
            << "  App Version: " << info->app_version << "\n"
            << "  Firmware: " << info->firmware << "\n"
            << "  Hardware: " << info->hardware << "\n";

  if (!info->attributes.empty()) {
    std::cout << "  Attributes: ";
    for (size_t i = 0; i < info->attributes.size(); ++i) {
      if (i > 0) std::cout << ", ";
      std::cout << info->attributes[i];
    }
    std::cout << "\n";
  }

  return 0;
}

static int cmd_upload(const std::string& file, bool verbose) {
  if (verbose) std::cout << "Checking file: " << file << "\n";

  if (!fs::exists(file)) {
    std::cerr << "File not found: " << file << "\n";
    return 1;
  }

  if (verbose) {
    std::cout << "File size: " << fs::file_size(file) << " bytes\n";
    std::cout << "Checking ADB connection...\n";
  }

  if (!reed::Adb::is_device_connected()) {
    std::cerr << "No ADB device connected\n";
    return 1;
  }

  auto type = reed::Media::detect_type(file);
  std::string upload_path = file;
  std::string remote_name = reed::Media::get_filename(file);

  if (verbose) std::cout << "Detected type: " << static_cast<int>(type) << "\n";

  if (type == reed::MediaType::Gif) {
    if (!reed::Media::is_ffmpeg_available()) {
      std::cerr << "ffmpeg not found. Install ffmpeg to upload GIF files.\n";
      return 1;
    }

    std::string converted_name = reed::Media::get_converted_name(file);
    std::string converted_path =
        std::string(reed::Media::TMP_DIR) + converted_name;

    std::cout << "Converting GIF to MP4...\n";
    if (verbose) std::cout << "Output path: " << converted_path << "\n";

    if (!reed::Media::convert_gif_to_mp4(file, converted_path)) {
      std::cerr << "Failed to convert GIF to MP4\n";
      return 1;
    }

    upload_path = converted_path;
    remote_name = converted_name;
    std::cout << "Converted: " << reed::Media::get_filename(file) << " -> "
              << remote_name << "\n";
  }

  if (verbose)
    std::cout << "Pushing via ADB: " << upload_path << " -> " << remote_name
              << "\n";

  std::cout << "Uploading " << remote_name << "...\n";
  if (!reed::Adb::push(upload_path, remote_name)) {
    std::cerr << "Failed to upload file\n";
    return 1;
  }

  std::cout << "Upload complete.\n";
  std::cout << "Display with: reed-tpse display " << remote_name << "\n";

  return 0;
}

static int cmd_display(const std::string& port,
                       const std::vector<std::string>& files,
                       const std::string& ratio, int brightness, bool keepalive,
                       int keepalive_interval, bool verbose) {
  if (brightness < 0 || brightness > 100) {
    std::cerr << "Brightness must be 0-100\n";
    return 1;
  }

  // Convert GIF filenames to MP4
  std::vector<std::string> media_files;
  for (const auto& f : files) {
    if (reed::Media::detect_type(f) == reed::MediaType::Gif) {
      media_files.push_back(reed::Media::get_converted_name(f));
    } else {
      media_files.push_back(f);
    }
  }

  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }

  device.handshake();

  reed::ScreenConfig config;
  config.media = media_files;
  config.ratio = ratio;

  device.set_screen_config(config);
  device.set_brightness(brightness);

  std::cout << "Display set to: ";
  for (size_t i = 0; i < media_files.size(); ++i) {
    if (i > 0) std::cout << ", ";
    std::cout << media_files[i];
  }
  std::cout << "\n";
  std::cout << "Brightness: " << brightness << "\n";

  // Save state for daemon
  reed::DisplayState state;
  state.media = media_files;
  state.ratio = ratio;
  state.brightness = brightness;
  reed::ConfigManager::save_state(state);

  if (!keepalive) {
    std::cout << "Run 'reed-tpse daemon start' to keep display persistent.\n";
    return 0;
  }

  std::cout << "Keeping connection alive (Ctrl+C to exit)...\n";

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  while (g_running) {
    std::this_thread::sleep_for(std::chrono::seconds(keepalive_interval));
    if (!g_running) break;
    device.handshake();
    if (verbose) {
      std::cout << "  keepalive sent\n";
    }
  }

  std::cout << "Stopping.\n";
  return 0;
}

static int cmd_brightness(const std::string& port, int value, bool verbose) {
  if (value < 0 || value > 100) {
    std::cerr << "Brightness must be 0-100\n";
    return 1;
  }

  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }

  device.handshake();
  device.set_brightness(value);

  std::cout << "Brightness set to " << value << "\n";
  return 0;
}

static int cmd_list() {
  if (!reed::Adb::is_device_connected()) {
    std::cerr << "No ADB device connected\n";
    return 1;
  }

  auto files = reed::Adb::list_media();
  if (!files) {
    std::cerr << "Failed to list media files\n";
    return 1;
  }

  if (files->empty()) {
    std::cout << "No media files on device.\n";
    return 0;
  }

  std::cout << "Media files on device:\n";
  for (const auto& f : *files) {
    std::cout << "  " << f << "\n";
  }

  return 0;
}

static int cmd_delete(const std::vector<std::string>& files) {
  if (!reed::Adb::is_device_connected()) {
    std::cerr << "No ADB device connected\n";
    return 1;
  }

  for (const auto& f : files) {
    if (reed::Adb::remove(f)) {
      std::cout << "Deleted: " << f << "\n";
    } else {
      std::cerr << "Failed to delete: " << f << "\n";
    }
  }

  return 0;
}

static int cmd_daemon_start(const std::string& port, bool foreground,
                            bool verbose) {
  if (!foreground) {
    int ret =
        std::system("systemctl --user enable reed-tpse.service 2>/dev/null");
    ret = std::system("systemctl --user start reed-tpse.service 2>/dev/null");

    if (ret == 0) {
      std::cout << "Daemon started via systemd.\n";
      std::cout << "Check status: reed-tpse daemon status\n";
      return 0;
    } else {
      std::cerr << "systemd service not installed. Run with --foreground or "
                   "install service.\n";
      return 1;
    }
  }

  // Foreground daemon mode
  auto state = reed::ConfigManager::load_state();
  if (!state || state->media.empty()) {
    std::cerr
        << "No display state saved. Run 'reed-tpse display <file>' first.\n";
    return 1;
  }

  auto config = reed::ConfigManager::load_config();
  std::string actual_port =
      (config && !config->port.empty()) ? config->port : port;
  int keepalive_interval = config ? config->keepalive_interval : 10;

  reed::Device device(actual_port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << actual_port << "\n";
    return 1;
  }

  device.handshake();

  reed::ScreenConfig screen_config;
  screen_config.media = state->media;
  screen_config.ratio = state->ratio;
  screen_config.screen_mode = state->screen_mode;
  screen_config.play_mode = state->play_mode;

  device.set_screen_config(screen_config);
  device.set_brightness(state->brightness);

  std::cout << "Display restored. Running keepalive...\n";

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  while (g_running) {
    std::this_thread::sleep_for(std::chrono::seconds(keepalive_interval));
    if (!g_running) break;
    device.handshake();
  }

  return 0;
}

static int cmd_daemon_stop() {
  int ret = std::system("systemctl --user stop reed-tpse.service 2>/dev/null");
  if (ret == 0) {
    std::cout << "Daemon stopped.\n";
    return 0;
  } else {
    std::cerr << "Failed to stop daemon (or not running).\n";
    return 1;
  }
}

static int cmd_daemon_status() {
  int ret =
      std::system("systemctl --user status reed-tpse.service 2>/dev/null");
  return ret == 0 ? 0 : 1;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  std::string port;
  bool verbose = false;
  std::string ratio = "2:1";
  int brightness = 100;
  bool keepalive = false;
  bool foreground = false;
  int keepalive_interval = 10;

  auto config = reed::ConfigManager::load_config();
  if (config) {
    if (!config->port.empty()) {
      port = config->port;
    }
    brightness = config->brightness;
    keepalive_interval = config->keepalive_interval;
  }

  std::string command;
  std::vector<std::string> args;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "-p" || arg == "--port") {
      if (++i < argc) {
        port = argv[i];
      }
    } else if (arg == "-v" || arg == "--verbose") {
      verbose = true;
    } else if (arg == "--ratio") {
      if (++i < argc) ratio = argv[i];
    } else if (arg == "--brightness") {
      if (++i < argc) brightness = std::atoi(argv[i]);
    } else if (arg == "--keepalive") {
      keepalive = true;
    } else if (arg == "--foreground") {
      foreground = true;
    } else if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (command.empty()) {
      command = arg;
    } else {
      args.push_back(arg);
    }
  }

  if (command.empty()) {
    print_usage(argv[0]);
    return 1;
  }

  // Auto-detect port for commands that need serial connection
  bool needs_serial = (command == "info" || command == "display" ||
                       command == "brightness" || command == "daemon");
  if (needs_serial && port.empty()) {
    if (verbose) {
      std::cout << "Auto-detecting device...\n";
    }
    auto detected = reed::Device::find_device(verbose);
    if (!detected) {
      std::cerr
          << "No device found. Specify port with -p or check connection.\n";
      return 1;
    }
    port = *detected;
    if (!verbose) {
      std::cout << "Found device at " << port << "\n";
    }
  }

  if (command == "info") {
    return cmd_info(port, verbose);
  } else if (command == "upload") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse upload <file>\n";
      return 1;
    }
    return cmd_upload(args[0], verbose);
  } else if (command == "display") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse display <file...>\n";
      return 1;
    }
    return cmd_display(port, args, ratio, brightness, keepalive,
                       keepalive_interval, verbose);
  } else if (command == "brightness") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse brightness <0-100>\n";
      return 1;
    }
    return cmd_brightness(port, std::atoi(args[0].c_str()), verbose);
  } else if (command == "list") {
    return cmd_list();
  } else if (command == "delete") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse delete <file...>\n";
      return 1;
    }
    return cmd_delete(args);
  } else if (command == "daemon") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse daemon <start|stop|status>\n";
      return 1;
    }
    if (args[0] == "start") {
      return cmd_daemon_start(port, foreground, verbose);
    } else if (args[0] == "stop") {
      return cmd_daemon_stop();
    } else if (args[0] == "status") {
      return cmd_daemon_status();
    } else {
      std::cerr << "Unknown daemon command: " << args[0] << "\n";
      return 1;
    }
  } else {
    std::cerr << "Unknown command: " << command << "\n";
    print_usage(argv[0]);
    return 1;
  }
}
