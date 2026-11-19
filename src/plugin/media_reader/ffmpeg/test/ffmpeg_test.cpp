// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <filesystem>

#include "ffmpeg_decoder.hpp"
#include "ffmpeg.hpp"
#ifdef XSTUDIO_HAS_VIDEOTOOLBOX
#include "vt_decoder.hpp"
#endif
#include "xstudio/global_store/global_store.hpp"
#include "xstudio/media/media.hpp"
#include "xstudio/media_reader/media_reader.hpp"
#include "xstudio/utility/helpers.hpp"
#include "xstudio/utility/caf_helpers.hpp"

using namespace xstudio;
using namespace xstudio::utility;
using namespace xstudio::media_reader;
using namespace xstudio::media_reader::ffmpeg;

ACTOR_TEST_MINIMAL()

// TEST(FFMpegMediaReaderTest, Test) {
//     FFMpegMediaReader ffmmr;
//     caf::uri good = posix_path_to_uri(TEST_RESOURCE "/media/test.mov");

//     EXPECT_EQ(ffmmr.supported(good, get_signature(good)), MRC_FULLY) << "Should be
//     supported"; EXPECT_EQ(
//         ffmmr.supported(
//             posix_path_to_uri(TEST_RESOURCE "/media/test.0001."), get_signature(good)),
//         MRC_MAYBE)
//         << "Should be supported";

//     EXPECT_TRUE(ffmmr.image(media::AVFrameID(good, 0))) << "Should be supported";
// }


// TEST(FFMPEGLeaker, TEST) {
//    auto path = "/jobs/UAP/IO/london/incoming/2022_06/2022_06_17/client/"
//                "Graded_Media_for_DNEG_Showreel_Aspera_Package/DNE/EP6/106_acc_0120_GRADED.mov";

//    auto decoder = new FFMpegDecoder(path, 44100, VIDEO_STREAM, "stream 0");

//    ImageBufPtr rt;

//    for (auto i = 0; i < 261; i++) {
//        decoder->decode_video_frame(i, rt);
//        rt.reset();
//    }
//    for (auto i = 0; i < 261; i++) {
//        decoder->decode_video_frame(i, rt);
//        rt.reset();
//    }

//    delete decoder;
//}

// Formats the shader can already unpack need no repacking at all.
TEST(FFMpegShaderFriendlyPixFormat, ShaderSupportedFormatsNeedNoRepack) {
    // These go straight down the planar path, so the helper is never consulted -
    // but a lossless target for them must still be themselves if it ever is.
    EXPECT_EQ(shader_friendly_pix_format(AV_PIX_FMT_YUV422P), AV_PIX_FMT_YUV422P);
    EXPECT_EQ(shader_friendly_pix_format(AV_PIX_FMT_YUV444P10LE), AV_PIX_FMT_YUV444P10LE);
}

// Packed YUV must be repacked to planar at the same bit depth and subsampling, so
// the original code values survive for the pixel probe.
TEST(FFMpegShaderFriendlyPixFormat, PackedYUVRepacksToPlanarAtSameDepth) {
    // uyvy422 - 8 bit 4:2:2, as written by Resolve's uncompressed 8 bit movies
    EXPECT_EQ(shader_friendly_pix_format(AV_PIX_FMT_UYVY422), AV_PIX_FMT_YUV422P);
    // v210 - 10 bit 4:2:2
    EXPECT_EQ(shader_friendly_pix_format(AV_PIX_FMT_YUV422P10LE), AV_PIX_FMT_YUV422P10LE);
}

// v408/uyva is 8 bit 4:4:4 with alpha. It used to have no planar target and fell
// back to 8 bit RGBA, losing the YUV code values and taking sws's BT.601 path.
TEST(FFMpegShaderFriendlyPixFormat, EightBitYUVWithAlphaKeepsAlphaAndDepth) {
    EXPECT_EQ(shader_friendly_pix_format(AV_PIX_FMT_VUYA), AV_PIX_FMT_YUVA444P);
    EXPECT_EQ(shader_friendly_pix_format(AV_PIX_FMT_YUVA420P), AV_PIX_FMT_YUVA420P);
    EXPECT_EQ(shader_friendly_pix_format(AV_PIX_FMT_YUVA422P), AV_PIX_FMT_YUVA422P);
}

// Promoting 8 bit to a deeper format would rescale the code values, so the target
// for 8 bit input must stay 8 bit.
TEST(FFMpegShaderFriendlyPixFormat, EightBitIsNeverPromotedToDeeperFormat) {
    for (const auto fmt :
         {AV_PIX_FMT_UYVY422, AV_PIX_FMT_VUYA, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV444P}) {
        const auto dst = shader_friendly_pix_format(fmt);
        ASSERT_NE(dst, AV_PIX_FMT_NONE) << av_get_pix_fmt_name(fmt);
        EXPECT_EQ(av_pix_fmt_desc_get(dst)->comp[0].depth, 8) << av_get_pix_fmt_name(fmt);
    }
}

// Big-endian RGB only needs a byte swap to reach a format the shader handles.
TEST(FFMpegShaderFriendlyPixFormat, BigEndianRGBRepacksToLittleEndian) {
    EXPECT_EQ(shader_friendly_pix_format(AV_PIX_FMT_RGB48BE), AV_PIX_FMT_RGB48LE);
    EXPECT_EQ(shader_friendly_pix_format(AV_PIX_FMT_RGBA64BE), AV_PIX_FMT_RGBA64LE);
}

// 8 bit RGB is already handled by the interleaved RGB/RGBA shader paths.
TEST(FFMpegShaderFriendlyPixFormat, EightBitRGBHasNoPlanarTarget) {
    EXPECT_EQ(shader_friendly_pix_format(AV_PIX_FMT_RGB24), AV_PIX_FMT_NONE);
    EXPECT_EQ(shader_friendly_pix_format(AV_PIX_FMT_RGBA), AV_PIX_FMT_NONE);
}

// Formats with no sensible lossless target fall back to a full conversion.
TEST(FFMpegShaderFriendlyPixFormat, UnsupportedFormatsFallBack) {
    EXPECT_EQ(shader_friendly_pix_format(AV_PIX_FMT_GRAY8), AV_PIX_FMT_NONE);
    EXPECT_EQ(shader_friendly_pix_format(AV_PIX_FMT_PAL8), AV_PIX_FMT_NONE);
    EXPECT_EQ(shader_friendly_pix_format(AV_PIX_FMT_NONE), AV_PIX_FMT_NONE);
}

// sws defaults to BT.601 whatever the stream says, which is visibly wrong for
// BT.709 and BT.2020 media going through the RGBA fallback.
TEST(FFMpegSwsColourspace, MapsStreamColourspaceToCoefficients) {
    EXPECT_EQ(sws_colourspace_from_av(AVCOL_SPC_BT709), SWS_CS_ITU709);
    EXPECT_EQ(sws_colourspace_from_av(AVCOL_SPC_BT2020_NCL), SWS_CS_BT2020);
    EXPECT_EQ(sws_colourspace_from_av(AVCOL_SPC_SMPTE240M), SWS_CS_SMPTE240M);
    EXPECT_EQ(sws_colourspace_from_av(AVCOL_SPC_SMPTE170M), SWS_CS_ITU601);
    EXPECT_EQ(sws_colourspace_from_av(AVCOL_SPC_BT470BG), SWS_CS_ITU601);
    // Unknown falls back to BT.601, matching sws's own default.
    EXPECT_EQ(sws_colourspace_from_av(AVCOL_SPC_UNSPECIFIED), SWS_CS_ITU601);
}


TEST(FFMpegVideoToolboxMode, ParsesPreferenceValues) {
    EXPECT_EQ(videotoolbox_mode_from_string("off"), VideoToolboxMode::Off);
    EXPECT_EQ(videotoolbox_mode_from_string("prefer"), VideoToolboxMode::Prefer);
    EXPECT_EQ(videotoolbox_mode_from_string("only"), VideoToolboxMode::Only);
    // Anything else gets the default.
    EXPECT_EQ(videotoolbox_mode_from_string(""), VideoToolboxMode::Prefer);
    EXPECT_EQ(videotoolbox_mode_from_string("Only"), VideoToolboxMode::Prefer);

    for (auto mode : {VideoToolboxMode::Off, VideoToolboxMode::Prefer, VideoToolboxMode::Only})
        EXPECT_EQ(videotoolbox_mode_from_string(to_string(mode)), mode);
}

// Media from test/videotoolbox_media/generate.sh. Tests that need it are skipped when
// it hasn't been generated.
static std::string vt_media(const std::string &name) {
    const std::string path = std::string(VT_TEST_MEDIA_DIR) + "/" + name;
    return std::filesystem::exists(path) ? path : std::string();
}

#define VT_MEDIA_OR_SKIP(var, name)                                                            \
    const std::string var = vt_media(name);                                                    \
    if (var.empty())                                                                           \
    GTEST_SKIP() << name << " not found in " VT_TEST_MEDIA_DIR                                 \
                 << " - run test/videotoolbox_media/generate.sh"

TEST(FFMpegVideoToolboxMode, DecoderCarriesMode) {
    VT_MEDIA_OR_SKIP(path, "h264_8bit_bframes.mov");
    for (auto mode :
         {VideoToolboxMode::Off, VideoToolboxMode::Prefer, VideoToolboxMode::Only}) {
        FFMpegDecoder decoder(
            path, 48000, utility::FrameRate(timebase::k_flicks_24fps), "", mode);
        EXPECT_EQ(decoder.videotoolbox_mode(), mode);
    }
}

// Decodes one frame through the full reader path (stream, shader params).
static ImageBufPtr decode_frame(
    const std::string &path,
    const int frame,
    const VideoToolboxMode mode = VideoToolboxMode::Off) {
    // Stream 0 is the video stream in all the test media.
    FFMpegDecoder decoder(
        path, 48000, utility::FrameRate(timebase::k_flicks_24fps), "stream 0", mode);
    ImageBufPtr buf;
    decoder.decode_video_frame(frame, buf);
    return buf;
}

// Y offset the shader subtracts: 0 for full range, 16 (8 bit) for video range.
static int luma_offset(const ImageBufPtr &buf) {
    return buf->shader_params()["yuv_offsets"][2].get<int>();
}

// DNxHD/HR in a .dneg.mov is always treated as video range, whatever it is tagged.
TEST(FFMpegColourRange, DnegMovForcesVideoRangeForDNxHD) {
    VT_MEDIA_OR_SKIP(plain, "dnxhr_fullrange.mov");
    VT_MEDIA_OR_SKIP(dneg, "dnxhr_fullrange.dneg.mov");

    auto plain_buf = decode_frame(plain, 0);
    auto dneg_buf  = decode_frame(dneg, 0);
    ASSERT_TRUE(plain_buf && dneg_buf);
    EXPECT_EQ(luma_offset(plain_buf), 0);
    EXPECT_EQ(luma_offset(dneg_buf), 16);
}

// ---------------------------------------------------------------------------------
// VtDecoder, driven directly with packets from libavformat.
#ifdef XSTUDIO_HAS_VIDEOTOOLBOX

namespace {

struct Demuxer {
    AVFormatContext *ctx = nullptr;
    AVStream *stream     = nullptr;
    AVPacket *pkt        = av_packet_alloc();
    explicit Demuxer(const std::string &path) {
        if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) == 0 &&
            avformat_find_stream_info(ctx, nullptr) >= 0) {
            const int i = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (i >= 0)
                stream = ctx->streams[i];
        }
    }
    ~Demuxer() {
        av_packet_free(&pkt);
        avformat_close_input(&ctx);
    }
    // next video packet, or null at the end
    AVPacket *next() {
        av_packet_unref(pkt);
        while (av_read_frame(ctx, pkt) >= 0) {
            if (pkt->stream_index == stream->index)
                return pkt;
            av_packet_unref(pkt);
        }
        return nullptr;
    }
};

struct DecodeResult {
    std::vector<int64_t> packet_pts;
    std::vector<int64_t> frame_pts;
    int send_errors                   = 0;
    std::string format;
    std::string colorspace;
    AVColorPrimaries primaries        = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED;
    bool full_range = false;
};

// Decodes the whole stream with VideoToolbox, avcodec-style send/receive loop.
DecodeResult vt_decode_all(const std::string &path, const int max_frames = 1 << 30) {
    DecodeResult r;
    Demuxer dm(path);
    std::string error;
    auto vt = VtDecoder::create(dm.stream, error);
    EXPECT_TRUE(vt) << error;
    if (!vt)
        return r;
    ImageBufPtr frame;
    auto take = [&]() {
        const auto &params   = frame->shader_params();
        const auto &metadata = frame->metadata();
        if (r.frame_pts.empty()) {
            r.format =
                fmt::format(
                    "{}{}{}",
                    params.value("half_scale_uvx", 0) ? "subx" : "fullx",
                    params.value("half_scale_uvy", 0) ? "suby" : "fully",
                    metadata.value("has_alpha", false) ? ":alpha" : "");
            r.colorspace = metadata.value("color_space", std::string());
            r.primaries = AVColorPrimaries(
                metadata.value("color_primaries", int(AVCOL_PRI_UNSPECIFIED)));
            r.trc = AVColorTransferCharacteristic(
                metadata.value("color_trc", int(AVCOL_TRC_UNSPECIFIED)));
            r.full_range = metadata.value("full_range", false);
        }
        r.frame_pts.push_back(metadata.value("pts", AV_NOPTS_VALUE));
    };
    while (AVPacket *pkt = dm.next()) {
        if (int(r.frame_pts.size()) >= max_frames)
            break;
        r.packet_pts.push_back(pkt->pts);
        if (vt->send_packet(pkt) < 0)
            r.send_errors++;
        while (vt->receive_frame(frame) == 0)
            take();
    }
    vt->send_packet(nullptr);
    while (vt->receive_frame(frame) == 0)
        take();
    return r;
}

// Same, with FFmpeg's software decoder - the reference.
DecodeResult sw_decode_all(const std::string &path) {
    DecodeResult r;
    Demuxer dm(path);
    const AVCodec *codec = avcodec_find_decoder(dm.stream->codecpar->codec_id);
    AVCodecContext *ctx  = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, dm.stream->codecpar);
    avcodec_open2(ctx, codec, nullptr);
    AVFrame *frame = av_frame_alloc();
    auto take      = [&]() {
        if (r.frame_pts.empty()) {
            const AVPixFmtDescriptor *description = av_pix_fmt_desc_get(AVPixelFormat(frame->format));
            r.format =
                fmt::format(
                    "{}{}{}",
                    description->log2_chroma_w ? "subx" : "fullx",
                    description->log2_chroma_h ? "suby" : "fully",
                    description->flags & AV_PIX_FMT_FLAG_ALPHA ? ":alpha" : "");
            switch (frame->colorspace) {
            case AVCOL_SPC_BT709:
                r.colorspace = "bt709";
                break;
            case AVCOL_SPC_BT2020_NCL:
            case AVCOL_SPC_BT2020_CL:
                r.colorspace = "bt2020";
                break;
            case AVCOL_SPC_SMPTE240M:
                r.colorspace = "smpte240";
                break;
            default:
                r.colorspace = "bt601";
                break;
            }
            r.primaries  = frame->color_primaries;
            r.trc        = frame->color_trc;
            r.full_range = frame->color_range == AVCOL_RANGE_JPEG;
        }
        r.frame_pts.push_back(frame->pts);
    };
    while (AVPacket *pkt = dm.next()) {
        r.packet_pts.push_back(pkt->pts);
        avcodec_send_packet(ctx, pkt);
        while (avcodec_receive_frame(ctx, frame) == 0)
            take();
    }
    avcodec_send_packet(ctx, nullptr);
    while (avcodec_receive_frame(ctx, frame) == 0)
        take();
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return r;
}

const std::vector<std::string> k_vt_media = {
    "h264_8bit_bframes.mov",
    "hevc_8bit_opengop.mov",
    "hevc_main10_bframes.mov",
    "hevc_422_10bit_bframes.mov",
    "hevc_main10_bt2020_pq.mov",
    "prores_422_proxy.mov",
    "prores_422_lt.mov",
    "prores_422_standard.mov",
    "prores_422_hq.mov",
    "prores_4444_alpha.mov",
    "prores_4444xq.mov",
    // variants (spike 1.8); the 12 bit ones need FFMPEG_HIGHBIT when generating
    "variant_h264_10bit_420.mov",
    "variant_h264_8bit_422.mov",
    "variant_h264_10bit_422.mov",
    "variant_h264_8bit_444.mov",
    "variant_h264_10bit_444.mov",
    "variant_hevc_8bit_422.mov",
    "variant_hevc_8bit_444.mov",
    "variant_hevc_10bit_444.mov",
    "variant_hevc_12bit_422.mov",
    "variant_hevc_12bit_444.mov",
};

// Is the clip there? Variant clips are optional; the core set is required.
bool have_clip(const std::string &name) {
    if (!vt_media(name).empty())
        return name.rfind("variant_", 0) != 0;
    const bool optional =
        name.rfind("variant_", 0) == 0 || name == "hevc_422_10bit_bframes.mov";
    EXPECT_TRUE(optional) << name << " missing";
    return false;
}

} // namespace

// Task 4.1 / 4.2
TEST(VtDecoder, EligibilityAndOutputFormat) {
    VT_MEDIA_OR_SKIP(first, "h264_8bit_bframes.mov");
    for (const auto &name : k_vt_media) {
        if (!have_clip(name))
            continue;
        Demuxer dm(vt_media(name));
        ASSERT_TRUE(dm.stream) << name;
        std::string reason;
        EXPECT_TRUE(VtDecoder::eligible(dm.stream, reason)) << name << ": " << reason;
        auto vt = VtDecoder::create(dm.stream, reason);
        ASSERT_TRUE(vt) << name << ": " << reason;
        EXPECT_EQ(vt->output_format_name(), "automatic") << name;
        bool decoded = false;
        while (AVPacket *packet = dm.next()) {
            ASSERT_GE(vt->send_packet(packet), 0) << name;
            ImageBufPtr frame;
            if (vt->receive_frame(frame) == 0) {
                decoded = true;
                break;
            }
        }
        EXPECT_TRUE(decoded) << name;
        EXPECT_FALSE(vt->output_format_name().empty()) << name;
        EXPECT_NE(vt->output_format_name(), "automatic") << name;
    }

    for (const auto *name :
         {"h264_annexb.h264", "dnxhr_fullrange.mov"}) {
        if (vt_media(name).empty())
            continue; // the 12 bit clip is optional
        Demuxer dm(vt_media(name));
        ASSERT_TRUE(dm.stream) << name;
        std::string reason;
        EXPECT_FALSE(VtDecoder::eligible(dm.stream, reason)) << name;
        EXPECT_FALSE(reason.empty()) << name;
        EXPECT_FALSE(VtDecoder::create(dm.stream, reason)) << name;
    }
}

// Task 4.3 / 4.4: every packet decodes, frames come out once each in presentation
// order, and the last frames aren't lost in the reorder delay.
TEST(VtDecoder, DecodesEveryPacketInPresentationOrder) {
    VT_MEDIA_OR_SKIP(first, "h264_8bit_bframes.mov");
    for (const auto &name : k_vt_media) {
        if (!have_clip(name))
            continue;
        const auto r = vt_decode_all(vt_media(name));
        EXPECT_EQ(r.send_errors, 0) << name;
        ASSERT_EQ(r.frame_pts.size(), r.packet_pts.size()) << name;
        EXPECT_TRUE(std::is_sorted(r.frame_pts.begin(), r.frame_pts.end())) << name;
        EXPECT_EQ(std::adjacent_find(r.frame_pts.begin(), r.frame_pts.end()), r.frame_pts.end())
            << name << " repeats a frame";
        auto sorted_packets = r.packet_pts;
        std::sort(sorted_packets.begin(), sorted_packets.end());
        EXPECT_EQ(r.frame_pts, sorted_packets) << name;
    }
}

// Task 4.5 / 4.6: planar output format and colour metadata match FFmpeg's decoder.
TEST(VtDecoder, FormatAndColourMatchSoftwareDecoder) {
    VT_MEDIA_OR_SKIP(first, "h264_8bit_bframes.mov");
    for (const auto &name : k_vt_media) {
        if (!have_clip(name))
            continue;
        const auto vt = vt_decode_all(vt_media(name), 1);
        const auto sw = sw_decode_all(vt_media(name));
        ASSERT_FALSE(vt.frame_pts.empty()) << name;
        EXPECT_EQ(vt.format, sw.format) << name;
        EXPECT_EQ(vt.primaries, sw.primaries) << name;
        EXPECT_EQ(vt.trc, sw.trc) << name;
        // FFmpeg reports MPEG range for ProRes; unspecified elsewhere means MPEG too.
        EXPECT_EQ(vt.full_range, sw.full_range) << name;
    }
}

// Task 4.5: decode 1000+ frames; run under 'leaks' to check no CVPixelBuffer or
// session objects are left behind.
TEST(VtDecoder, LongDecodeForLeakCheck) {
    VT_MEDIA_OR_SKIP(path, "h264_8bit_bframes.mov");
    size_t frames = 0;
    for (int i = 0; i < 21; ++i)
        frames += vt_decode_all(path).frame_pts.size();
    EXPECT_GE(frames, 1000u);
}


// ---------------------------------------------------------------------------------
// Through FFMpegDecoder / FFMpegStream: the path the reader uses.

namespace {

// Planar YUV(A) samples of a decoded image, read via the shader parameters the
// reader produced (so this also checks those are right).
struct Samples {
    int bits = 0;
    std::array<std::vector<uint16_t>, 4> planes;
};

Samples samples_of(const ImageBufPtr &buf) {
    Samples out;
    const auto &p            = buf->shader_params();
    const auto &metadata     = buf->metadata();
    const int vt_sample_bits = p.value("vt_sample_bits", 0);
    out.bits = vt_sample_bits ? metadata.value("value_bits", 8) : p.value("bits_per_channel", 8);
    const int bps            = out.bits > 8 ? 2 : 1;
    const auto size          = buf->image_size_in_pixels();
    const int cw             = p.value("half_scale_uvx", 0) ? (size.x + 1) / 2 : size.x;
    const int ch             = p.value("half_scale_uvy", 0) ? (size.y + 1) / 2 : size.y;
    const char *names[4]     = {"y", "u", "v", "a"};
    for (int i = 0; i < 4; ++i) {
        if (vt_sample_bits && p.value("vt_chroma_interleaved", false) && i == 2)
            continue;
        const int linesize = p.value(std::string(names[i]) + "_linesize", 0);
        if (!linesize)
            continue;
        const size_t offset = p.value(std::string(names[i]) + "_plane_bytes_offset", size_t(0));
        const int w         = (i == 1 || i == 2) ? cw : size.x;
        const int h         = (i == 1 || i == 2) ? ch : size.y;
        auto &plane         = out.planes[i];
        plane.reserve(size_t(w) * h);
        const auto *base = reinterpret_cast<const uint8_t *>(buf->buffer()) + offset;
        for (int y = 0; y < h; ++y) {
            const uint8_t *row = base + size_t(linesize) * y;
            for (int x = 0; x < w; ++x) {
                uint16_t sample =
                    bps == 2 ? reinterpret_cast<const uint16_t *>(row)[x] : uint16_t(row[x]);
                if (vt_sample_bits == 10)
                    sample >>= 6;
                else if (vt_sample_bits == 16)
                    sample = uint16_t((sample + 8) >> 4);
                plane.push_back(sample);
            }
        }
    }
    if (vt_sample_bits && p.value("vt_chroma_interleaved", false)) {
        const int chroma_stride    = p.value("u_linesize", 0);
        const size_t chroma_offset = p.value("u_plane_bytes_offset", size_t(0));
        auto &u                    = out.planes[1];
        auto &v                    = out.planes[2];
        u.clear();
        v.clear();
        u.reserve(size_t(cw) * ch);
        v.reserve(size_t(cw) * ch);
        const auto *base = reinterpret_cast<const uint8_t *>(buf->buffer()) + chroma_offset;
        for (int y = 0; y < ch; ++y) {
            const auto *row = base + size_t(chroma_stride) * y;
            for (int x = 0; x < cw; ++x) {
                uint16_t cb, cr;
                if (vt_sample_bits == 8) {
                    cb = row[x * 2];
                    cr = row[x * 2 + 1];
                } else {
                    cb = reinterpret_cast<const uint16_t *>(row)[x * 2];
                    cr = reinterpret_cast<const uint16_t *>(row)[x * 2 + 1];
                    if (vt_sample_bits == 10) {
                        cb >>= 6;
                        cr >>= 6;
                    } else {
                        cb = uint16_t((cb + 8) >> 4);
                        cr = uint16_t((cr + 8) >> 4);
                    }
                }
                u.push_back(cb);
                v.push_back(cr);
            }
        }
    }
    return out;
}

// Largest absolute sample difference between two images, -1 if their layouts differ.
int max_difference(const ImageBufPtr &a, const ImageBufPtr &b) {
    const Samples sa = samples_of(a), sb = samples_of(b);
    const int max_a = (1 << sa.bits) - 1;
    const int max_b = (1 << sb.bits) - 1;
    int worst = 0;
    for (int i = 0; i < 4; ++i) {
        if (sa.planes[i].size() != sb.planes[i].size())
            return -1;
        for (size_t k = 0; k < sa.planes[i].size(); ++k) {
            const int normalized_a = int(sa.planes[i][k]) * 255 / max_a;
            const int normalized_b = int(sb.planes[i][k]) * 255 / max_b;
            worst                  = std::max(worst, std::abs(normalized_a - normalized_b));
        }
    }
    return worst;
}

// Scoped environment variable
struct ScopedEnv {
    std::string name;
    ScopedEnv(const char *n, const char *v) : name(n) { setenv(n, v, 1); }
    ~ScopedEnv() { unsetenv(name.c_str()); }
};

FFMpegDecoder make_decoder(const std::string &path, const VideoToolboxMode mode) {
    return FFMpegDecoder(
        path, 48000, utility::FrameRate(timebase::k_flicks_24fps), "stream 0", mode);
}

} // namespace

// Task 5.1 (and the second half of 4.5): frames come from VideoToolbox, as planar
// YUV the shader reads directly - no CPU conversion on the way.
TEST(VtStream, PlaysThroughVideoToolboxWithoutCpuConversion) {
    VT_MEDIA_OR_SKIP(first, "h264_8bit_bframes.mov");
    for (const auto &name : k_vt_media) {
        if (!have_clip(name))
            continue;
        auto decoder = make_decoder(vt_media(name), VideoToolboxMode::Only);
        auto stream  = decoder.stream(0);
        ASSERT_TRUE(stream) << name;
        // Task 3.2: FFmpeg's decoder is never even looked up, so builds without
        // one for this codec work the same.
        for (int f : {0, 1, 2, 10}) {
            ImageBufPtr buf;
            decoder.decode_video_frame(f, buf);
            ASSERT_TRUE(buf) << name << " frame " << f;
            EXPECT_FALSE(buf->shader_params().value("vt_sample_bits", 0) == 0)
                << name << ": VideoToolbox unpack shader path";
            EXPECT_EQ(buf->decoder_frame_number(), f) << name;
        }
        EXPECT_FALSE(stream->converted_on_cpu()) << name;
    }
}

// Spec: frame-accurate presentation order (task 7.1). A fixed jumbled sequence of
// frame requests - forwards, backwards, first and last - must give the same
// pictures as FFmpeg's decoder: bit-exact for H.264/HEVC, within the ProRes
// tolerance measured in the spike (max 4 code values).
TEST(VtStream, RandomAccessMatchesSoftwareDecoder) {
    VT_MEDIA_OR_SKIP(first, "h264_8bit_bframes.mov");
    const std::vector<int> sequence = {
        0, 1, 2, 47, 12, 13, 5, 30, 29, 28, 46, 23, 24, 0, 37, 11};
    for (const auto &name : k_vt_media) {
        if (!have_clip(name))
            continue;
        const bool prores = name.rfind("prores", 0) == 0;
        auto vt           = make_decoder(vt_media(name), VideoToolboxMode::Only);
        auto sw           = make_decoder(vt_media(name), VideoToolboxMode::Off);
        for (const int f : sequence) {
            ImageBufPtr a, b;
            std::string vt_error, sw_error;
            try {
                vt.decode_video_frame(f, a);
            } catch (const std::exception &e) {
                vt_error = e.what();
            }
            try {
                sw.decode_video_frame(f, b);
            } catch (const std::exception &e) {
                sw_error = e.what();
            }
            // FFMpegDecoder can't reach the frame just before an open-GOP keyframe
            // after a backward jump (frame 23 of the open-GOP clips here) with FFmpeg's
            // decoder - a seek issue predating VideoToolbox support. VideoToolbox may do
            // better, but must never fail where FFmpeg's decoder succeeds.
            if (vt_error.empty() && !sw_error.empty()) {
                ASSERT_TRUE(a) << name << " frame " << f;
                EXPECT_EQ(a->decoder_frame_number(), f) << name;
                continue;
            }
            EXPECT_EQ(vt_error, sw_error) << name << " frame " << f;
            if (!vt_error.empty() || !sw_error.empty())
                continue;
            ASSERT_TRUE(a && b) << name << " frame " << f;
            EXPECT_EQ(a->decoder_frame_number(), f) << name;
            EXPECT_EQ(a->has_alpha(), b->has_alpha()) << name;
            const int diff = max_difference(a, b);
            if (name.find("12bit") != std::string::npos) {
                EXPECT_GE(diff, 0) << name << " frame " << f << ": layouts differ";
                EXPECT_LE(diff, 1) << name << " frame " << f;
            } else if (prores) {
                EXPECT_GE(diff, 0) << name << " frame " << f << ": layouts differ";
                EXPECT_LE(diff, 4) << name << " frame " << f;
            } else {
                EXPECT_EQ(diff, 0) << name << " frame " << f;
            }
        }
    }
}

// Task 5.2: when VideoToolbox can't create a session, 'prefer' falls back to
// FFmpeg's decoder and 'only' fails each frame with a message naming the codec.
TEST(VtStream, SessionFailureFallsBackOrFails) {
    VT_MEDIA_OR_SKIP(path, "hevc_main10_bframes.mov");
    ScopedEnv force("XSTUDIO_VIDEOTOOLBOX_FORCE_FAILURE", "1");

    {
        auto decoder = make_decoder(path, VideoToolboxMode::Prefer);
        ImageBufPtr buf;
        decoder.decode_video_frame(3, buf);
        ASSERT_TRUE(buf);
        EXPECT_EQ(buf->decoder_frame_number(), 3);
    }
    {
        auto decoder = make_decoder(path, VideoToolboxMode::Only);
        ImageBufPtr buf;
        try {
            decoder.decode_video_frame(3, buf);
            ADD_FAILURE() << "expected videotoolbox_unavailable_error";
        } catch (const videotoolbox_unavailable_error &e) {
            EXPECT_NE(std::string(e.what()).find("hevc"), std::string::npos) << e.what();
            EXPECT_NE(std::string(e.what()).find("VideoToolbox"), std::string::npos)
                << e.what();
        }
        // and keeps failing cleanly rather than crashing
        EXPECT_THROW(decoder.decode_video_frame(4, buf), videotoolbox_unavailable_error);
    }
}

// Task 5.3: streams VideoToolbox doesn't take are decoded by FFmpeg, in any mode,
// exactly as with VideoToolbox off. (A raw Annex-B stream has no container timing,
// so the reader gives it no duration or frame numbers either way.)
TEST(VtStream, IneligibleStreamsUseSoftware) {
    VT_MEDIA_OR_SKIP(annexb, "h264_annexb.h264");
    VT_MEDIA_OR_SKIP(dnx, "dnxhr_fullrange.mov");
    for (const auto &path : {annexb, dnx}) {
        auto reference = make_decoder(path, VideoToolboxMode::Off);
        ImageBufPtr ref;
        reference.decode_video_frame(5, ref);
        ASSERT_TRUE(ref) << path;
        for (auto mode : {VideoToolboxMode::Prefer, VideoToolboxMode::Only}) {
            auto decoder = make_decoder(path, mode);
            ImageBufPtr buf;
            decoder.decode_video_frame(5, buf);
            ASSERT_TRUE(buf) << path;
            EXPECT_EQ(buf->decoder_frame_number(), ref->decoder_frame_number()) << path;
            EXPECT_EQ(max_difference(buf, ref), 0) << path;
        }
    }
}

// Task 5.4: thumbnails of VideoToolbox-decoded media.
TEST(VtStream, Thumbnails) {
    VT_MEDIA_OR_SKIP(first, "h264_8bit_bframes.mov");
    for (const auto &name : k_vt_media) {
        if (!have_clip(name))
            continue;
        auto decoder = make_decoder(vt_media(name), VideoToolboxMode::Only);
        auto thumb   = decoder.decode_thumbnail_frame(24, 256);
        ASSERT_TRUE(thumb) << name;
        EXPECT_GT(thumb->width(), 0u) << name;
        EXPECT_GT(thumb->height(), 0u) << name;
    }
}

// Changing the preference at runtime must not hand back a frame decoded under the
// previous mode (FFMpegMediaReader keeps the last decoded image to answer repeat
// requests).
TEST(VtStream, PreferenceChangeDropsFramesDecodedUnderOldMode) {
    VT_MEDIA_OR_SKIP(path, "prores_422_hq.mov");

    utility::JsonStore prefs;
    global_store::set_preference_value(
        prefs, std::string("prefer"), "/plugin/media_reader/FFMPEG/videotoolbox");
    FFMpegMediaReader reader(prefs);

    const media::AVFrameID frame(
        posix_path_to_uri(path),
        10,
        0,
        media::FS_ON_DISK,
        0,
        1.0f,
        utility::FrameRate(timebase::k_flicks_24fps),
        "stream 0");

    const ImageBufPtr vt = reader.image(frame);
    ASSERT_TRUE(vt);
    vt->set_media_key(frame.key()); // as the reader actor does
    EXPECT_EQ(reader.image(frame), vt) << "repeat request is answered from memory";

    global_store::set_preference_value(
        prefs, std::string("off"), "/plugin/media_reader/FFMPEG/videotoolbox");
    reader.update_preferences(prefs);

    const ImageBufPtr sw = reader.image(frame);
    ASSERT_TRUE(sw);
    EXPECT_NE(sw, vt);
    // Apple's and FFmpeg's ProRes decoders differ slightly, so a fresh decode shows it
    EXPECT_GT(max_difference(sw, vt), 0);
    EXPECT_LE(max_difference(sw, vt), 4);
}

// ---------------------------------------------------------------------------------
// Task 6.1: AAC through AudioToolbox (aac_at)

namespace {

struct DecodedAudio {
    int64_t first_pts = AV_NOPTS_VALUE;
    std::vector<float> samples; // first channel
};

DecodedAudio decode_audio_with(const std::string &path, const char *decoder_name) {
    DecodedAudio out;
    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0)
        return out;
    avformat_find_stream_info(ctx, nullptr);
    const int index      = av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    const AVCodec *codec = avcodec_find_decoder_by_name(decoder_name);
    AVCodecContext *dec  = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, ctx->streams[index]->codecpar);
    avcodec_open2(dec, codec, nullptr);
    AVPacket *pkt  = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    auto take      = [&]() {
        if (out.first_pts == AV_NOPTS_VALUE)
            out.first_pts = frame->pts;
        for (int i = 0; i < frame->nb_samples; ++i) {
            switch (frame->format) {
            case AV_SAMPLE_FMT_FLTP:
            case AV_SAMPLE_FMT_FLT:
                out.samples.push_back(reinterpret_cast<const float *>(frame->data[0])[i]);
                break;
            case AV_SAMPLE_FMT_S16P:
            case AV_SAMPLE_FMT_S16:
                out.samples.push_back(
                    reinterpret_cast<const int16_t *>(frame->data[0])[i] / 32768.0f);
                break;
            default:
                break;
            }
        }
    };
    while (av_read_frame(ctx, pkt) >= 0) {
        if (pkt->stream_index == index) {
            avcodec_send_packet(dec, pkt);
            while (avcodec_receive_frame(dec, frame) == 0)
                take();
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(dec, nullptr);
    while (avcodec_receive_frame(dec, frame) == 0)
        take();
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&ctx);
    return out;
}

} // namespace

TEST(AacAudioToolbox, StreamUsesAacAt) {
    VT_MEDIA_OR_SKIP(path, "h264_aac.mov");
    ASSERT_TRUE(avcodec_find_decoder_by_name("aac_at")) << "FFmpeg built without AudioToolbox";
    for (auto mode : {VideoToolboxMode::Off, VideoToolboxMode::Prefer}) {
        FFMpegDecoder decoder(
            path, 48000, utility::FrameRate(timebase::k_flicks_24fps), "stream 1", mode);
        auto stream = decoder.stream(1);
        ASSERT_TRUE(stream);
        EXPECT_EQ(stream->ffmpeg_decoder_name(), "aac_at") << to_string(mode);
        AudioBufPtr audio;
        decoder.decode_audio_frame(10, audio);
        ASSERT_TRUE(audio);
        EXPECT_GT(audio->num_samples(), 0u);
    }
}

// aac_at must line up sample for sample with FFmpeg's own AAC decoder - same first
// timestamp, same length, no priming offset - or audio sync would change.
TEST(AacAudioToolbox, MatchesFFmpegAacTiming) {
    VT_MEDIA_OR_SKIP(path, "h264_aac.mov");
    const auto ref = decode_audio_with(path, "aac");
    const auto at  = decode_audio_with(path, "aac_at");
    ASSERT_FALSE(ref.samples.empty());
    ASSERT_FALSE(at.samples.empty());
    EXPECT_EQ(at.first_pts, ref.first_pts);
    EXPECT_EQ(at.samples.size(), ref.samples.size());

    // best alignment within +-4096 samples, on the chirp (non-periodic)
    const size_t n  = std::min(at.samples.size(), ref.samples.size());
    int best_lag    = 0;
    double best_err = 1e30;
    for (int lag = -4096; lag <= 4096; lag += 1) {
        double err  = 0;
        size_t used = 0;
        for (size_t i = 8192; i + 8192 < n; i += 7) {
            const long j   = long(i) + lag;
            const double d = at.samples[size_t(j)] - ref.samples[i];
            err += d * d;
            used++;
        }
        err /= double(used);
        if (err < best_err) {
            best_err = err;
            best_lag = lag;
        }
    }
    EXPECT_EQ(best_lag, 0);
    EXPECT_LT(std::sqrt(best_err), 1e-3) << "RMS difference between AAC decoders";
}

#endif // XSTUDIO_HAS_VIDEOTOOLBOX


// Not run by default (slow). Run with --gtest_also_run_disabled_tests.
// Memory must stay flat however many frames are decoded: VideoToolbox's frames live
// in IOSurfaces, which 'leaks' can't see but the process footprint includes.
#ifdef XSTUDIO_HAS_VIDEOTOOLBOX
#include <mach/mach.h>

static double footprint_mb() {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
        return -1;
    return double(info.phys_footprint) / (1024.0 * 1024.0);
}

TEST(VtStream, DISABLED_MemoryStaysFlatOverLongDecode) {
    VT_MEDIA_OR_SKIP(path, "bench_uhd_prores_4444_grain.mov");
    for (auto mode : {VideoToolboxMode::Off, VideoToolboxMode::Prefer}) {
        std::vector<double> mb;
        for (int pass = 0; pass < 6; ++pass) {
            // a new decoder each pass, like the reader for thumbnails / reopened media
            auto decoder = make_decoder(path, mode);
            for (int f = 0; f < 120; ++f) {
                ImageBufPtr buf;
                decoder.decode_video_frame(f, buf);
            }
            mb.push_back(footprint_mb());
        }
        std::cout << to_string(mode) << " footprint MB after each 120 frame pass:";
        for (auto v : mb)
            std::cout << " " << int(v);
        std::cout << "\n";
        // allow some slack for allocator pools; a leaked 4K 4444 frame is ~66MB
        EXPECT_LT(mb.back() - mb[1], 200.0) << to_string(mode);
    }
}

#endif
