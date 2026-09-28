#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace vrthumb {

enum class DecodeBackend { automatic, software, vaapi };
enum class VideoLayout { flat, vr180_sbs, vr180_tb };
enum class InputProjection { fisheye, hequirect };
enum class OutputFormat { jpeg, webp };

struct Thumbnail;

// Direction and width, in degrees, of the flat view projected from VR input.
struct View {
  double yaw = 0;
  double pitch = 0;
  double horizontal_fov = 100;
};

struct ThumbnailRequest {
  double seek_percentage = 0;
  View view;
};

struct ExtractOptions {
  std::uint32_t count = 10;
  std::uint32_t max_width = 640;
  std::uint32_t max_height = 360;
  std::uint32_t vr_eye_size = 1080;
  // Encoder quality (1-100) for both JPEG and WebP output.
  int quality = 85;
  OutputFormat format = OutputFormat::jpeg;
  DecodeBackend backend = DecodeBackend::automatic;
  VideoLayout layout = VideoLayout::flat;
  InputProjection input_projection = InputProjection::fisheye;
  View view;
  double input_horizontal_fov = 180;
  bool deovr_packed_alpha = false;
  std::vector<double> seek_percentages;
  // Absolute timestamps (seconds) for exact-frame extraction. Mutually
  // exclusive with seek_percentages. Unlike a percentage seek, which accepts
  // the preceding keyframe, each of these decodes forward from that keyframe
  // to the first frame at or after the requested timestamp.
  std::vector<double> seek_seconds;
  // Heterogeneous projected views. When present, these replace count and the
  // homogeneous seek lists while retaining every other batch-wide option.
  std::vector<ThumbnailRequest> requests;
  bool overwrite = false;
  bool sprite_sheet = false;
  std::uint32_t sprite_columns = 0;
  // When set, a candidate frame whose luma standard deviation falls below
  // flat_frame_stddev_threshold (suggesting a black, blank, or otherwise
  // flat frame such as a fade or title card) is resampled a few seconds
  // forward before being accepted as a thumbnail.
  bool skip_flat_frames = false;
  double flat_frame_stddev_threshold = 8.0;
  std::function<void(const Thumbnail&)> on_thumbnail;
};

struct VideoInfo {
  double duration_seconds = 0;
  int coded_width = 0;
  int coded_height = 0;
  std::string codec;
};

struct Thumbnail {
  std::uint32_t index = 0;
  double target_seconds = 0;
  double source_seconds = 0;
  int width = 0;
  int height = 0;
  // Empty when ExtractOptions::sprite_sheet is set, since no per-thumbnail
  // file is written; see ExtractionResult::sprite_path instead.
  std::filesystem::path path;
};

struct Timings {
  double probe_ms = 0;
  double seek_decode_ms = 0;
  double scale_encode_ms = 0;
  double write_ms = 0;
  double total_ms = 0;
};

struct ExtractionResult {
  VideoInfo video;
  DecodeBackend backend_used = DecodeBackend::software;
  std::vector<Thumbnail> thumbnails;
  Timings timings;
  std::optional<std::filesystem::path> sprite_path;
  std::optional<std::filesystem::path> vtt_path;
};

enum class ErrorCode {
  invalid_option,
  open_failed,
  unsupported_backend,
  decode_failed,
  encode_failed,
  io_error,
};

class Error : public std::runtime_error {
 public:
  Error(ErrorCode code, const std::string& message)
      : std::runtime_error(message), code_(code) {}

  ErrorCode code() const noexcept { return code_; }

 private:
  ErrorCode code_;
};

const char* error_code_name(ErrorCode code) noexcept;

class Extractor {
 public:
  ExtractionResult extract(const std::filesystem::path& input,
                           const std::filesystem::path& output_directory,
                           const ExtractOptions& options = {}) const;
};

const char* backend_name(DecodeBackend backend) noexcept;
const char* format_extension(OutputFormat format) noexcept;

}  // namespace vrthumb
