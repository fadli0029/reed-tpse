#pragma once

namespace reed::panel {

/** Native Tryx CM01-class panel (per product specs): 2240×1080 @ 60 Hz. */
inline constexpr int kWidth = 2240;
inline constexpr int kHeight = 1080;
inline constexpr int kFps = 60;

/** Split mode: two tiles side-by-side on the full panel. */
inline constexpr int kSplitHalfWidth = kWidth / 2;  // 1120
inline constexpr int kSplitHalfHeight = kHeight;  // 1080

/**
 * `ratio` string sent in USB JSON (`waterBlockScreenId`). CM01 firmware appears to
 * accept only coarse values like `2:1` / `1:1`; `56:27` can cause the device to
 * reject the config. Frames are still kWidth×kHeight pixels.
 */
inline constexpr const char* kDeviceJsonRatio = "2:1";

/**
 * ffmpeg display aspect to match kDeviceJsonRatio. The panel is slightly wider than
 * 2:1 in pixels; players that honor DAR alongside `ratio:2:1` may letterbox if we
 * tag 56:27. Using 2:1 + kFfmpegSar maps square-ish intent to the wire protocol.
 */
inline constexpr const char* kFfmpegDar = "2/1";

/** SAR so (kWidth x kHeight) samples present as kFfmpegDar: 2 * kHeight / kWidth. */
inline constexpr const char* kFfmpegSar = "27/28";

}  // namespace reed::panel
