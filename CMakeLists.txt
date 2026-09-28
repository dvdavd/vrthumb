cmake_minimum_required(VERSION 3.20)
project(vrthumb VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Wpedantic)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat libavcodec libavutil libavfilter libswscale)
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp)

configure_file(cmake/version.hpp.in generated/vrthumb/version.hpp @ONLY)

add_executable(vrthumb src/main.cpp src/extractor.cpp)
target_include_directories(vrthumb PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(vrthumb PRIVATE
  PkgConfig::FFMPEG PkgConfig::TURBOJPEG PkgConfig::WEBP)

include(CTest)
if(BUILD_TESTING)
  add_executable(vrthumb_tests tests/tests.cpp src/extractor.cpp)
  target_include_directories(vrthumb_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  # The tests decode generated JPEG and WebP output to inspect it.
  target_link_libraries(vrthumb_tests PRIVATE
    PkgConfig::FFMPEG PkgConfig::TURBOJPEG PkgConfig::WEBP)
  # The CLI path lets the suite check its JSON output as well.
  add_test(NAME vrthumb_tests COMMAND vrthumb_tests $<TARGET_FILE:vrthumb>)
  add_test(NAME cli_rejects_negative_count
    COMMAND vrthumb input.mp4 --output out --count -1)
  add_test(NAME cli_rejects_out_of_range_size
    COMMAND vrthumb input.mp4 --output out --size 4294967616x360)
  set_tests_properties(cli_rejects_negative_count PROPERTIES
    PASS_REGULAR_EXPRESSION "invalid value for --count")
  set_tests_properties(cli_rejects_out_of_range_size PROPERTIES
    PASS_REGULAR_EXPRESSION "invalid value for --size")
endif()

install(TARGETS vrthumb RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
