#include "extractor.hpp"

#include <turbojpeg.h>
#include <webp/decode.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
int failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void expect_error(const auto& action, const std::string& message) {
  try {
    action();
    check(false, message);
  } catch (const vrthumb::Error&) {
  }
}

// Runs "ffmpeg ARGS -y OUTPUT" quietly to generate a fixture.
bool make_fixture(const std::string& args, const fs::path& output) {
  const std::string command =
      "ffmpeg -hide_banner -loglevel error " + args + " -y " + output.string();
  if (std::system(command.c_str()) == 0) return true;
  std::cerr << "FAIL: could not generate test fixture " << output.filename() << '\n';
  return false;
}

struct Image {
  int width = 0;
  int height = 0;
  std::vector<unsigned char> rgb;

  // Mean of every channel over [x0, x1) x [y0, y1).
  double mean(int x0, int x1, int y0, int y1) const {
    double sum = 0;
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x)
        for (int c = 0; c < 3; ++c)
          sum += rgb[(static_cast<std::size_t>(y) * width + x) * 3 + c];
    return sum / ((x1 - x0) * (y1 - y0) * 3);
  }

  // True when the pixel is clearly red (or, with `blue`, clearly blue).
  bool is_red(int x, int y, bool blue = false) const {
    const unsigned char* p = &rgb[(static_cast<std::size_t>(y) * width + x) * 3];
    const int wanted = blue ? p[2] : p[0];
    const int other = blue ? p[0] : p[2];
    return wanted > 200 && other < 60 && p[1] < 60;
  }
  bool is_blue(int x, int y) const { return is_red(x, y, true); }
};

// Decodes a JPEG to RGB; the result's width is 0 if decoding fails.
Image read_jpeg(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  const std::vector<unsigned char> jpeg((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
  Image image;
  tjhandle tj = tjInitDecompress();
  int subsampling = 0, colorspace = 0;
  if (tj && !jpeg.empty() &&
      tjDecompressHeader3(tj, jpeg.data(), jpeg.size(), &image.width, &image.height,
                          &subsampling, &colorspace) == 0) {
    image.rgb.resize(static_cast<std::size_t>(image.width) * image.height * 3);
    if (tjDecompress2(tj, jpeg.data(), jpeg.size(), image.rgb.data(), image.width, 0,
                      image.height, TJPF_RGB, 0) != 0)
      image = {};
  } else {
    image = {};
  }
  if (tj) tjDestroy(tj);
  return image;
}

void expect_error_code(const auto& action, vrthumb::ErrorCode expected,
                       const std::string& message) {
  try {
    action();
    check(false, message);
  } catch (const vrthumb::Error& error) {
    check(error.code() == expected, message + " (unexpected error code)");
  }
}
}  // namespace

int main(int argc, char** argv) {
  // Optional path to the vrthumb CLI, for the JSON output checks.
  const fs::path cli = argc > 1 ? argv[1] : "";
  // mkdtemp gives each run its own directory, so concurrent runs or leftovers
  // from a crashed run cannot trip the existing-output checks.
  std::string root_template =
      (fs::temp_directory_path() / "vrthumb-tests-XXXXXX").string();
  if (!mkdtemp(root_template.data())) {
    std::cerr << "FAIL: could not create test directory\n";
    return 1;
  }
  const fs::path root = root_template;
  const fs::path video = root / "fixture.mp4";
  const fs::path hdr_video = root / "hdr-fixture.mkv";
  const fs::path black_video = root / "black-fixture.mp4";
  // A single keyframe at the final frame (7.967s): reaching it requires
  // draining frames the decoder holds back at end of input.
  const fs::path tail_video = root / "tail-keyframe-fixture.mp4";
  // Switches from 16:9 to 4:3 at 4s, so later sprite tiles are narrower than
  // the cell size fixed by the first tile.
  const fs::path resize_video = root / "resize-fixture.mkv";
  const fs::path hdr_vr_video = root / "hdr-vr-fixture.mkv";
  // MPEG-TS has no seek index, so FFmpeg's seek can land on a non-keyframe
  // packet after the keyframe that precedes the target.
  const fs::path ts_video = root / "fixture.ts";
  // Blue left half, red right half; rotated copies tag it with a display
  // rotation of 90, 180, or 270 degrees counterclockwise.
  const fs::path rotation_source = root / "rotation-source.mp4";
  // Stereo fixtures whose left or top eye is red and whose other eye is blue.
  const fs::path sbs_video = root / "sbs-fixture.mp4";
  const fs::path tb_video = root / "tb-fixture.mp4";
  const fs::path portrait_4k_video = root / "portrait-4k-fixture.mp4";
  const bool fixtures =
      make_fixture("-f lavfi -i testsrc2=size=640x360:rate=30:duration=8 -c:v libx264 "
                   "-g 60 -keyint_min 60 -sc_threshold 0 -pix_fmt yuv420p", video) &&
      make_fixture("-f lavfi -i color=black:size=320x180:rate=30:duration=3 "
                   "-f lavfi -i testsrc2=size=320x180:rate=30:duration=7 "
                   "-filter_complex [0:v][1:v]concat=n=2:v=1:a=0[v] -map [v] "
                   "-c:v libx264 -g 30 -pix_fmt yuv420p", black_video) &&
      make_fixture("-f lavfi -i testsrc2=size=320x180:rate=1:duration=2 "
                   "-vf format=yuv420p10le,setparams=color_primaries=bt2020:"
                   "color_trc=smpte2084:colorspace=bt2020nc -c:v ffv1", hdr_video) &&
      make_fixture("-f lavfi -i testsrc2=size=320x180:rate=30:duration=8 -c:v libx264 "
                   "-g 1000 -force_key_frames 7.95 -sc_threshold 0 -pix_fmt yuv420p",
                   tail_video) &&
      make_fixture("-f lavfi -i testsrc2=size=320x180:rate=30:duration=4 -c:v libx264 "
                   "-g 30 -bf 0 -pix_fmt yuv420p -f mpegts", root / "resize-a.ts") &&
      make_fixture("-f lavfi -i testsrc2=size=320x240:rate=30:duration=4 -c:v libx264 "
                   "-g 30 -bf 0 -pix_fmt yuv420p -output_ts_offset 4 -f mpegts",
                   root / "resize-b.ts") &&
      make_fixture("-i concat:" + (root / "resize-a.ts").string() + "\\|" +
                   (root / "resize-b.ts").string() + " -c copy", resize_video) &&
      make_fixture("-f lavfi -i testsrc2=size=640x320:rate=1:duration=2 "
                   "-vf format=yuv420p10le,setparams=color_primaries=bt2020:"
                   "color_trc=smpte2084:colorspace=bt2020nc -c:v ffv1", hdr_vr_video) &&
      make_fixture("-f lavfi -i testsrc2=size=320x240:rate=30:duration=4 -c:v libx264 "
                   "-g 30 -sc_threshold 0 -pix_fmt yuv420p", ts_video) &&
      make_fixture("-f lavfi -i color=c=red:s=320x180:r=30:d=2,"
                   "drawbox=x=0:y=0:w=160:h=180:color=blue:t=fill "
                   "-c:v libx264 -pix_fmt yuv420p", rotation_source) &&
      make_fixture("-display_rotation 90 -i " + rotation_source.string() + " -c copy",
                   root / "rotation-90.mp4") &&
      make_fixture("-display_rotation 180 -i " + rotation_source.string() + " -c copy",
                   root / "rotation-180.mp4") &&
      make_fixture("-display_rotation 270 -i " + rotation_source.string() + " -c copy",
                   root / "rotation-270.mp4") &&
      make_fixture("-f lavfi -i color=c=red:s=640x320:r=30:d=2,"
                   "drawbox=x=320:y=0:w=320:h=320:color=blue:t=fill "
                   "-c:v libx264 -pix_fmt yuv420p", sbs_video) &&
      make_fixture("-f lavfi -i color=c=red:s=640x640:r=30:d=2,"
                   "drawbox=x=0:y=320:w=640:h=320:color=blue:t=fill "
                   "-c:v libx264 -pix_fmt yuv420p", tb_video) &&
      make_fixture("-f lavfi -i testsrc2=size=2160x3840:rate=5:duration=1 "
                   "-c:v libx264 -preset ultrafast -pix_fmt yuv420p", portrait_4k_video);
  if (!fixtures) {
    fs::remove_all(root);
    return 1;
  }

  try {
    const vrthumb::ExtractOptions defaults;
    check(defaults.count == 10, "default candidate count is ten");
    check(defaults.max_width == 640 && defaults.max_height == 360,
          "default thumbnail size is 640x360");
    check(defaults.input_horizontal_fov == 180,
          "default source field of view is 180 degrees");

    vrthumb::ExtractOptions options;
    options.count = 5;
    options.max_width = 320;
    options.max_height = 180;
    options.backend = vrthumb::DecodeBackend::software;
    const auto result = vrthumb::Extractor{}.extract(video, root / "output", options);
    check(result.thumbnails.size() == 5, "requested thumbnail count is preserved");
    check(result.video.coded_width == 640 && result.video.coded_height == 360,
          "video dimensions are reported");
    check(result.video.duration_seconds > 7.9 && result.video.duration_seconds < 8.1,
          "duration is reported");
    double previous_target = -1;
    for (const auto& thumbnail : result.thumbnails) {
      check(fs::file_size(thumbnail.path) > 100, "JPEG was written");
      check(thumbnail.width == 320 && thumbnail.height == 180, "thumbnail fits requested box");
      check(thumbnail.source_seconds <= thumbnail.target_seconds + 0.001,
            "source keyframe precedes target");
      check(thumbnail.target_seconds > previous_target, "targets are chronological");
      previous_target = thumbnail.target_seconds;
    }
    expect_error_code([&] { vrthumb::Extractor{}.extract(video, root / "output", options); },
                      vrthumb::ErrorCode::io_error, "existing output is rejected");
    options.overwrite = true;
    const auto overwritten = vrthumb::Extractor{}.extract(video, root / "output", options);
    check(overwritten.thumbnails.size() == 5, "overwrite succeeds");

    vrthumb::ExtractOptions hdr_options;
    hdr_options.count = 1;
    hdr_options.backend = vrthumb::DecodeBackend::software;
    const auto hdr = vrthumb::Extractor{}.extract(
        hdr_video, root / "hdr-output", hdr_options);
    check(hdr.thumbnails.size() == 1, "HDR thumbnail is generated");
    check(fs::file_size(hdr.thumbnails.front().path) > 100,
          "tone-mapped HDR JPEG was written");

    vrthumb::ExtractOptions vr_options;
    vr_options.count = 2;
    vr_options.backend = vrthumb::DecodeBackend::software;
    vr_options.layout = vrthumb::VideoLayout::vr180_sbs;
    vr_options.input_horizontal_fov = 190;
    const auto vr = vrthumb::Extractor{}.extract(video, root / "vr-output", vr_options);
    check(vr.thumbnails.size() == 2, "VR candidate count is preserved");
    for (const auto& thumbnail : vr.thumbnails) {
      check(thumbnail.width == 640 && thumbnail.height == 360,
            "VR thumbnail has configured output dimensions");
      check(fs::file_size(thumbnail.path) > 100, "VR JPEG was written");
    }

    vrthumb::ExtractOptions alpha_options = vr_options;
    alpha_options.count = 2;
    alpha_options.deovr_packed_alpha = true;
    const auto alpha = vrthumb::Extractor{}.extract(
        video, root / "alpha-output", alpha_options);
    check(alpha.thumbnails.size() == 2,
          "packed-alpha candidates stay in one native extraction batch");
    for (const auto& thumbnail : alpha.thumbnails) {
      check(thumbnail.width == 640 && thumbnail.height == 360,
            "packed-alpha thumbnail has configured output dimensions");
      check(fs::file_size(thumbnail.path) > 100,
            "packed-alpha composite WebP was written");
      check(thumbnail.path.extension() == ".webp",
            "packed-alpha composite is written as WebP, not JPEG");
      std::ifstream in(thumbnail.path, std::ios::binary);
      const std::vector<unsigned char> bytes(
          (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      WebPBitstreamFeatures features;
      const bool decoded =
          WebPGetFeatures(bytes.data(), bytes.size(), &features) == VP8_STATUS_OK;
      check(decoded && features.has_alpha,
            "packed-alpha composite carries a real alpha channel");
    }

    vrthumb::ExtractOptions hdr_alpha_options = alpha_options;
    hdr_alpha_options.count = 1;
    const auto hdr_alpha = vrthumb::Extractor{}.extract(
        hdr_vr_video, root / "hdr-alpha-output", hdr_alpha_options);
    {
      std::ifstream in(hdr_alpha.thumbnails.front().path, std::ios::binary);
      const std::vector<unsigned char> bytes(
          (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      WebPBitstreamFeatures features;
      const bool decoded =
          WebPGetFeatures(bytes.data(), bytes.size(), &features) == VP8_STATUS_OK;
      check(decoded && features.has_alpha,
            "tone mapping preserves the packed-alpha channel of HDR input");
    }

    vr_options.seek_percentages = {12.5, 87.5};
    const auto sought = vrthumb::Extractor{}.extract(
        video, root / "sought-output", vr_options);
    check(sought.thumbnails.size() == 2, "explicit seek count is preserved");
    check(std::abs(sought.thumbnails[0].target_seconds - 1.0) < 0.05,
          "first explicit seek percentage is used");
    check(std::abs(sought.thumbnails[1].target_seconds - 7.0) < 0.05,
          "second explicit seek percentage is used");

    vrthumb::ExtractOptions request_options = vr_options;
    request_options.seek_percentages.clear();
    request_options.requests = {
        {25.0, {-30.0, 5.0, 80.0}},
        {75.0, {40.0, -10.0, 120.0}},
    };
    const auto requested = vrthumb::Extractor{}.extract(
        video, root / "request-output", request_options);
    check(requested.thumbnails.size() == 2,
          "heterogeneous requests stay in one extraction batch");
    check(std::abs(requested.thumbnails[0].target_seconds - 2.0) < 0.05,
          "first heterogeneous request seek is used");
    check(std::abs(requested.thumbnails[1].target_seconds - 6.0) < 0.05,
          "second heterogeneous request seek is used");

    vrthumb::ExtractOptions exact_options;
    exact_options.max_width = 320;
    exact_options.max_height = 180;
    exact_options.backend = vrthumb::DecodeBackend::software;
    // The fixture is 30fps with a 60-frame GOP, so 2.13s falls mid-GOP,
    // between keyframes at 0s and 2s: a percentage/keyframe seek would land
    // on the 2s keyframe, while an exact seek must decode forward to it.
    exact_options.seek_seconds = {2.13};
    const auto exact = vrthumb::Extractor{}.extract(
        video, root / "exact-output", exact_options);
    check(exact.thumbnails.size() == 1, "explicit seek-seconds count is preserved");
    check(std::abs(exact.thumbnails[0].target_seconds - 2.13) < 0.001,
          "exact seek target is reported verbatim");
    check(exact.thumbnails[0].source_seconds >= 2.13 &&
          exact.thumbnails[0].source_seconds < 2.13 + (1.0 / 30.0) + 0.01,
          "exact seek decodes forward to the nearest frame at or after the target, "
          "not the preceding keyframe");

    vrthumb::ExtractOptions tail_options;
    tail_options.max_width = 160;
    tail_options.max_height = 90;
    tail_options.backend = vrthumb::DecodeBackend::software;
    tail_options.seek_percentages = {100};
    const auto tail = vrthumb::Extractor{}.extract(
        tail_video, root / "tail-output", tail_options);
    check(tail.thumbnails.size() == 1 && tail.thumbnails[0].source_seconds > 7.9,
          "keyframe seek into a final one-frame GOP drains the decoder");

    vrthumb::ExtractOptions tail_exact_options = tail_options;
    tail_exact_options.seek_percentages.clear();
    tail_exact_options.seek_seconds = {7.99};
    const auto tail_exact = vrthumb::Extractor{}.extract(
        tail_video, root / "tail-exact-output", tail_exact_options);
    check(tail_exact.thumbnails.size() == 1 &&
          tail_exact.thumbnails[0].source_seconds > 7.9,
          "exact seek past the last frame falls back to the drained final frame");

    for (std::uint32_t count = 1; count <= 6; ++count) {
      vrthumb::ExtractOptions ts_options;
      ts_options.count = count;
      ts_options.max_width = 160;
      ts_options.max_height = 120;
      ts_options.backend = vrthumb::DecodeBackend::software;
      const auto ts = vrthumb::Extractor{}.extract(
          ts_video, root / ("ts-output-" + std::to_string(count)), ts_options);
      // Keyframes fall on whole seconds (30-frame GOPs at 30fps), including
      // targets such as 1.0 and 2.0 that land exactly on one.
      for (const auto& thumbnail : ts.thumbnails)
        check(std::abs(thumbnail.source_seconds -
                       std::floor(thumbnail.target_seconds + 0.001)) < 0.001,
              "MPEG-TS keyframe seek returns the latest keyframe at or before target " +
              std::to_string(thumbnail.target_seconds) + " (got " +
              std::to_string(thumbnail.source_seconds) + ")");
    }

    vrthumb::ExtractOptions ts_exact_options;
    ts_exact_options.max_width = 160;
    ts_exact_options.max_height = 120;
    ts_exact_options.backend = vrthumb::DecodeBackend::software;
    ts_exact_options.seek_seconds = {0.5, 1.5, 2.73, 3.9};
    const auto ts_exact = vrthumb::Extractor{}.extract(
        ts_video, root / "ts-exact-output", ts_exact_options);
    for (const auto& thumbnail : ts_exact.thumbnails)
      check(thumbnail.source_seconds >= thumbnail.target_seconds - 0.001 &&
            thumbnail.source_seconds < thumbnail.target_seconds + (1.0 / 30.0) + 0.01,
            "MPEG-TS exact seek returns the first frame at or after target " +
            std::to_string(thumbnail.target_seconds) + " (got " +
            std::to_string(thumbnail.source_seconds) + ")");

    vrthumb::ExtractOptions sprite_options;
    sprite_options.count = 7;
    sprite_options.max_width = 160;
    sprite_options.max_height = 90;
    sprite_options.backend = vrthumb::DecodeBackend::software;
    sprite_options.sprite_sheet = true;
    const auto sprite = vrthumb::Extractor{}.extract(video, root / "sprite-output", sprite_options);
    check(sprite.thumbnails.size() == 7, "sprite mode reports all thumbnails");
    check(sprite.sprite_path.has_value(), "sprite path is reported");
    check(sprite.vtt_path.has_value(), "VTT path is reported");
    for (const auto& thumbnail : sprite.thumbnails)
      check(thumbnail.path.empty(), "sprite mode does not write per-thumbnail files");
    check(fs::exists(*sprite.sprite_path), "sprite sheet was written");
    check(fs::file_size(*sprite.sprite_path) > 100, "sprite sheet is non-trivial");
    check(fs::exists(*sprite.vtt_path), "sprite VTT was written");
    {
      std::ifstream vtt(*sprite.vtt_path);
      std::string first_line;
      std::getline(vtt, first_line);
      check(first_line == "WEBVTT", "VTT starts with WEBVTT header");
    }
    expect_error_code(
        [&] { vrthumb::Extractor{}.extract(video, root / "sprite-output", sprite_options); },
        vrthumb::ErrorCode::io_error, "existing sprite output is rejected");

    vrthumb::ExtractOptions sprite_columns_options = sprite_options;
    sprite_columns_options.count = 5;
    sprite_columns_options.sprite_columns = 5;
    const auto sprite_row = vrthumb::Extractor{}.extract(
        video, root / "sprite-row-output", sprite_columns_options);
    check(sprite_row.thumbnails.size() == 5, "sprite column override preserves count");

    vrthumb::ExtractOptions resize_options;
    resize_options.count = 4;
    resize_options.max_width = 160;
    resize_options.max_height = 90;
    resize_options.backend = vrthumb::DecodeBackend::software;
    resize_options.sprite_sheet = true;
    const auto resized = vrthumb::Extractor{}.extract(
        resize_video, root / "resize-output", resize_options);
    {
      // 2x2 grid of 160x90 cells; the last two tiles come from the 4:3 part,
      // so they are 120x90 and centred between 20px black pillars.
      const Image sprite = read_jpeg(*resized.sprite_path);
      check(sprite.width == 320 && sprite.height == 180,
            "resized sprite keeps the first tile's cell size");
      if (sprite.width == 320 && sprite.height == 180) {
        check(sprite.mean(160, 176, 96, 174) < 16,
              "narrower sprite tile leaves a black pillar in its cell");
        check(sprite.mean(200, 280, 96, 174) > 30, "narrower sprite tile is drawn centred");
      }
    }

    vrthumb::ExtractOptions webp_options;
    webp_options.count = 3;
    webp_options.max_width = 160;
    webp_options.max_height = 90;
    webp_options.backend = vrthumb::DecodeBackend::software;
    webp_options.format = vrthumb::OutputFormat::webp;
    const auto webp = vrthumb::Extractor{}.extract(video, root / "webp-output", webp_options);
    check(webp.thumbnails.size() == 3, "WebP thumbnail count is preserved");
    for (const auto& thumbnail : webp.thumbnails) {
      check(thumbnail.path.extension() == ".webp", "WebP thumbnail has .webp extension");
      check(fs::file_size(thumbnail.path) > 20, "WebP thumbnail was written");
    }

    vrthumb::ExtractOptions webp_sprite_options = webp_options;
    webp_sprite_options.sprite_sheet = true;
    const auto webp_sprite = vrthumb::Extractor{}.extract(
        video, root / "webp-sprite-output", webp_sprite_options);
    check(webp_sprite.sprite_path.has_value(), "WebP sprite path is reported");
    check(webp_sprite.sprite_path->extension() == ".webp", "WebP sprite has .webp extension");
    check(fs::file_size(*webp_sprite.sprite_path) > 20, "WebP sprite sheet was written");

    vrthumb::ExtractOptions flat_probe_options;
    flat_probe_options.count = 1;
    flat_probe_options.max_width = 320;
    flat_probe_options.max_height = 180;
    flat_probe_options.backend = vrthumb::DecodeBackend::software;
    flat_probe_options.seek_percentages = {16.6};  // ~0.5s into a 3s black intro
    const auto flat_default = vrthumb::Extractor{}.extract(
        black_video, root / "flat-default-output", flat_probe_options);
    check(flat_default.thumbnails[0].source_seconds < 3.0,
          "without skip_flat_frames, a black target frame is kept as-is");

    vrthumb::ExtractOptions skip_flat_options = flat_probe_options;
    skip_flat_options.skip_flat_frames = true;
    const auto flat_skipped = vrthumb::Extractor{}.extract(
        black_video, root / "flat-skipped-output", skip_flat_options);
    check(flat_skipped.thumbnails[0].source_seconds >= 3.0,
          "skip_flat_frames resamples past a black intro");

    // Retry steps of 0.5s x 4 reach at most 2.5s past the target, still short
    // of the 3s black/colorful boundary, so this never clears the threshold
    // and exercises the retry-cap fallback path.
    vrthumb::ExtractOptions exhausted_options = skip_flat_options;
    exhausted_options.seek_percentages = {0.1};
    const auto exhausted = vrthumb::Extractor{}.extract(
        black_video, root / "flat-exhausted-output", exhausted_options);
    check(exhausted.thumbnails.size() == 1,
          "skip_flat_frames still produces output when nothing clears the threshold");
    // Expected orientations match FFmpeg's own autorotation of the fixture.
    struct RotationCase {
      int degrees;
      int width;
      int height;
    };
    for (const RotationCase& rotation :
         {RotationCase{90, 180, 320}, RotationCase{180, 320, 180},
          RotationCase{270, 180, 320}}) {
      vrthumb::ExtractOptions rotation_options;
      rotation_options.count = 1;
      rotation_options.backend = vrthumb::DecodeBackend::software;
      const std::string name = "rotation-" + std::to_string(rotation.degrees);
      const auto rotated = vrthumb::Extractor{}.extract(
          root / (name + ".mp4"), root / (name + "-output"), rotation_options);
      const Image image = read_jpeg(rotated.thumbnails.front().path);
      const int w = image.width;
      const int h = image.height;
      check(w == rotation.width && h == rotation.height,
            name + " thumbnail has the displayed dimensions");
      if (w != rotation.width || h != rotation.height) continue;
      const bool placed =
          rotation.degrees == 90 ? image.is_red(w / 2, h / 6) && image.is_blue(w / 2, h * 5 / 6)
          : rotation.degrees == 180 ? image.is_red(w / 6, h / 2) && image.is_blue(w * 5 / 6, h / 2)
                                    : image.is_blue(w / 2, h / 6) && image.is_red(w / 2, h * 5 / 6);
      check(placed, name + " thumbnail is turned the way FFmpeg displays it");
    }

    struct LayoutCase {
      const char* name;
      fs::path video;
      vrthumb::VideoLayout layout;
      vrthumb::InputProjection projection;
    };
    for (const LayoutCase& layout :
         {LayoutCase{"sbs-fisheye", sbs_video, vrthumb::VideoLayout::vr180_sbs,
                     vrthumb::InputProjection::fisheye},
          LayoutCase{"sbs-hequirect", sbs_video, vrthumb::VideoLayout::vr180_sbs,
                     vrthumb::InputProjection::hequirect},
          LayoutCase{"tb-fisheye", tb_video, vrthumb::VideoLayout::vr180_tb,
                     vrthumb::InputProjection::fisheye},
          LayoutCase{"tb-hequirect", tb_video, vrthumb::VideoLayout::vr180_tb,
                     vrthumb::InputProjection::hequirect}}) {
      vrthumb::ExtractOptions layout_options;
      layout_options.count = 1;
      layout_options.max_width = 160;
      layout_options.max_height = 90;
      layout_options.backend = vrthumb::DecodeBackend::software;
      layout_options.layout = layout.layout;
      layout_options.input_projection = layout.projection;
      const std::string name = layout.name;
      const auto projected = vrthumb::Extractor{}.extract(
          layout.video, root / (name + "-output"), layout_options);
      const Image image = read_jpeg(projected.thumbnails.front().path);
      check(image.width == 160 && image.height == 90,
            name + " thumbnail has the configured dimensions");
      if (image.width == 160 && image.height == 90)
        check(image.is_red(80, 45), name + " thumbnail shows the left or top eye");
    }

    // FFV1 has no VA-API decoder, so forcing VA-API fails the same way whether
    // or not this machine has a VA-API device.
    vrthumb::ExtractOptions vaapi_options;
    vaapi_options.count = 1;
    vaapi_options.backend = vrthumb::DecodeBackend::vaapi;
    expect_error_code(
        [&] { vrthumb::Extractor{}.extract(hdr_video, root / "vaapi-ffv1-output", vaapi_options); },
        vrthumb::ErrorCode::unsupported_backend,
        "forced VA-API rejects a codec without VA-API support");

    // Portrait 4K is eligible for automatic VA-API. Without a usable device
    // this exercises the software fallback instead; either way it must work.
    vrthumb::ExtractOptions automatic_options;
    automatic_options.count = 2;
    automatic_options.max_width = 160;
    automatic_options.max_height = 90;
    const auto automatic = vrthumb::Extractor{}.extract(
        portrait_4k_video, root / "automatic-4k-output", automatic_options);
    std::cout << "note: automatic decoding of portrait 4K used "
              << vrthumb::backend_name(automatic.backend_used) << '\n';
    check(automatic.backend_used == vrthumb::DecodeBackend::vaapi ||
              automatic.backend_used == vrthumb::DecodeBackend::software,
          "automatic decoding resolves to a concrete backend");
    check(automatic.thumbnails.size() == 2, "automatic 4K decoding produces every thumbnail");
    for (const auto& thumbnail : automatic.thumbnails) {
      const Image image = read_jpeg(thumbnail.path);
      check(image.width == 50 && image.height == 90,
            "automatic 4K thumbnail decodes at its reported size");
    }

    if (!cli.empty()) {
      // A tab in the output directory must come out escaped in the JSON.
      const fs::path cli_output = root / "cli\toutput";
      const fs::path cli_stdout = root / "cli-stdout.txt";
      const std::string command =
          "'" + cli.string() + "' '" + video.string() + "' --output '" +
          cli_output.string() + "' --count 2 --size 64x36 --decoder software "
          "--json-lines > '" + cli_stdout.string() + "'";
      check(std::system(command.c_str()) == 0, "CLI --json-lines run succeeds");
      std::ifstream in(cli_stdout);
      std::vector<std::string> lines;
      for (std::string line; std::getline(in, line);) lines.push_back(line);
      check(lines.size() == 3, "--json-lines prints one line per thumbnail plus a summary");
      if (lines.size() == 3) {
        for (int i = 0; i < 2; ++i)
          check(lines[i].starts_with("{\"type\":\"thumbnail\",\"index\":" +
                                     std::to_string(i + 1) + ","),
                "--json-lines thumbnail records are typed and ordered");
        check(lines[2].starts_with("{\"type\":\"summary\","),
              "--json-lines ends with a typed summary");
        check(lines[0].find("cli\\toutput") != std::string::npos,
              "JSON escapes a tab in the output path");
        for (const auto& line : lines)
          check(line.ends_with('}') && line.find('\t') == std::string::npos,
                "JSON lines are complete and contain no raw control characters");
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
    ++failures;
  }

  vrthumb::ExtractOptions invalid;
  invalid.count = 0;
  expect_error_code([&] { vrthumb::Extractor{}.extract(video, root / "bad", invalid); },
                    vrthumb::ErrorCode::invalid_option, "invalid count is rejected");
  invalid.count = 1;
  invalid.vr_eye_size = 32;
  expect_error_code([&] { vrthumb::Extractor{}.extract(video, root / "bad-eye", invalid); },
                    vrthumb::ErrorCode::invalid_option, "invalid VR eye size is rejected");
  invalid.vr_eye_size = 1080;
  invalid.input_horizontal_fov = 361;
  expect_error_code([&] { vrthumb::Extractor{}.extract(video, root / "bad-input-fov", invalid); },
                    vrthumb::ErrorCode::invalid_option, "invalid input FOV is rejected");
  invalid.input_horizontal_fov = 180;
  invalid.deovr_packed_alpha = true;
  expect_error_code(
      [&] { vrthumb::Extractor{}.extract(video, root / "bad-alpha", invalid); },
      vrthumb::ErrorCode::invalid_option,
      "packed alpha rejects non-VR input");
  invalid.deovr_packed_alpha = false;
  invalid.seek_percentages = {101};
  expect_error_code([&] { vrthumb::Extractor{}.extract(video, root / "bad-seek", invalid); },
                    vrthumb::ErrorCode::invalid_option, "invalid seek percentage is rejected");
  invalid.seek_percentages.clear();
  invalid.seek_seconds = {-1};
  expect_error_code([&] { vrthumb::Extractor{}.extract(video, root / "bad-seek-seconds", invalid); },
                    vrthumb::ErrorCode::invalid_option, "negative seek seconds is rejected");
  invalid.seek_seconds = {1.0};
  invalid.seek_percentages = {50};
  expect_error_code(
      [&] { vrthumb::Extractor{}.extract(video, root / "bad-seek-combo", invalid); },
      vrthumb::ErrorCode::invalid_option,
      "combining seek_percentages and seek_seconds is rejected");
  expect_error_code(
      [&] { vrthumb::Extractor{}.extract(root / "missing.mp4", root / "bad-open", {}); },
      vrthumb::ErrorCode::open_failed, "missing input is rejected as open_failed");
  fs::remove_all(root);
  if (failures == 0) std::cout << "All tests passed\n";
  return failures == 0 ? 0 : 1;
}
