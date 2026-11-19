// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ffmpeg_stream.hpp"
#include "videotoolbox_mode.hpp"
#include "vt_decoder.hpp"

#include <memory>
#include <string>

namespace xstudio::media_reader::ffmpeg {

class FFMpegStreamVideoToolbox final : public FFMpegStreamInterface {
  public:
    FFMpegStreamVideoToolbox(
        AVFormatContext *fmt_ctx,
        AVStream *stream,
        int index,
        int thread_count,
        std::string path,
        VideoToolboxMode mode);

    ~FFMpegStreamVideoToolbox() override;

    int64_t current_frame() override;
    ImageBufPtr get_ffmpeg_frame_as_xstudio_image() override;
    [[nodiscard]] const ImageBufPtr &attached_pic() const override { return attached_pic_; }
    std::shared_ptr<thumbnail::ThumbnailBuffer>
    convert_av_frame_to_thumbnail(size_t size_hint) override;
    int64_t receive_frame() override;
    void send_flush_packet() override;
    int send_packet(AVPacket *packet) override;
    void flush_buffers() override;

    [[nodiscard]] bool available() const { return bool(vt_decoder_); }
    [[nodiscard]] bool eligible() const { return eligible_; }
    [[nodiscard]] FFMpegStreamType stream_type() const override { return VIDEO_STREAM; }
    [[nodiscard]] bool is_attached_pic() const override { return false; }
    [[nodiscard]] std::string ffmpeg_decoder_name() const override { return ""; }
    [[nodiscard]] bool converted_on_cpu() const override { return false; }

  private:

    ImageBufPtr decoded_image_;
    SwsContext *sws_context_{nullptr};
    bool nothing_decoded_yet_{true};
    int64_t frame_timestamp_{AV_NOPTS_VALUE};
    ImageBufPtr attached_pic_;
    const VideoToolboxMode mode_;
    bool eligible_ = false;
    std::shared_ptr<VtDecoder> vt_decoder_;
    std::string error_;
};

} // namespace xstudio::media_reader::ffmpeg
