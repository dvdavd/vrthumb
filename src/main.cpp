#include "extractor.hpp"
#include "vrthumb/version.hpp"

#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

namespace {

[[noreturn]] void usage(const char* program, int status) {
  std::cerr << "Usage: " << program
            << " INPUT --output DIR [-o DIR] [--count N] [-n N] [--size WxH]\n"
               "       [--quality N] [--decoder auto|software|vaapi]\n"
               "       [--format jpeg|webp] [--vr180 sbs|tb]\n"
               "       [--input-projection fisheye|hequirect] [--eye-size N]\n"
               "       [--yaw N] [--pitch N] [--input-hfov N] [--hfov N] [--seek-percent N]\n"
               "       [--seek-seconds N]\n"
               "       [--request SEEK_PERCENT,YAW,PITCH,HFOV]...\n"
               "       [--alpha-packing none|deovr]\n"
               "       [--overwrite] [--sprite-sheet] [--sprite-columns N]\n"
               "       [--skip-flat-frames] [--flat-frame-threshold N]\n"
               "       [--json] [--json-lines]\n"
               "       --version\n"
               "       --help, -h\n";
  std::exit(status);
}

vrthumb::Error invalid_option(const std::string& message) {
  return vrthumb::Error(vrthumb::ErrorCode::invalid_option, message);
}

double number(std::string_view value, const char* option) {
  try {
    std::size_t used = 0;
    const double parsed = std::stod(std::string(value), &used);
    if (used != value.size() || !std::isfinite(parsed))
      throw std::invalid_argument("invalid");
    return parsed;
  } catch (...) {
    throw invalid_option(std::string("invalid value for ") + option);
  }
}

std::uint32_t whole_number(std::string_view value, const char* option) {
  // Unlike std::stoul, from_chars rejects signs and whitespace and reports
  // out-of-range input instead of wrapping "-1" to a huge value.
  std::uint32_t parsed = 0;
  const char* end = value.data() + value.size();
  const auto [used, error] = std::from_chars(value.data(), end, parsed);
  if (value.empty() || error != std::errc{} || used != end)
    throw invalid_option(std::string("invalid value for ") + option);
  return parsed;
}

vrthumb::ThumbnailRequest request(std::string_view value) {
  std::string text(value);
  std::array<double, 4> fields{};
  std::size_t start = 0;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const std::size_t comma = text.find(',', start);
    if ((index + 1 < fields.size()) == (comma == std::string::npos))
      throw invalid_option("--request must be SEEK_PERCENT,YAW,PITCH,HFOV");
    fields[index] = number(
        std::string_view(text).substr(start, comma == std::string::npos
                                                ? comma : comma - start),
        "--request");
    start = comma + 1;
  }
  return {fields[0], {fields[1], fields[2], fields[3]}};
}

std::string json_escape(std::string_view text) {
  std::string out;
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          std::array<char, 7> escaped{};
          std::snprintf(escaped.data(), escaped.size(), "\\u%04x",
                        static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += escaped.data();
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Writes a thumbnail's fields without braces, so callers can prepend a type.
void print_thumbnail_fields(const vrthumb::Thumbnail& t) {
  std::cout << "\"index\":" << t.index << ",\"target\":" << t.target_seconds
            << ",\"source\":" << t.source_seconds << ",\"width\":" << t.width
            << ",\"height\":" << t.height << ",\"path\":\""
            << json_escape(t.path.string()) << '"';
}

void print_json(const vrthumb::ExtractionResult& r) {
  std::cout << std::fixed << std::setprecision(6)
            << "{\"type\":\"summary\",\"backend\":\""
            << vrthumb::backend_name(r.backend_used)
            << "\",\"video\":{\"duration\":" << r.video.duration_seconds
            << ",\"width\":" << r.video.coded_width << ",\"height\":"
            << r.video.coded_height << ",\"codec\":\"" << json_escape(r.video.codec)
            << "\"},\"timings_ms\":{\"probe\":" << r.timings.probe_ms
            << ",\"seek_decode\":" << r.timings.seek_decode_ms
            << ",\"scale_encode\":" << r.timings.scale_encode_ms
            << ",\"write\":" << r.timings.write_ms << ",\"total\":"
            << r.timings.total_ms << "},\"thumbnails\":[";
  for (std::size_t i = 0; i < r.thumbnails.size(); ++i) {
    if (i) std::cout << ',';
    std::cout << '{';
    print_thumbnail_fields(r.thumbnails[i]);
    std::cout << '}';
  }
  std::cout << ']';
  if (r.sprite_path) std::cout << ",\"sprite_path\":\"" << json_escape(r.sprite_path->string()) << '"';
  if (r.vtt_path) std::cout << ",\"vtt_path\":\"" << json_escape(r.vtt_path->string()) << '"';
  std::cout << "}\n";
}

void print_thumbnail_json(const vrthumb::Thumbnail& t) {
  std::cout << std::fixed << std::setprecision(6) << "{\"type\":\"thumbnail\",";
  print_thumbnail_fields(t);
  std::cout << "}\n" << std::flush;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) usage(argv[0], 2);
  try {
    std::filesystem::path input;
    std::filesystem::path output;
    vrthumb::ExtractOptions options;
    bool json = false;
    bool json_lines = false;
    for (int i = 1; i < argc; ++i) {
      std::string_view arg = argv[i];
      auto next = [&](const char* name) -> std::string_view {
        if (++i >= argc) throw invalid_option(std::string("missing value for ") + name);
        return argv[i];
      };
      if (arg == "--help" || arg == "-h") usage(argv[0], 0);
      else if (arg == "--version") {
        std::cout << "vrthumb " << vrthumb::version << '\n';
        return 0;
      }
      else if (arg == "--output" || arg == "-o") output = next("--output");
      else if (arg == "--count" || arg == "-n") options.count = whole_number(next("--count"), "--count");
      else if (arg == "--quality") options.quality = static_cast<int>(whole_number(next("--quality"), "--quality"));
      else if (arg == "--size") {
        const std::string value(next("--size"));
        const auto x = value.find('x');
        if (x == std::string::npos) throw invalid_option("size must be WxH");
        options.max_width = whole_number(value.substr(0, x), "--size");
        options.max_height = whole_number(value.substr(x + 1), "--size");
      } else if (arg == "--format") {
        const auto value = next("--format");
        if (value == "jpeg") options.format = vrthumb::OutputFormat::jpeg;
        else if (value == "webp") options.format = vrthumb::OutputFormat::webp;
        else throw invalid_option("--format must be jpeg or webp");
      } else if (arg == "--decoder") {
        const auto value = next("--decoder");
        if (value == "auto") options.backend = vrthumb::DecodeBackend::automatic;
        else if (value == "software") options.backend = vrthumb::DecodeBackend::software;
        else if (value == "vaapi") options.backend = vrthumb::DecodeBackend::vaapi;
        else throw invalid_option("decoder must be auto, software, or vaapi");
      } else if (arg == "--vr180") {
        const auto value = next("--vr180");
        if (value == "sbs") options.layout = vrthumb::VideoLayout::vr180_sbs;
        else if (value == "tb") options.layout = vrthumb::VideoLayout::vr180_tb;
        else throw invalid_option("--vr180 must be sbs or tb");
      } else if (arg == "--input-projection") {
        const auto value = next("--input-projection");
        if (value == "fisheye") options.input_projection = vrthumb::InputProjection::fisheye;
        else if (value == "hequirect") options.input_projection = vrthumb::InputProjection::hequirect;
        else throw invalid_option("--input-projection must be fisheye or hequirect");
      } else if (arg == "--eye-size") {
        options.vr_eye_size = whole_number(next("--eye-size"), "--eye-size");
      } else if (arg == "--yaw") options.view.yaw = number(next("--yaw"), "--yaw");
      else if (arg == "--pitch") options.view.pitch = number(next("--pitch"), "--pitch");
      else if (arg == "--hfov") options.view.horizontal_fov = number(next("--hfov"), "--hfov");
      else if (arg == "--input-hfov")
        options.input_horizontal_fov = number(next("--input-hfov"), "--input-hfov");
      else if (arg == "--alpha-packing") {
        const auto value = next("--alpha-packing");
        if (value == "none") options.deovr_packed_alpha = false;
        else if (value == "deovr") options.deovr_packed_alpha = true;
        else throw invalid_option("--alpha-packing must be none or deovr");
      } else if (arg == "--seek-percent")
        options.seek_percentages.push_back(number(next("--seek-percent"), "--seek-percent"));
      else if (arg == "--seek-seconds")
        options.seek_seconds.push_back(number(next("--seek-seconds"), "--seek-seconds"));
      else if (arg == "--request")
        options.requests.push_back(request(next("--request")));
      else if (arg == "--overwrite") options.overwrite = true;
      else if (arg == "--sprite-sheet") options.sprite_sheet = true;
      else if (arg == "--sprite-columns") options.sprite_columns = whole_number(next("--sprite-columns"), "--sprite-columns");
      else if (arg == "--skip-flat-frames") options.skip_flat_frames = true;
      else if (arg == "--flat-frame-threshold")
        options.flat_frame_stddev_threshold = number(next("--flat-frame-threshold"), "--flat-frame-threshold");
      else if (arg == "--json") json = true;
      else if (arg == "--json-lines") json_lines = true;
      else if (!arg.empty() && arg.front() == '-') throw invalid_option("unknown option: " + std::string(arg));
      else if (input.empty()) input = arg;
      else throw invalid_option("only one input file is supported");
    }
    if (input.empty() || output.empty()) usage(argv[0], 2);
    if (json_lines) options.on_thumbnail = print_thumbnail_json;
    const auto result = vrthumb::Extractor{}.extract(input, output, options);
    if (json || json_lines) print_json(result);
    else std::cerr << "Created " << result.thumbnails.size() << " thumbnails in "
                   << std::fixed << std::setprecision(1) << result.timings.total_ms
                   << " ms using " << vrthumb::backend_name(result.backend_used) << " decoding\n";
    return 0;
  } catch (const vrthumb::Error& e) {
    std::cerr << "vrthumb: [" << vrthumb::error_code_name(e.code()) << "] "
               << e.what() << '\n';
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "vrthumb: " << e.what() << '\n';
    return 1;
  }
}
