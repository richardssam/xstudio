// SPDX-License-Identifier: Apache-2.0
#pragma once

// VideoToolbox decode backend for FFMpegStream (macOS only).
//
// This header deliberately includes no Apple headers: MacTypes.h defines a 'nil'
// macro that breaks caf, and this header is included alongside caf ones.

#include <memory>
#include <string>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "xstudio/media_reader/image_buffer.hpp"

namespace xstudio::media_reader::ffmpeg {

/* Decodes one video stream with a VideoToolbox decompression session.

The send_packet / receive_frame / flush methods follow the same rules as
avcodec_send_packet / avcodec_receive_frame / avcodec_flush_buffers, return codes
included, so FFMpegStream can use it in place of an AVCodecContext.

Frames come out in presentation order in a planar YUV(A) format the xstudio
shader reads directly. Pixel data and all per-frame metadata are returned in
the ImageBufPtr; no AVFrame intermediary is required. */
class VtDecoder {
  public:
    /* Whether the stream's codec and decoder configuration can go to VideoToolbox.
    If not, 'reason' says why. */
    static bool eligible(const AVStream *stream, std::string &reason);

    /* Creates a decoder for the stream. Returns null and sets 'error' if
    VideoToolbox can't create a session for it. Setting the environment variable
    XSTUDIO_VIDEOTOOLBOX_FORCE_FAILURE makes it always fail, for testing the
    fallback paths. */
    static std::unique_ptr<VtDecoder> create(const AVStream *stream, std::string &error);

    ~VtDecoder();

    VtDecoder(const VtDecoder &)            = delete;
    VtDecoder &operator=(const VtDecoder &) = delete;

    /* A null packet starts draining, as with avcodec_send_packet. Returns 0 or a
    negative AVERROR. The packet isn't unreferenced. */
    int send_packet(const AVPacket *packet);

    /* Returns 0 with 'image' filled, AVERROR(EAGAIN) if more packets are needed,
    or AVERROR_EOF once fully drained. */
    int receive_frame(ImageBufPtr &image);

    /* Drops every frame in flight, e.g. before a seek. */
    void flush();

    /* VideoToolbox's name for the session's output format, for logging. */
    [[nodiscard]] std::string output_format_name() const;

    [[nodiscard]] bool using_hardware() const;

  private:
    struct Impl;
    explicit VtDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace xstudio::media_reader::ffmpeg
