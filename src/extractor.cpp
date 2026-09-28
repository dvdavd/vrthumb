#include "extractor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <numbers>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <turbojpeg.h>
#include <webp/encode.h>
}

namespace vrthumb {
namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string av_error(int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  av_strerror(code, buffer.data(), buffer.size());
  return buffer.data();
}

[[noreturn]] void fail_av(ErrorCode error_code, const std::string& what, int av_code) {
  throw Error(error_code, what + ": " + av_error(av_code));
}

[[noreturn]] void fail(ErrorCode code, const std::string& message) {
  throw Error(code, message);
}

struct FormatDeleter {
  void operator()(AVFormatContext* p) const { avformat_close_input(&p); }
};
struct CodecDeleter {
  void operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
};
struct FrameDeleter {
  void operator()(AVFrame* p) const { av_frame_free(&p); }
};
struct PacketDeleter {
  void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct SwsDeleter {
  void operator()(SwsContext* p) const { sws_freeContext(p); }
};
struct BufferDeleter {
  void operator()(AVBufferRef* p) const { av_buffer_unref(&p); }
};
struct TjDeleter {
  void operator()(tjhandle p) const { if (p) tjDestroy(p); }
};
struct FilterGraphDeleter {
  void operator()(AVFilterGraph* p) const { avfilter_graph_free(&p); }
};
struct FilterInOutDeleter {
  void operator()(AVFilterInOut* p) const { avfilter_inout_free(&p); }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatDeleter>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using BufferPtr = std::unique_ptr<AVBufferRef, BufferDeleter>;
using TjPtr = std::unique_ptr<void, TjDeleter>;
using FilterGraphPtr = std::unique_ptr<AVFilterGraph, FilterGraphDeleter>;
using FilterInOutPtr = std::unique_ptr<AVFilterInOut, FilterInOutDeleter>;

struct EncodedThumbnail {
  Thumbnail metadata;
  std::vector<unsigned char> bytes;
};

struct Session {
  FormatPtr format;
  AVStream* stream = nullptr;
  int stream_index = -1;
  // The decoder av_find_best_stream() prefers for the stream.
  const AVCodec* best_decoder = nullptr;
  CodecPtr codec;
  DecodeBackend backend = DecodeBackend::software;
  BufferPtr hw_device;
};

void validate_view(const View& view, const std::string& prefix) {
  if (view.yaw < -180 || view.yaw > 180)
    fail(ErrorCode::invalid_option, prefix + "yaw must be between -180 and 180");
  if (view.pitch < -90 || view.pitch > 90)
    fail(ErrorCode::invalid_option, prefix + "pitch must be between -90 and 90");
  if (view.horizontal_fov < 30 || view.horizontal_fov > 170)
    fail(ErrorCode::invalid_option,
         prefix + "horizontal field of view must be between 30 and 170");
}

void validate(const ExtractOptions& o) {
  if (o.count < 1 || o.count > 1000)
    fail(ErrorCode::invalid_option, "count must be between 1 and 1000");
  if (o.max_width < 2 || o.max_height < 2 || o.max_width > 8192 || o.max_height > 8192)
    fail(ErrorCode::invalid_option, "thumbnail dimensions must be between 2 and 8192");
  if (o.quality < 1 || o.quality > 100)
    fail(ErrorCode::invalid_option, "quality must be between 1 and 100");
  if (o.vr_eye_size < 64 || o.vr_eye_size > 4096)
    fail(ErrorCode::invalid_option, "VR eye size must be between 64 and 4096");
  validate_view(o.view, "");
  if (o.input_horizontal_fov < 30 || o.input_horizontal_fov > 360)
    fail(ErrorCode::invalid_option, "input horizontal field of view must be between 30 and 360");
  for (double percentage : o.seek_percentages)
    if (percentage < 0 || percentage > 100)
      fail(ErrorCode::invalid_option, "seek percentage must be between 0 and 100");
  if (!o.seek_percentages.empty() && !o.seek_seconds.empty())
    fail(ErrorCode::invalid_option,
         "seek_percentages and seek_seconds are mutually exclusive");
  for (double seconds : o.seek_seconds)
    if (seconds < 0)
      fail(ErrorCode::invalid_option, "seek seconds must not be negative");
  if (!o.requests.empty() && (!o.seek_percentages.empty() || !o.seek_seconds.empty()))
    fail(ErrorCode::invalid_option,
         "requests cannot be combined with seek_percentages or seek_seconds");
  if (!o.requests.empty() && o.sprite_sheet)
    fail(ErrorCode::invalid_option, "requests cannot be combined with a sprite sheet");
  if (o.requests.size() > 1000)
    fail(ErrorCode::invalid_option, "request count must not exceed 1000");
  for (const ThumbnailRequest& request : o.requests) {
    if (request.seek_percentage < 0 || request.seek_percentage > 100)
      fail(ErrorCode::invalid_option, "request seek percentage must be between 0 and 100");
    validate_view(request.view, "request ");
  }
  if (o.sprite_columns > 1000)
    fail(ErrorCode::invalid_option, "sprite columns must be between 0 and 1000");
  if (o.flat_frame_stddev_threshold < 0)
    fail(ErrorCode::invalid_option, "flat frame threshold must not be negative");
  if (o.deovr_packed_alpha &&
      (o.layout != VideoLayout::vr180_sbs ||
       o.input_projection != InputProjection::fisheye))
    fail(ErrorCode::invalid_option,
         "DeoVR packed alpha requires side-by-side fisheye VR180 input");
  if (o.deovr_packed_alpha && o.sprite_sheet)
    fail(ErrorCode::invalid_option,
         "DeoVR packed alpha cannot be combined with a sprite sheet");
}

AVPixelFormat vaapi_format(AVCodecContext*, const AVPixelFormat* formats) {
  for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f)
    if (*f == AV_PIX_FMT_VAAPI) return *f;
  return AV_PIX_FMT_NONE;
}

bool codec_has_vaapi(const AVCodec* decoder) {
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, i);
    if (!config) return false;
    if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
        config->device_type == AV_HWDEVICE_TYPE_VAAPI) return true;
  }
}

const AVCodec* vaapi_decoder(AVCodecID codec_id) {
  void* opaque = nullptr;
  while (const AVCodec* candidate = av_codec_iterate(&opaque)) {
    if (av_codec_is_decoder(candidate) && candidate->id == codec_id &&
        codec_has_vaapi(candidate)) return candidate;
  }
  return nullptr;
}

// Opens the input and selects its video stream; open_decoder() then attaches
// a decoder, and can replace it without reopening the input.
Session open_input(const std::filesystem::path& input) {
  AVFormatContext* raw_format = nullptr;
  int rc = avformat_open_input(&raw_format, input.c_str(), nullptr, nullptr);
  if (rc < 0) fail_av(ErrorCode::open_failed, "cannot open input", rc);
  FormatPtr format(raw_format);
  if ((rc = avformat_find_stream_info(format.get(), nullptr)) < 0)
    fail_av(ErrorCode::open_failed, "cannot read stream information", rc);

  const AVCodec* decoder = nullptr;
  const int stream_index = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO,
                                                -1, -1, &decoder, 0);
  if (stream_index < 0)
    fail_av(ErrorCode::open_failed, "no decodable video stream", stream_index);
  AVStream* stream = format->streams[stream_index];
  if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC)
    fail(ErrorCode::open_failed, "input contains only an attached picture, not a video stream");
  Session session;
  session.format = std::move(format);
  session.stream = stream;
  session.stream_index = stream_index;
  session.best_decoder = decoder;
  return session;
}

void open_decoder(Session& s, DecodeBackend requested, bool auto_eligible) {
  s.codec.reset();
  s.hw_device.reset();
  AVStream* stream = s.stream;
  const AVCodec* decoder = s.best_decoder;
  int rc = 0;
  const bool try_vaapi = requested == DecodeBackend::vaapi ||
                         (requested == DecodeBackend::automatic && auto_eligible);
  // av_find_best_stream() prefers external software decoders such as
  // libdav1d for AV1.  They do not advertise a VA-API configuration, even
  // when FFmpeg's native decoder for the same codec does.  Pick the latter
  // only when hardware decoding was requested; software keeps the best
  // decoder FFmpeg chose.
  if (try_vaapi && !codec_has_vaapi(decoder)) {
    if (const AVCodec* candidate = vaapi_decoder(stream->codecpar->codec_id))
      decoder = candidate;
  }

  AVCodecContext* raw_codec = avcodec_alloc_context3(decoder);
  if (!raw_codec) fail(ErrorCode::open_failed, "cannot allocate decoder");
  CodecPtr codec(raw_codec);
  if ((rc = avcodec_parameters_to_context(codec.get(), stream->codecpar)) < 0)
    fail_av(ErrorCode::open_failed, "cannot configure decoder", rc);

  BufferPtr hw_device;
  DecodeBackend used = DecodeBackend::software;
  if (try_vaapi && codec_has_vaapi(decoder)) {
    AVBufferRef* raw_device = nullptr;
    rc = av_hwdevice_ctx_create(&raw_device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
    if (rc >= 0) {
      hw_device.reset(raw_device);
      AVHWFramesConstraints* constraints =
          av_hwdevice_get_hwframe_constraints(hw_device.get(), nullptr);
      const bool dimensions_supported = !constraints || (
          (constraints->min_width == 0 || codec->width >= constraints->min_width) &&
          (constraints->min_height == 0 || codec->height >= constraints->min_height) &&
          codec->width <= constraints->max_width &&
          codec->height <= constraints->max_height);
      av_hwframe_constraints_free(&constraints);
      if (dimensions_supported) {
        codec->hw_device_ctx = av_buffer_ref(hw_device.get());
        codec->get_format = vaapi_format;
        used = DecodeBackend::vaapi;
      } else if (requested == DecodeBackend::vaapi) {
        fail(ErrorCode::unsupported_backend, "video dimensions exceed VA-API hardware limits");
      } else {
        hw_device.reset();
      }
    } else if (requested == DecodeBackend::vaapi) {
      fail_av(ErrorCode::unsupported_backend, "cannot initialize VA-API", rc);
    }
  } else if (requested == DecodeBackend::vaapi) {
    fail(ErrorCode::unsupported_backend, "the selected decoder does not support VA-API");
  }

  // The demuxer seek itself lands on a keyframe.  Do not set
  // AVDISCARD_NONKEY here: codecs with frame threading may need following
  // reference packets before they emit that first frame.
  codec->skip_frame = AVDISCARD_DEFAULT;
  if ((rc = avcodec_open2(codec.get(), decoder, nullptr)) < 0)
    fail_av(ErrorCode::open_failed, "cannot open decoder", rc);
  s.codec = std::move(codec);
  s.backend = used;
  s.hw_device = std::move(hw_device);
}

double duration_seconds(const Session& s) {
  if (s.stream->duration != AV_NOPTS_VALUE && s.stream->duration > 0)
    return s.stream->duration * av_q2d(s.stream->time_base);
  if (s.format->duration != AV_NOPTS_VALUE && s.format->duration > 0)
    return static_cast<double>(s.format->duration) / AV_TIME_BASE;
  fail(ErrorCode::open_failed, "input has no usable duration");
}

int rotation_degrees(const AVStream* stream) {
  const AVPacketSideData* side = av_packet_side_data_get(
      stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
      AV_PKT_DATA_DISPLAYMATRIX);
  if (!side || side->size < 9 * sizeof(std::int32_t)) return 0;
  double degrees = -av_display_rotation_get(reinterpret_cast<const std::int32_t*>(side->data));
  if (std::isnan(degrees)) return 0;
  int normalized = static_cast<int>(std::lround(degrees / 90.0)) * 90;
  normalized = ((normalized % 360) + 360) % 360;
  return normalized;
}

// What one thumbnail should show: the box its output must fit and, for VR
// input, the view projected into it. These vary per thumbnail with requests
// and sprite cells, while ExtractOptions stays batch-wide.
struct Framing {
  std::uint32_t max_width = 0;
  std::uint32_t max_height = 0;
  View view;
};

std::pair<int, int> output_size(const AVFrame* frame, const AVStream* stream,
                                int rotation, const Framing& framing) {
  AVRational sar = frame->sample_aspect_ratio.num ? frame->sample_aspect_ratio
                                                  : stream->sample_aspect_ratio;
  double display_w = frame->width;
  double display_h = frame->height;
  if (sar.num > 0 && sar.den > 0) display_w *= av_q2d(sar);
  if (rotation == 90 || rotation == 270) std::swap(display_w, display_h);
  const double scale = std::min({static_cast<double>(framing.max_width) / display_w,
                                 static_cast<double>(framing.max_height) / display_h, 1.0});
  int width = std::max(2, static_cast<int>(std::floor(display_w * scale)));
  int height = std::max(2, static_cast<int>(std::floor(display_h * scale)));
  width &= ~1;
  height &= ~1;
  return {width, height};
}

FramePtr transfer_if_needed(const AVFrame* frame) {
  if (frame->format != AV_PIX_FMT_VAAPI) {
    FramePtr clone(av_frame_clone(frame));
    if (!clone) fail(ErrorCode::decode_failed, "cannot clone decoded frame");
    return clone;
  }
  FramePtr software(av_frame_alloc());
  if (!software) fail(ErrorCode::decode_failed, "cannot allocate software frame");
  int rc = av_hwframe_transfer_data(software.get(), frame, 0);
  if (rc < 0) fail_av(ErrorCode::decode_failed, "cannot transfer VA-API frame", rc);
  if ((rc = av_frame_copy_props(software.get(), frame)) < 0)
    fail_av(ErrorCode::decode_failed, "cannot copy frame properties", rc);
  return software;
}

double vertical_fov(double horizontal_fov, double aspect_ratio) {
  const double horizontal_radians = horizontal_fov * std::numbers::pi / 180.0;
  return 2.0 * std::atan(std::tan(horizontal_radians / 2.0) / aspect_ratio) *
         180.0 / std::numbers::pi;
}

// A buffersrc -> filters -> buffersink graph, configured from the first frame
// it receives and then run one frame at a time.
class FilterGraph {
 public:
  explicit FilterGraph(const char* purpose) : purpose_(purpose) {}

  bool initialized() const { return graph_ != nullptr; }

  void initialize(const AVFrame* frame, const std::string& filters) {
    graph_.reset(avfilter_graph_alloc());
    if (!graph_) fail_filter("cannot allocate", "graph");

    const AVFilter* buffer = avfilter_get_by_name("buffer");
    const AVFilter* buffersink = avfilter_get_by_name("buffersink");
    if (!buffer || !buffersink)
      fail(ErrorCode::decode_failed, "required FFmpeg filters are unavailable");

    source_ = avfilter_graph_alloc_filter(graph_.get(), buffer, "in");
    if (!source_) fail_filter("cannot allocate", "input");
    AVBufferSrcParameters* parameters = av_buffersrc_parameters_alloc();
    if (!parameters) fail_filter("cannot allocate", "parameters");
    parameters->format = frame->format;
    parameters->width = frame->width;
    parameters->height = frame->height;
    parameters->time_base = {1, 1000000};
    parameters->sample_aspect_ratio = frame->sample_aspect_ratio.num
                                          ? frame->sample_aspect_ratio
                                          : AVRational{1, 1};
    parameters->color_space = frame->colorspace;
    parameters->color_range = frame->color_range;
    if (frame->hw_frames_ctx) {
      parameters->hw_frames_ctx = av_buffer_ref(frame->hw_frames_ctx);
      if (!parameters->hw_frames_ctx) {
        av_free(parameters);
        fail(ErrorCode::decode_failed, "cannot retain hardware frame context");
      }
    }
    int rc = av_buffersrc_parameters_set(source_, parameters);
    av_buffer_unref(&parameters->hw_frames_ctx);
    av_free(parameters);
    if (rc < 0) fail_filter("cannot configure", "input", rc);
    if ((rc = avfilter_init_str(source_, nullptr)) < 0)
      fail_filter("cannot initialize", "input", rc);
    if ((rc = avfilter_graph_create_filter(&sink_, buffersink, "out", nullptr,
                                            nullptr, graph_.get())) < 0)
      fail_filter("cannot create", "output", rc);

    FilterInOutPtr outputs(avfilter_inout_alloc());
    FilterInOutPtr inputs(avfilter_inout_alloc());
    if (!outputs || !inputs) fail_filter("cannot allocate", "links");
    outputs->name = av_strdup("in");
    outputs->filter_ctx = source_;
    outputs->pad_idx = 0;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = sink_;
    inputs->pad_idx = 0;
    AVFilterInOut* raw_inputs = inputs.release();
    AVFilterInOut* raw_outputs = outputs.release();
    rc = avfilter_graph_parse_ptr(graph_.get(), filters.c_str(), &raw_inputs,
                                  &raw_outputs, nullptr);
    avfilter_inout_free(&raw_inputs);
    avfilter_inout_free(&raw_outputs);
    if (rc < 0) fail_filter("cannot parse", "graph", rc);
    if ((rc = avfilter_graph_config(graph_.get(), nullptr)) < 0)
      fail_filter("cannot configure", "graph", rc);
  }

  FramePtr apply(const AVFrame* input) {
    FramePtr submitted(av_frame_clone(input));
    if (!submitted) fail_filter("cannot clone", "input frame");
    int rc = av_buffersrc_add_frame_flags(source_, submitted.get(), 0);
    if (rc < 0) fail_filter("cannot submit", "frame", rc);
    FramePtr output(av_frame_alloc());
    if (!output) fail_filter("cannot allocate", "output frame");
    rc = av_buffersink_get_frame(sink_, output.get());
    if (rc < 0) fail_filter("cannot receive", "frame", rc);
    return output;
  }

 private:
  [[noreturn]] void fail_filter(const char* action, const char* object) const {
    fail(ErrorCode::decode_failed,
         std::string(action) + ' ' + purpose_ + " filter " + object);
  }
  [[noreturn]] void fail_filter(const char* action, const char* object, int rc) const {
    fail_av(ErrorCode::decode_failed,
            std::string(action) + ' ' + purpose_ + " filter " + object, rc);
  }

  const char* purpose_;
  FilterGraphPtr graph_;
  AVFilterContext* source_ = nullptr;
  AVFilterContext* sink_ = nullptr;
};

class VrFilter {
 public:
  FramePtr apply(const AVFrame* input, const ExtractOptions& options, const Framing& framing) {
    if (!graph_.initialized())
      graph_.initialize(input, description(input, options, framing));
    return graph_.apply(input);
  }

 private:
  static std::string description(const AVFrame* frame, const ExtractOptions& options,
                                 const Framing& framing) {
    const bool sbs = options.layout == VideoLayout::vr180_sbs;
    const bool hardware = frame->format == AV_PIX_FMT_VAAPI;
    const unsigned eye = options.vr_eye_size;
    std::ostringstream filters;
    if (hardware && options.deovr_packed_alpha) {
      // Preserve the packed tiles at source resolution. Scaling the complete
      // stereo surface before extracting its small mask tiles changes their
      // pixel-centre registration relative to the colour eye. This download
      // matches the precise FFmpeg fallback while retaining one decoder
      // session for the whole batch.
      filters << "hwdownload,format=nv12,";
    } else if (hardware && sbs) {
      filters << "scale_vaapi=w=" << eye * 2 << ":h=" << eye
              << ":format=nv12:mode=fast,hwdownload,format=nv12,";
    } else if (hardware) {
      // TB VR180 fisheye commonly stores a square eye centred in a 2:1
      // half-frame. Download before cropping so the padded sides are removed
      // rather than squeezed into the projection by a hardware scale.
      filters << "hwdownload,format=nv12,";
    }
    const View& view = framing.view;
    const double vfov = vertical_fov(view.horizontal_fov,
                                      static_cast<double>(framing.max_width) /
                                      framing.max_height);
    auto projection = [&] {
      std::ostringstream value;
      value << "v360=input="
            << (options.input_projection == InputProjection::fisheye
                    ? "fisheye" : "hequirect")
            << ":ih_fov=" << options.input_horizontal_fov
            << ":iv_fov=" << options.input_horizontal_fov
            << ":output=flat:w=" << framing.max_width
            << ":h=" << framing.max_height << ":yaw=" << view.yaw
            << ":pitch=" << view.pitch << ":h_fov=" << view.horizontal_fov
            << ":v_fov=" << vfov;
      return value.str();
    };

    if (options.deovr_packed_alpha) {
      // Produce a real alpha channel for the cut-out subject instead of
      // baking a gradient backdrop and drop shadow into opaque pixels. The
      // caller (a browser CSS background, or the WebGL player's own
      // procedural backdrop) draws the surrounding scene itself, so there is
      // no accent colour or shadow work to do in this graph any more.
      const std::string project = projection();
      filters << "split=3[colour][mask_top_source][mask_bottom_source];"
              << "[colour]crop=iw/2:ih:0:0,setsar=1," << project
              << ",format=gbrp[projected_colour];"
              << "[mask_top_source]crop=iw/5:ih/5:iw*2/5:ih*4/5[mask_top];"
              << "[mask_bottom_source]crop=iw/5:ih/5:iw*2/5:0[mask_bottom];"
              << "[mask_top][mask_bottom]vstack,format=gbrp,"
              << "geq=r='max(r(X,Y)-max(g(X,Y),b(X,Y)),0)':"
              << "g='max(r(X,Y)-max(g(X,Y),b(X,Y)),0)':"
              << "b='max(r(X,Y)-max(g(X,Y),b(X,Y)),0)',"
              << "extractplanes=r," << project
              << ",gblur=sigma=1,lut=y='clip((val*4-20.4)/.92,0,255)',format=gray[subject_mask];"
              << "[projected_colour][subject_mask]alphamerge,format=yuva420p";
    } else {
      if (sbs) {
        filters << "crop=iw/2:ih:0:0,";
      } else {
        filters << "crop=iw:ih/2:0:0,";
        // A fisheye image is round, so a top/bottom eye stores it inscribed in
        // a centred square and pads the sides. An equirectangular eye fills
        // its rectangle instead, and squaring it would discard half the
        // horizontal field.
        if (options.input_projection == InputProjection::fisheye)
          filters << "crop=ih:ih:(iw-ih)/2:0,";
      }
      if (!hardware || !sbs)
        filters << "scale=" << eye << ':' << eye << ":flags=fast_bilinear,";
      filters << "setsar=1," << projection();
    }

    return filters.str();
  }

  FilterGraph graph_{"VR"};
};

bool is_hdr(const AVFrame* frame) {
  return frame->color_trc == AVCOL_TRC_SMPTE2084 ||
         frame->color_trc == AVCOL_TRC_ARIB_STD_B67;
}

class ToneMapFilter {
 public:
  FramePtr apply(const AVFrame* input) {
    if (!graph_.initialized()) {
      // zscale interprets the frame's PQ/HLG metadata, tonemap operates in
      // linear light, and the second zscale produces display-ready SDR BT.709.
      // Alpha-capable formats carry a packed-alpha cut-out through unchanged.
      const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(input->format));
      const bool alpha = desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA);
      graph_.initialize(input,
                        std::string("zscale=transfer=linear:npl=100,format=") +
                            (alpha ? "gbrapf32le" : "gbrpf32le") +
                            ",tonemap=tonemap=mobius:desat=0,"
                            "zscale=primaries=bt709:transfer=bt709:matrix=bt709:range=limited,"
                            "format=" + (alpha ? "yuva420p" : "yuv420p"));
    }
    return graph_.apply(input);
  }

 private:
  FilterGraph graph_{"tone-mapping"};
};

enum class SeekMode { keyframe, exact };

enum class SeekStrategy {
  // Decode from the first keyframe after wherever the seek lands, and report
  // an overshoot if that keyframe is already past the target.
  direct,
  // Keyframe mode after an overshoot: the seek landed an unknown distance
  // before the target, so decode only keyframe packets up to the target and
  // keep the last, instead of decoding every frame in between.
  scan_keyframes,
  // The seek went back to the stream start: send every packet, in case the
  // demuxer marks no keyframes, and accept the first frame even when it is
  // past the target, since nothing earlier exists.
  from_start,
};

struct SeekAttempt {
  FramePtr frame;
  bool overshot = false;
};

// One seek-and-decode attempt; decode_frame() below chains them.
SeekAttempt decode_after_seek(Session& s, std::int64_t seek_ts, std::int64_t target_ts,
                              SeekMode mode, SeekStrategy strategy) {
  int rc = av_seek_frame(s.format.get(), s.stream_index, seek_ts, AVSEEK_FLAG_BACKWARD);
  if (rc < 0) fail_av(ErrorCode::decode_failed, "keyframe seek failed", rc);
  avcodec_flush_buffers(s.codec.get());

  PacketPtr packet(av_packet_alloc());
  FramePtr frame(av_frame_alloc());
  if (!packet || !frame) fail(ErrorCode::decode_failed, "cannot allocate decode buffers");
  FramePtr last;
  bool first = true;
  bool overshot = false;
  // Seeks into containers without an index can land mid-GOP. Packets before
  // the next keyframe cannot decode cleanly (VA-API rejects them outright),
  // and skipping them reveals an overshoot before anything is decoded.
  bool awaiting_keyframe = strategy != SeekStrategy::from_start;
  // Drains every frame the decoder has ready; true once `frame` is the result
  // or the seek is known to have overshot.
  auto receive = [&] {
    int received;
    while ((received = avcodec_receive_frame(s.codec.get(), frame.get())) >= 0) {
      const auto pts = frame->best_effort_timestamp;
      const bool past_target = pts != AV_NOPTS_VALUE && pts > target_ts;
      if (std::exchange(first, false) && past_target &&
          strategy == SeekStrategy::direct) {
        overshot = true;
        return true;
      }
      if (mode == SeekMode::keyframe && strategy != SeekStrategy::scan_keyframes)
        return true;
      if (mode == SeekMode::exact && (pts == AV_NOPTS_VALUE || pts >= target_ts))
        return true;
      last.reset(av_frame_clone(frame.get()));
      av_frame_unref(frame.get());
    }
    if (received != AVERROR(EAGAIN) && received != AVERROR_EOF)
      fail_av(ErrorCode::decode_failed, "decoding failed", received);
    return false;
  };
  auto result = [&]() -> SeekAttempt {
    if (overshot) return {nullptr, true};
    return {std::move(frame), false};
  };
  constexpr std::size_t max_packets_per_seek = 10000;
  std::size_t packets_read = 0;
  while ((rc = av_read_frame(s.format.get(), packet.get())) >= 0) {
    if (++packets_read > max_packets_per_seek)
      fail(ErrorCode::decode_failed, "seek exceeded packet limit");
    if (packet->stream_index != s.stream_index) {
      av_packet_unref(packet.get());
      continue;
    }
    const bool key = packet->flags & AV_PKT_FLAG_KEY;
    const auto packet_ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    const bool past_target = packet_ts != AV_NOPTS_VALUE && packet_ts > target_ts;
    if (strategy == SeekStrategy::scan_keyframes) {
      if (past_target) {
        av_packet_unref(packet.get());
        break;
      }
      if (!key) {
        av_packet_unref(packet.get());
        continue;
      }
    } else if (awaiting_keyframe) {
      if (!key) {
        av_packet_unref(packet.get());
        continue;
      }
      awaiting_keyframe = false;
      if (past_target) {
        av_packet_unref(packet.get());
        return {nullptr, true};
      }
    }
    rc = avcodec_send_packet(s.codec.get(), packet.get());
    if (rc == AVERROR(EAGAIN)) {
      // The decoder wants its output read before it accepts this packet.
      if (receive()) return result();
      rc = avcodec_send_packet(s.codec.get(), packet.get());
    }
    av_packet_unref(packet.get());
    if (rc < 0) fail_av(ErrorCode::decode_failed, "decoder rejected packet", rc);
    if (receive()) return result();
  }
  // Decoders with reordering or frame-threading delay hold frames back until
  // flushed, so a seek into a short final GOP needs the tail drained.
  if (avcodec_send_packet(s.codec.get(), nullptr) >= 0 && receive()) return result();
  if (last) return {std::move(last), false};
  // Nothing decodable after the landing point, e.g. it fell after the final
  // keyframe or a scan met no keyframe before the target: it started too late.
  if (strategy != SeekStrategy::from_start) return {nullptr, true};
  fail(ErrorCode::decode_failed, mode == SeekMode::keyframe
           ? "no preceding keyframe could be decoded"
           : "no frame could be decoded at or after target timestamp");
}

// Seeks to the keyframe preceding target_ts and decodes from there. In
// keyframe mode that keyframe is the result. In exact mode decoding continues
// to the first frame at or after target_ts, matching the semantics of FFmpeg's
// combined fast/output -ss seek, and falls back to the last decodable frame if
// target_ts is beyond the stream's end.
FramePtr decode_frame(Session& s, std::int64_t target_ts, SeekMode mode) {
  SeekAttempt attempt = decode_after_seek(s, target_ts, target_ts, mode, SeekStrategy::direct);
  if (!attempt.overshot) return std::move(attempt.frame);

  // Containers without a seek index, such as MPEG-TS, can land after the
  // keyframe preceding the target, so decoding first reaches a later one.
  // Seek from progressively further back until the target is covered.
  const std::int64_t start = s.stream->start_time == AV_NOPTS_VALUE ? 0 : s.stream->start_time;
  const std::int64_t second = av_rescale_q(1, AVRational{1, 1}, s.stream->time_base);
  for (std::int64_t backoff = second; target_ts - backoff > start; backoff *= 2) {
    attempt = decode_after_seek(s, target_ts - backoff, target_ts, mode,
                                mode == SeekMode::keyframe ? SeekStrategy::scan_keyframes
                                                           : SeekStrategy::direct);
    if (!attempt.overshot) return std::move(attempt.frame);
  }
  // A seek to the start timestamp itself can land past the first keyframe,
  // so aim a second before it.
  if (mode == SeekMode::keyframe) {
    attempt = decode_after_seek(s, start - second, target_ts, mode,
                                SeekStrategy::scan_keyframes);
    if (!attempt.overshot) return std::move(attempt.frame);
  }
  return std::move(
      decode_after_seek(s, start - second, target_ts, mode, SeekStrategy::from_start).frame);
}

std::vector<unsigned char> rotate_rgb(const std::vector<unsigned char>& src,
                                      int src_w, int src_h, int rotation) {
  if (rotation == 0) return src;
  const int dst_w = (rotation == 90 || rotation == 270) ? src_h : src_w;
  const int dst_h = (rotation == 90 || rotation == 270) ? src_w : src_h;
  std::vector<unsigned char> dst(static_cast<std::size_t>(dst_w) * dst_h * 3);
  for (int y = 0; y < src_h; ++y) for (int x = 0; x < src_w; ++x) {
    int dx = x, dy = y;
    if (rotation == 90) { dx = src_h - 1 - y; dy = x; }
    else if (rotation == 180) { dx = src_w - 1 - x; dy = src_h - 1 - y; }
    else if (rotation == 270) { dx = y; dy = src_w - 1 - x; }
    std::memcpy(&dst[(static_cast<std::size_t>(dy) * dst_w + dx) * 3],
                &src[(static_cast<std::size_t>(y) * src_w + x) * 3], 3);
  }
  return dst;
}

struct ScaledFrame {
  std::vector<unsigned char> rgb;
  int width = 0;
  int height = 0;
  // True when `rgb` is actually packed RGBA (4 bytes/pixel), e.g. a
  // packed-alpha DeoVR composite. The name stays `rgb` for the common case
  // rather than threading a rename through every caller.
  bool has_alpha = false;
  int channels() const { return has_alpha ? 4 : 3; }
};

ScaledFrame scale_frame(const AVFrame* decoded, const AVStream* stream, int rotation,
                        const ExtractOptions& o, const Framing& framing,
                        VrFilter* vr_filter,
                        ToneMapFilter* tone_map_filter) {
  FramePtr frame = vr_filter ? vr_filter->apply(decoded, o, framing)
                             : transfer_if_needed(decoded);
  if (is_hdr(decoded)) frame = tone_map_filter->apply(frame.get());
  const int effective_rotation = vr_filter ? 0 : rotation;
  auto [final_w, final_h] = output_size(frame.get(), stream, effective_rotation, framing);
  const int scale_w = (effective_rotation == 90 || effective_rotation == 270)
                          ? final_h : final_w;
  const int scale_h = (effective_rotation == 90 || effective_rotation == 270)
                          ? final_w : final_h;

  const auto* format_desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
  const bool has_alpha = format_desc && (format_desc->flags & AV_PIX_FMT_FLAG_ALPHA);
  const AVPixelFormat dst_format = has_alpha ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24;
  const int channels = has_alpha ? 4 : 3;
  SwsPtr sws(sws_getContext(frame->width, frame->height,
                            static_cast<AVPixelFormat>(frame->format),
                            scale_w, scale_h, dst_format,
                            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr));
  if (!sws) fail(ErrorCode::encode_failed, "cannot initialize image scaler");
  std::vector<unsigned char> rgb(static_cast<std::size_t>(scale_w) * scale_h * channels);
  std::array<std::uint8_t*, 4> dst{rgb.data(), nullptr, nullptr, nullptr};
  std::array<int, 4> stride{scale_w * channels, 0, 0, 0};
  int rc = sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height,
                     dst.data(), stride.data());
  if (rc != scale_h) fail(ErrorCode::encode_failed, "image scaling failed");
  if (has_alpha) {
    // The packed-alpha graph only ever runs through the VR path, which
    // forces effective_rotation to 0, so there is no rotated-RGBA case to
    // support in rotate_rgb.
    return {std::move(rgb), final_w, final_h, true};
  }
  rgb = rotate_rgb(rgb, scale_w, scale_h, effective_rotation);
  return {std::move(rgb), final_w, final_h, false};
}

double luma_stddev(const ScaledFrame& scaled) {
  const std::size_t pixel_count =
      static_cast<std::size_t>(scaled.width) * scaled.height;
  if (pixel_count == 0) return 0;
  double sum = 0;
  double sum_squares = 0;
  const int channels = scaled.channels();
  for (std::size_t i = 0; i < pixel_count; ++i) {
    const unsigned char* p = &scaled.rgb[i * static_cast<std::size_t>(channels)];
    // Rec. 601 luma weights; a cheap-enough approximation for flat-frame
    // detection that we don't need to match any particular color space.
    const double y = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
    sum += y;
    sum_squares += y * y;
  }
  const double mean = sum / static_cast<double>(pixel_count);
  const double variance = sum_squares / static_cast<double>(pixel_count) - mean * mean;
  return std::sqrt(std::max(variance, 0.0));
}

struct DecodedThumbnail {
  FramePtr frame;
  ScaledFrame scaled;
};

// Decodes the keyframe at target_ts and scales it. If skip_flat_frames is
// set and the frame looks flat (low luma variance, e.g. a black frame, fade,
// or title card), nudges the seek target forward by fixed steps and retries
// a bounded number of times, keeping the least-flat candidate seen if none
// clear the threshold.
DecodedThumbnail decode_avoiding_flat_frames(Session& s, std::int64_t start_ts,
                                             std::int64_t target_ts,
                                             double target_seconds, int rotation,
                                             const ExtractOptions& o, const Framing& framing,
                                             VrFilter* vr_filter,
                                             ToneMapFilter* tone_map_filter,
                                             Timings& timings, SeekMode mode) {
  constexpr int max_attempts = 5;  // original attempt plus up to 4 retries
  constexpr double retry_step_seconds = 0.5;

  DecodedThumbnail best;
  double best_stddev = -1;
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    const double attempt_seconds = target_seconds + attempt * retry_step_seconds;
    const std::int64_t attempt_ts = start_ts + av_rescale_q(
        static_cast<std::int64_t>(attempt_seconds * AV_TIME_BASE), AV_TIME_BASE_Q,
        s.stream->time_base);
    const std::int64_t seek_target = attempt == 0 ? target_ts : attempt_ts;
    const auto decode_start = Clock::now();
    FramePtr frame = decode_frame(s, seek_target, mode);
    timings.seek_decode_ms += elapsed_ms(decode_start);

    const auto encode_start = Clock::now();
    ScaledFrame scaled = scale_frame(frame.get(), s.stream, rotation, o, framing, vr_filter,
                                     tone_map_filter);
    timings.scale_encode_ms += elapsed_ms(encode_start);

    if (!o.skip_flat_frames) return {std::move(frame), std::move(scaled)};

    const double stddev = luma_stddev(scaled);
    if (stddev >= o.flat_frame_stddev_threshold)
      return {std::move(frame), std::move(scaled)};
    if (stddev > best_stddev) {
      best_stddev = stddev;
      best = {std::move(frame), std::move(scaled)};
    }
  }
  return best;
}

std::vector<unsigned char> encode_jpeg(const unsigned char* rgb, int width, int height,
                                       int quality) {
  TjPtr tj(tjInitCompress());
  if (!tj) fail(ErrorCode::encode_failed, "cannot initialize JPEG encoder");
  unsigned char* jpeg = nullptr;
  unsigned long jpeg_size = 0;
  if (tjCompress2(tj.get(), const_cast<unsigned char*>(rgb), width, width * 3, height,
                  TJPF_RGB, &jpeg, &jpeg_size, TJSAMP_420, quality, TJFLAG_FASTDCT) < 0)
    fail(ErrorCode::encode_failed, std::string("JPEG encoding failed: ") + tjGetErrorStr2(tj.get()));
  std::vector<unsigned char> bytes(jpeg, jpeg + jpeg_size);
  tjFree(jpeg);
  return bytes;
}

std::vector<unsigned char> encode_webp(const unsigned char* rgb, int width, int height,
                                       int quality) {
  std::uint8_t* webp = nullptr;
  const std::size_t size = WebPEncodeRGB(rgb, width, height, width * 3,
                                        static_cast<float>(quality), &webp);
  if (size == 0 || !webp) fail(ErrorCode::encode_failed, "WebP encoding failed");
  std::vector<unsigned char> bytes(webp, webp + size);
  WebPFree(webp);
  return bytes;
}

std::vector<unsigned char> encode_webp_rgba(const unsigned char* rgba, int width, int height,
                                            int quality) {
  std::uint8_t* webp = nullptr;
  const std::size_t size = WebPEncodeRGBA(rgba, width, height, width * 4,
                                          static_cast<float>(quality), &webp);
  if (size == 0 || !webp) fail(ErrorCode::encode_failed, "WebP encoding failed");
  std::vector<unsigned char> bytes(webp, webp + size);
  WebPFree(webp);
  return bytes;
}

std::vector<unsigned char> encode_image(const unsigned char* rgb, int width, int height,
                                        const ExtractOptions& o, bool has_alpha = false) {
  if (has_alpha) {
    if (o.format != OutputFormat::webp)
      fail(ErrorCode::invalid_option, "alpha output requires the WebP format");
    return encode_webp_rgba(rgb, width, height, o.quality);
  }
  switch (o.format) {
    case OutputFormat::jpeg: return encode_jpeg(rgb, width, height, o.quality);
    case OutputFormat::webp: return encode_webp(rgb, width, height, o.quality);
  }
  fail(ErrorCode::invalid_option, "unsupported output format");
}

EncodedThumbnail encode_thumbnail(const ScaledFrame& scaled, const ExtractOptions& o,
                                  Thumbnail metadata) {
  auto bytes = encode_image(scaled.rgb.data(), scaled.width, scaled.height, o, scaled.has_alpha);
  metadata.width = scaled.width;
  metadata.height = scaled.height;
  return {std::move(metadata), std::move(bytes)};
}

std::pair<std::uint32_t, std::uint32_t> sprite_grid(std::uint32_t count, std::uint32_t requested_columns) {
  std::uint32_t columns = requested_columns;
  if (columns == 0) {
    columns = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(count))));
  }
  columns = std::min(columns, count);
  const std::uint32_t rows = (count + columns - 1) / columns;
  return {columns, rows};
}

// Copies an RGB tile, which must fit within one cell, centred into its grid
// cell; the rest of the cell keeps the canvas's black background.
void blit_tile(std::vector<unsigned char>& canvas, int canvas_w, int cell_w, int cell_h,
               const ScaledFrame& tile, int col, int row) {
  const std::size_t dst_x =
      static_cast<std::size_t>(col) * cell_w + (cell_w - tile.width) / 2;
  const std::size_t dst_y =
      static_cast<std::size_t>(row) * cell_h + (cell_h - tile.height) / 2;
  for (int y = 0; y < tile.height; ++y) {
    unsigned char* dst = &canvas[((dst_y + y) * canvas_w + dst_x) * 3];
    const unsigned char* src = &tile.rgb[static_cast<std::size_t>(y) * tile.width * 3];
    std::memcpy(dst, src, static_cast<std::size_t>(tile.width) * 3);
  }
}

std::string vtt_timestamp(double seconds) {
  if (seconds < 0) seconds = 0;
  const auto total_ms = static_cast<std::int64_t>(std::llround(seconds * 1000.0));
  const std::int64_t ms = total_ms % 1000;
  const std::int64_t total_s = total_ms / 1000;
  const std::int64_t s = total_s % 60;
  const std::int64_t total_m = total_s / 60;
  const std::int64_t m = total_m % 60;
  const std::int64_t h = total_m / 60;
  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << h << ':' << std::setw(2) << m << ':'
      << std::setw(2) << s << '.' << std::setw(3) << ms;
  return out.str();
}

std::string sprite_vtt(const std::string& sprite_name,
                       const std::vector<Thumbnail>& thumbnails, double duration_seconds,
                       int tile_w, int tile_h, std::uint32_t columns) {
  std::ostringstream out;
  out << "WEBVTT\n\n";
  for (std::size_t i = 0; i < thumbnails.size(); ++i) {
    const double start = thumbnails[i].target_seconds;
    const double end = i + 1 < thumbnails.size() ? thumbnails[i + 1].target_seconds
                                                  : duration_seconds;
    const std::uint32_t col = static_cast<std::uint32_t>(i) % columns;
    const std::uint32_t row = static_cast<std::uint32_t>(i) / columns;
    out << vtt_timestamp(start) << " --> " << vtt_timestamp(std::max(end, start)) << '\n'
        << sprite_name << "#xywh=" << (col * tile_w) << ',' << (row * tile_h) << ','
        << tile_w << ',' << tile_h << "\n\n";
  }
  return out.str();
}

std::filesystem::path thumbnail_path(const std::filesystem::path& dir,
                                     std::uint32_t index, std::uint32_t count,
                                     OutputFormat format) {
  const int digits = std::max(4, static_cast<int>(std::to_string(count).size()));
  std::ostringstream name;
  name << "thumb-" << std::setw(digits) << std::setfill('0') << index << '.'
       << format_extension(format);
  return dir / name.str();
}

std::string_view as_chars(const std::vector<unsigned char>& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Writes `<path>.tmp` and renames it into place, so no partially written
// output is ever visible under its final name and no temporary survives a
// failure. `what` names the output in error messages.
void write_file_atomic(const std::filesystem::path& path, std::string_view contents,
                       bool overwrite, const char* what) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) throw Error(ErrorCode::io_error, "cannot create output directory: " + ec.message());
  if (!overwrite && std::filesystem::exists(path))
    throw Error(ErrorCode::io_error, "output already exists: " + path.string());

  auto temp = path;
  temp += ".tmp";
  std::ofstream out(temp, std::ios::binary | std::ios::trunc);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  out.close();
  if (!out) {
    std::filesystem::remove(temp, ec);
    throw Error(ErrorCode::io_error, std::string("cannot write ") + what + ": " + temp.string());
  }
  if (overwrite) std::filesystem::remove(path, ec);
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    const std::string message = ec.message();
    std::filesystem::remove(temp, ec);
    throw Error(ErrorCode::io_error, std::string("cannot finalize ") + what + ": " + message);
  }
}

}  // namespace

const char* backend_name(DecodeBackend backend) noexcept {
  switch (backend) {
    case DecodeBackend::automatic: return "auto";
    case DecodeBackend::software: return "software";
    case DecodeBackend::vaapi: return "vaapi";
  }
  return "unknown";
}

const char* format_extension(OutputFormat format) noexcept {
  switch (format) {
    case OutputFormat::jpeg: return "jpg";
    case OutputFormat::webp: return "webp";
  }
  return "bin";
}

const char* error_code_name(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::invalid_option: return "invalid_option";
    case ErrorCode::open_failed: return "open_failed";
    case ErrorCode::unsupported_backend: return "unsupported_backend";
    case ErrorCode::decode_failed: return "decode_failed";
    case ErrorCode::encode_failed: return "encode_failed";
    case ErrorCode::io_error: return "io_error";
  }
  return "unknown";
}

ExtractionResult Extractor::extract(const std::filesystem::path& input,
                                    const std::filesystem::path& output_directory,
                                    const ExtractOptions& options) const {
  validate(options);
  // The packed-alpha composite always needs a real alpha channel now (see
  // VrFilter below), and JPEG can't carry one, so alpha requests are forced
  // to WebP here rather than left for every downstream site to remember.
  ExtractOptions o = options;
  if (o.deovr_packed_alpha) o.format = OutputFormat::webp;
  const auto total_start = Clock::now();
  const auto probe_start = Clock::now();

  Session session = open_input(input);
  // Automatic decoding only tries VA-API where it pays off: 4K and larger,
  // in either orientation.
  const AVCodecParameters* parameters = session.stream->codecpar;
  const auto [short_side, long_side] = std::minmax(parameters->width, parameters->height);
  const bool auto_eligible = long_side >= 3840 && short_side >= 2160;
  open_decoder(session, o.backend, auto_eligible);
  if (o.backend == DecodeBackend::automatic &&
      session.backend == DecodeBackend::vaapi) {
    const std::int64_t probe_ts = session.stream->start_time == AV_NOPTS_VALUE
                                      ? 0 : session.stream->start_time;
    try {
      // Device constraints are not reliable on every VA-API driver. Decode one
      // frame before producing output so an unsupported stream can safely fall
      // back without leaving a partial batch behind.
      decode_frame(session, probe_ts, SeekMode::keyframe);
    } catch (const Error&) {
      open_decoder(session, DecodeBackend::software, false);
    }
  }

  ExtractionResult result;
  result.backend_used = session.backend;
  result.video.duration_seconds = duration_seconds(session);
  result.video.coded_width = session.codec->width;
  result.video.coded_height = session.codec->height;
  result.video.codec = avcodec_get_name(session.codec->codec_id);
  result.timings.probe_ms = elapsed_ms(probe_start);
  const int rotation = rotation_degrees(session.stream);
  const std::int64_t start_ts = session.stream->start_time == AV_NOPTS_VALUE
                                    ? 0 : session.stream->start_time;

  const bool use_exact_seconds = !o.seek_seconds.empty();
  const std::uint32_t output_count = !o.requests.empty()
      ? static_cast<std::uint32_t>(o.requests.size())
      : use_exact_seconds
      ? static_cast<std::uint32_t>(o.seek_seconds.size())
      : (o.seek_percentages.empty() ? o.count
             : static_cast<std::uint32_t>(o.seek_percentages.size()));
  VrFilter vr_filter;
  ToneMapFilter tone_map_filter;
  VrFilter* active_vr_filter = o.layout == VideoLayout::flat
                                   ? nullptr : &vr_filter;

  const std::filesystem::path sprite_path =
      output_directory / (std::string("sprite.") + format_extension(o.format));
  const std::filesystem::path vtt_path = output_directory / "sprite.vtt";
  if (!o.overwrite) {
    if (o.sprite_sheet) {
      if (std::filesystem::exists(sprite_path))
        throw Error(ErrorCode::io_error, "output already exists: " + sprite_path.string());
      if (std::filesystem::exists(vtt_path))
        throw Error(ErrorCode::io_error, "output already exists: " + vtt_path.string());
    } else {
      for (std::uint32_t i = 0; i < output_count; ++i) {
        const auto path = thumbnail_path(output_directory, i + 1, output_count, o.format);
        if (std::filesystem::exists(path))
          throw Error(ErrorCode::io_error, "output already exists: " + path.string());
      }
    }
  }

  const auto [sprite_columns, sprite_rows] =
      o.sprite_sheet ? sprite_grid(output_count, o.sprite_columns)
                           : std::pair<std::uint32_t, std::uint32_t>{0, 0};
  std::vector<unsigned char> sprite_canvas;
  int sprite_tile_w = 0;
  int sprite_tile_h = 0;

  for (std::uint32_t i = 0; i < output_count; ++i) {
    Framing framing{o.max_width, o.max_height,
                    o.requests.empty() ? o.view : o.requests[i].view};
    if (o.sprite_sheet && !sprite_canvas.empty()) {
      // The first tile fixes the cell size. Later frames can differ in size
      // or aspect ratio after a mid-stream resolution change, so fit them
      // inside that cell rather than the requested box.
      framing.max_width = static_cast<std::uint32_t>(sprite_tile_w);
      framing.max_height = static_cast<std::uint32_t>(sprite_tile_h);
    }
    const double target_seconds = !o.requests.empty()
        ? result.video.duration_seconds * o.requests[i].seek_percentage / 100.0
        : use_exact_seconds ? o.seek_seconds[i]
        : (o.seek_percentages.empty()
               ? result.video.duration_seconds * (i + 1.0) / (output_count + 1.0)
               : result.video.duration_seconds * o.seek_percentages[i] / 100.0);
    const std::int64_t relative_ts = av_rescale_q(
        static_cast<std::int64_t>(target_seconds * AV_TIME_BASE), AV_TIME_BASE_Q,
        session.stream->time_base);
    const std::int64_t target_ts = start_ts + relative_ts;
    VrFilter request_vr_filter;
    VrFilter* request_active_vr_filter = !o.requests.empty() && active_vr_filter
                                             ? &request_vr_filter : active_vr_filter;
    DecodedThumbnail decoded = decode_avoiding_flat_frames(
        session, start_ts, target_ts, target_seconds, rotation, o, framing,
        request_active_vr_filter, &tone_map_filter, result.timings,
        use_exact_seconds ? SeekMode::exact : SeekMode::keyframe);

    Thumbnail metadata;
    metadata.index = i + 1;
    metadata.target_seconds = target_seconds;
    const std::int64_t pts = decoded.frame->best_effort_timestamp;
    metadata.source_seconds = pts == AV_NOPTS_VALUE ? target_seconds
        : (pts - start_ts) * av_q2d(session.stream->time_base);

    if (o.sprite_sheet) {
      const ScaledFrame& scaled = decoded.scaled;
      if (sprite_canvas.empty()) {
        sprite_tile_w = scaled.width;
        sprite_tile_h = scaled.height;
        sprite_canvas.assign(static_cast<std::size_t>(sprite_tile_w) * sprite_columns *
                                 sprite_tile_h * sprite_rows * 3,
                             0);
      }
      const std::uint32_t col = i % sprite_columns;
      const std::uint32_t row = i / sprite_columns;
      blit_tile(sprite_canvas, sprite_tile_w * static_cast<int>(sprite_columns),
                sprite_tile_w, sprite_tile_h, scaled, static_cast<int>(col),
                static_cast<int>(row));
      metadata.width = sprite_tile_w;
      metadata.height = sprite_tile_h;
      result.thumbnails.push_back(metadata);
      if (o.on_thumbnail) o.on_thumbnail(result.thumbnails.back());
      continue;
    }

    metadata.path = thumbnail_path(output_directory, i + 1, output_count, o.format);
    auto encoded = encode_thumbnail(decoded.scaled, o, std::move(metadata));

    const auto write_start = Clock::now();
    write_file_atomic(encoded.metadata.path, as_chars(encoded.bytes), o.overwrite,
                      "thumbnail");
    result.timings.write_ms += elapsed_ms(write_start);
    result.thumbnails.push_back(std::move(encoded.metadata));
    if (o.on_thumbnail) o.on_thumbnail(result.thumbnails.back());
  }

  if (o.sprite_sheet && !result.thumbnails.empty()) {
    const auto write_start = Clock::now();
    const auto sprite_bytes = encode_image(sprite_canvas.data(),
                                     sprite_tile_w * static_cast<int>(sprite_columns),
                                     sprite_tile_h * static_cast<int>(sprite_rows), o);
    write_file_atomic(sprite_path, as_chars(sprite_bytes), o.overwrite, "sprite sheet");
    write_file_atomic(vtt_path,
                      sprite_vtt(sprite_path.filename().string(), result.thumbnails,
                                 result.video.duration_seconds, sprite_tile_w,
                                 sprite_tile_h, sprite_columns),
                      o.overwrite, "sprite index");
    result.sprite_path = sprite_path;
    result.vtt_path = vtt_path;
    result.timings.write_ms += elapsed_ms(write_start);
  }

  result.timings.total_ms = elapsed_ms(total_start);
  return result;
}

}  // namespace vrthumb
