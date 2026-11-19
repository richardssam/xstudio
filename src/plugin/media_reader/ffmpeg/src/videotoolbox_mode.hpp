// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>
#include <string>

namespace xstudio::media_reader::ffmpeg {

enum class VideoToolboxMode { Off, Prefer, Only };

inline VideoToolboxMode videotoolbox_mode_from_string(const std::string &value) {
    if (value == "off")
        return VideoToolboxMode::Off;
    if (value == "only")
        return VideoToolboxMode::Only;
    return VideoToolboxMode::Prefer;
}

inline const char *to_string(const VideoToolboxMode mode) {
    switch (mode) {
    case VideoToolboxMode::Off:
        return "off";
    case VideoToolboxMode::Only:
        return "only";
    default:
        return "prefer";
    }
}

class videotoolbox_unavailable_error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class videotoolbox_fallback_error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

} // namespace xstudio::media_reader::ffmpeg
