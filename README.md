# vrthumb

A command-line tool for quickly extracting thumbnails from conventional
and VR180 videos. It seeks to keyframes instead of decoding the entire video
and can write JPEGs, WebP images, or a sprite sheet with a WebVTT index.

## Requirements

- CMake 3.20 or newer
- A C++20 compiler
- `pkg-config`
- FFmpeg development libraries: `libavformat`, `libavcodec`, `libavutil`,
  `libavfilter`, and `libswscale`
- TurboJPEG and WebP development libraries
- Ninja (optional, but used in the commands below)

Debian or Ubuntu, install the dependencies with:

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev libavfilter-dev \
  libswscale-dev libturbojpeg0-dev libwebp-dev
```

Arch Linux, install them with:

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf ffmpeg \
  libjpeg-turbo libwebp
```

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Install

```sh
cmake --install build
```

This installs the `vrthumb` executable. Set `CMAKE_INSTALL_PREFIX` while
configuring if you want to install somewhere other than the system default.


## Use

The input video and output directory are required:

```sh
./build/vrthumb video.mp4 --output thumbnails
```

This creates 10 JPEG thumbnails, up to 640x360 pixels. 

Common options:

- `--count N` sets the number of thumbnails.
- `--size WxH` sets the maximum output dimensions while preserving aspect ratio.
- `--format jpeg|webp` selects the image format.
- `--quality N` sets image quality from 1 to 100.
- `--seek-seconds N` extracts a frame at a specific timestamp; it can be repeated.
- `--sprite-sheet` writes one tiled image and a `sprite.vtt` index.
- `--overwrite` allows existing output files to be replaced.
- `--json` prints an extraction summary as JSON.

Use `--help` for every option, including VA-API decoding and VR180 projection support.

## VR180 video

Use `--vr180` to extract flat views from VR180 video. The source may use
side-by-side (`sbs`) or top/bottom (`tb`) stereo layout:

```sh
./build/vrthumb vr-video.mp4 --output thumbnails \
  --vr180 sbs --input-projection fisheye --count 10
```

To choose the direction and field of view of the extracted images:

```sh
./build/vrthumb vr-video.mp4 --output thumbnails \
  --vr180 tb --input-projection hequirect \
  --yaw 30 --pitch -10 --hfov 90 --format webp
```

Useful VR options:

- `--vr180 sbs|tb` selects the stereo layout and enables VR180 processing.
- `--input-projection fisheye|hequirect` selects the source projection.
- `--yaw N` and `--pitch N` set the view direction in degrees.
- `--hfov N` sets the output horizontal field of view in degrees.
- `--input-hfov N` sets the source projection's field of view (default: 180).
- `--eye-size N` sets the intermediate square size for each eye (default: 1080).
- `--request PERCENT,YAW,PITCH,HFOV` may be repeated to extract different views
  and timestamps in one run.
- `--alpha-packing deovr` reconstructs DeoVR packed alpha from side-by-side
  fisheye input and writes transparent WebP images.
