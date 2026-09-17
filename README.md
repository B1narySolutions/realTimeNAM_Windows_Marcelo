# NAM Seed3

Neural amp modeling on the Daisy Seed3 using libDaisy, with three A2-Lite models:
Fender '65 Twin Reverb, Vox AC30 Chimey, and Marshall JCM800 (gain 5).

## Setup

On macOS, install [Homebrew](https://brew.sh) and Apple's Command Line Tools
(`xcode-select --install`). On Ubuntu, the installer uses `apt-get` and requests
`sudo` access when needed. Run from the repository root:

```sh
make install
make
```

`make install` is the whole setup. It installs the ARM compiler, DFU uploader,
and `jq`, fetches pinned versions of libDaisy and NAM Core, downloads the three
Tone3000 models to Git-ignored `models/local/`, and converts them to the
embedded weight data the build links in. Already-downloaded models are left
alone, so re-running it is cheap. On Ubuntu, the installer also installs the
build and download prerequisites, including the ARM C/C++ libraries and
Python 3. The underlying steps are still available on their own:
`bash scripts/install.sh`, `bash scripts/download_models.sh`, `make model`.

## Upload and play

Hold **BOOT**, press and release **RESET**, then release **BOOT** to enter DFU mode.

```sh
make upload
make monitor
```

The firmware starts with Fender selected. Press a key in the serial monitor
to switch models or bypass; no Enter is needed:

| Key | Selection |
| --- | --- |
| `0` | Clean bypass |
| `1` | Fender '65 Twin Reverb |
| `2` | Vox AC30 Chimey |
| `3` | Marshall JCM800 (gain 5) |

Audio runs at 48 kHz, with the left input processed and sent to both outputs.
Switching models or toggling bypass briefly interrupts playback. These are
amp-only models; cabinet filtering and hardware gain calibration are not implemented.

If multiple serial ports are connected, use `make monitor PORT=/dev/cu.usbmodem…`.
Exit the monitor with **Ctrl-A**, then **K**, then **Y**.

## Development

| Command | Purpose |
| --- | --- |
| `make` | Build firmware and embed all three models. |
| `make test` | Compare against upstream NAM and run sanitized host audio tests. |
| `make clean` | Remove build outputs; keep downloaded models and dependencies. |
| `make format` | Format C/C++ source in `src/` and `tests/`. |
| `make compiledb` | Generate the clangd compilation database. |
| `make list-devices` | Build and run `list_devices` to look up audio device indices. |
| `make live-mic IN=<in> OUT=<out> [AMP=1]` | Build and run live mic mode in one command. |
| `make help` | List commands. |

Formatting and clangd setup require `brew install clang-format compiledb`.
Host tests require a C++20 compiler.


## Prerequisites

### Everyone (works on any Linux, including WSL)
- `git`
- `python3`
- `jq`
- A C++ compiler — `sudo apt install build-essential` on Ubuntu/WSL. Host
  tests specifically need C++20 support (GCC 10+; modern Ubuntu ships 12+).
- On Ubuntu/WSL, also install `libdigest-sha-perl` — it provides the
  `shasum` command that `scripts/download_models.sh` uses (Ubuntu ships
  `sha256sum` by default, not `shasum`).

```sh
sudo apt update
sudo apt install -y build-essential git jq python3 libdigest-sha-perl
```

### Only if building/flashing real firmware
- ARM cross-compiler (`arm-none-eabi-g++`). On macOS, `make install` gets
  this via Homebrew automatically. On Ubuntu/WSL, Ubuntu's packaged version
  is often outdated or missing, so download it directly from
  [Arm's GNU toolchain page](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  and add its `bin/` folder to your PATH.
- `dfu-util` — `sudo apt install dfu-util` on Ubuntu/WSL.
- If flashing from WSL specifically, you'll also need
  [usbipd-win](https://github.com/dorssel/usbipd-win) installed on the
  Windows side to pass the USB device through.

### Only if testing offline with `tools/render_wav.cpp` (no hardware needed)
- `ffmpeg` — `sudo apt install ffmpeg` — used to convert guitar recordings
  to the mono/16-bit/48kHz WAV format the tool expects.

### Fetched automatically, not manually downloaded
- **libDaisy** and **NeuralAmpModelerCore** — cloned at pinned commits by
  `scripts/install.sh` on macOS. On Ubuntu/WSL, clone them manually (see
  below) until the install script supports apt.
- **The three amp model captures** — fetched from Tone3000 by
  `scripts/download_models.sh`. Not stored in the repo (see `.gitignore`);
  Tone3000's license permits local use only, not redistribution.

### Manual dependency setup on Ubuntu/WSL (until install.sh supports apt)
```sh
mkdir -p libs
git clone --no-checkout https://github.com/electro-smith/libDaisy.git libs/libDaisy
git -C libs/libDaisy checkout --detach cc146d5065dd8286078a662e2830bf820c37a612
git -C libs/libDaisy submodule update --init --recursive

git clone --no-checkout https://github.com/sdatkinson/NeuralAmpModelerCore.git libs/NeuralAmpModelerCore
git -C libs/NeuralAmpModelerCore checkout --detach 2563c0fd4cb1f9ce457d89a761738ea15097e1f3
git -C libs/NeuralAmpModelerCore submodule update --init --recursive
```

## Testing tools (no hardware required)

`tools/` has small standalone programs for testing the amp models on a
computer, without needing the actual pedal. They all reuse the exact same
`NamAudio`/`NamProcessor`/`A2Lite` code that runs on the real firmware, so
what you hear from them is what the pedal will actually sound like.

| Tool | What it does | Where it runs |
| --- | --- | --- |
| `render_wav.cpp` | Reads a WAV file, runs it through one amp model, writes a new WAV file | Anywhere — Linux, WSL, or native Windows |
| `list_devices.cpp` | Lists every audio device your computer can see, with an index for each | Native Windows (MSYS2) only |
| `live_mic.cpp` | Streams live audio from a chosen input device, through an amp model, out to a chosen output device, in real time | Native Windows (MSYS2) only |

### `render_wav.cpp`

Uses the prerequisites already listed above (WSL/Linux included). Build
from the repo root, after the usual `download_models.sh` +
`convert_a2.py` steps:

```sh
g++ -std=c++17 -O2 -Ibuild/generated -Isrc \
  -Ilibs/NeuralAmpModelerCore -Ilibs/NeuralAmpModelerCore/NAM \
  -Ilibs/NeuralAmpModelerCore/Dependencies/eigen \
  -Ilibs/NeuralAmpModelerCore/Dependencies/nlohmann \
  -DNAM_SAMPLE_FLOAT -DNAM_USE_INLINE_GEMM \
  tools/render_wav.cpp src/audio/nam_audio.cpp src/models/amp_models.cpp \
  src/models/a2_lite.cpp src/audio/nam_processor.cpp \
  libs/NeuralAmpModelerCore/NAM/dsp.cpp -o render_wav
```

Usage:

```sh
ffmpeg -i yourfile.wav -ar 48000 -ac 1 -sample_fmt s16 solo_48k_mono.wav
./render_wav solo_48k_mono.wav fender_out.wav 1   # 1=Fender 2=Vox 3=Marshall
```

### `list_devices.cpp` and `live_mic.cpp` (native Windows only)

These need real, live access to a microphone and speakers, which WSL
cannot provide — it runs in a virtualized environment with no direct
hardware audio access (`list_devices` finds 0 devices under WSL). They
need a genuinely **native Windows build**, via **MSYS2**, so Windows can
hand over real audio device access the way it would to any other Windows
program.

**1. Install MSYS2** from [msys2.org](https://www.msys2.org/) (default
install location, `C:\msys64`, is fine).

**2. Open "MSYS2 MinGW64"** specifically from the Start Menu — not "MSYS2
MSYS" and not "MSYS2 UCRT64". This is the shell that produces real native
Windows `.exe` programs with full Windows API access, required for audio
device access to work.

**3. Update and install packages:**

```sh
pacman -Syu
```

(If it asks you to close and reopen the terminal partway through, do so,
then re-run `pacman -Syu` to finish.)

```sh
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-portaudio git mingw-w64-x86_64-python mingw-w64-x86_64-jq perl
```

`mingw-w64-x86_64-portaudio` is the key addition here — PortAudio is the
cross-platform library that actually talks to the mic/speakers.

**4. Clone the repo into a native Windows path** (a real `C:\...` path,
not `\\wsl$\...`, so the build is genuinely native):

```sh
git clone <this repo's URL> /c/Users/<you>/projects/<folder-name>
cd /c/Users/<you>/projects/<folder-name>
```

**5. Repeat the NAM Core clone, model download, and header generation
steps** from the sections above, run again here — this native Windows
checkout needs its own copies, separate from any WSL/Linux one.

**6. Build `list_devices` and find your device indices:**

```sh
g++ -std=c++17 tools/list_devices.cpp -lportaudio -o list_devices.exe
./list_devices.exe
```

This lists every device, tagged with which Windows audio API exposes it
(MME, DirectSound, WASAPI, WDM-KS). Each physical device appears once per
API — **use the `Windows WASAPI` entries**. Note the index numbers for
your input device (mic/headset/interface) and output device
(speakers/headphones). **Re-run this right before every use** — indices
reliably shift whenever a device is plugged, unplugged, or Windows
re-enumerates it, even without you changing anything.

**7. Build and run `live_mic` in one command:**

```sh
make live-mic IN=15 OUT=13 AMP=1
```

`IN`/`OUT` are the device indices from step 6, `AMP` is `1`=Fender,
`2`=Vox, `3`=Marshall (defaults to `1`). The target builds the binary to
`build/tools/live_mic` with the host compiler (`$(CXX)`), generating the
embedded model header first, and then runs it. Step 6's lister has a
target too: `make list-devices`. PortAudio's compiler flags come from
`pkg-config`, falling back to the Homebrew prefix and then to plain
`-lportaudio`; override them with `PA_CFLAGS=` / `PA_LIBS=` if your
install lives elsewhere. Skip to step 8 for how to read the arguments.

The equivalent manual build:

```sh
g++ -std=c++17 -O2 -Ibuild/generated -Isrc \
  -Ilibs/NeuralAmpModelerCore -Ilibs/NeuralAmpModelerCore/NAM \
  -Ilibs/NeuralAmpModelerCore/Dependencies/eigen \
  -Ilibs/NeuralAmpModelerCore/Dependencies/nlohmann \
  -DNAM_SAMPLE_FLOAT -DNAM_USE_INLINE_GEMM \
  tools/live_mic.cpp src/audio/nam_audio.cpp src/models/amp_models.cpp \
  src/models/a2_lite.cpp src/audio/nam_processor.cpp \
  libs/NeuralAmpModelerCore/NAM/dsp.cpp -lportaudio -o live_mic.exe
```

**8. Run it:**

```sh
./live_mic.exe 1 15 13
```

Arguments: `amp_id input_device_index output_device_index`
(`amp_id`: `1`=Fender, `2`=Vox, `3`=Marshall). Replace `15`/`13` with your
own device indices from step 6.

Windows may prompt for microphone permission the first time — allow it.
Press **Enter** in the terminal to stop. To change amps, stop it and
re-run with a different `amp_id` (switching live isn't supported yet).
Every second while running, it prints a breakdown of where the latency
is coming from — see [Reading the stats output](#reading-the-stats-output)
below.

### Plugging in a real guitar

A laptop's mic/headphone jack is built for mic-level signals, not a
guitar pickup's higher-impedance, lower-level output — plugging a guitar
straight in will sound thin and weak. For a real guitar signal, use a
**USB audio interface with a Hi-Z instrument input** (e.g. a PreSonus
AudioBox, Focusrite Scarlett Solo, etc.) — it has the buffering/impedance
matching a guitar pickup actually needs built into the hardware.

1. **Install the interface's own driver software** if it has one (e.g.
   PreSonus **Universal Control**) — don't rely on Windows' generic
   driver, which can misreport or restrict the device's real
   capabilities. Restart after installing.
2. Plug your guitar into **Input 1** specifically — `live_mic.cpp`
   currently only reads the first input channel. Flip that input's
   **Mic/Line/Inst switch to Inst** — this is what activates the
   high-impedance buffering; leaving it on Mic will sound weak or wrong.
3. Turn up **Input 1's gain knob** — at zero you'll get silence
   regardless of anything else being right.
4. Run `list_devices.exe` with the interface plugged in and look for its
   name (e.g. `Line (... AudioBox USB)`) under the `Windows WASAPI`
   section — that's your input index.

**Mismatched sample rates between devices:** some interfaces (particularly
older/basic ones, like the original PreSonus AudioBox USB) are hard-locked
to 44100 Hz and cannot be switched to 48000 Hz, even though the NAM model
requires 48000 Hz internally, and your speakers/headphones might be
separately locked to 48000 Hz with no 44100 Hz option. `live_mic.cpp`
handles this automatically: it opens the input and output as two
independent streams, each at its own device's native rate (read
automatically via `defaultSampleRate` — you don't need to configure
this), with a small internal resampler bridging the input side up to
48000 Hz before the amp model, and a thread-safe FIFO buffer connecting
the two streams. No code changes are needed for this case — it just
works with whatever rate each device reports.

### Reading the stats output

Every second, `live_mic` prints the whole signal path, in order, with the
delay each stage is holding:

```
[stats 12s] ROUND TRIP 64.1 ms mic to speaker
  1 mic in     device buffer 32.8 ms  (63 frames per callback, cpu 1.7%)
  2 resampler  0.0 ms held  (1 samples at 48000 Hz)
  3 block fill 0.7 ms held  (32 of 48 samples toward the next model block)
  4 model      0.018 ms per block avg, 0.061 ms worst callback  (4.6% of the 1.3 ms deadline, 1006 blocks/s)
  5 fifo       12.7 ms now, swung 10.7 to 12.7 ms  (608 samples, jitter 2.0 ms)
  6 speaker    device buffer 18.0 ms  (64 frames per callback, cpu 0.1%)
  glitches     underflow in=0 out=0 | overflow in=0 out=0 | silence filled 0 samples this second
```

**ROUND TRIP** is stages 1, 2, 3, 5 and 6 added up — the honest end-to-end
figure. Stages 2, 3 and 5 are ours and are invisible to PortAudio's own
latency numbers, so the two device figures alone *understate* what you
hear. Stage 4 is not added in: the model runs inside the input callback,
so its cost shows up as pressure on that callback's deadline rather than
as extra queued audio.

Read it top to bottom to see where the delay actually lives:

- **1 mic in / 6 speaker** — each device's own buffering, as PortAudio
  reports it. On typical laptop hardware these two dominate everything
  else combined, and no amount of code tuning moves them; only a
  different device or a lower-latency driver mode will.
- **2 resampler** — input-rate samples held to bridge the input device's
  rate up to the model's 48 kHz. Near zero when the input already runs
  at 48 kHz; it never holds more than a sample or two.
- **3 block fill** — samples waiting to complete the model's next
  48-sample (1 ms) block. Bounded by one block, so this is always under
  1 ms.
- **4 model** — what the amp model costs, per block and for the worst
  single callback in the last second, against the deadline that callback
  had to meet. The percentage is the one to watch: low means the model
  is nowhere near being the problem, and climbing toward 100% means
  dropouts are imminent. On an M-series Mac this sits in the low single
  digits.
- **5 fifo** — audio queued between the two independent streams. The
  swing matters more than the size: a **stable** number, even a
  moderately large one, plays better than a **small but jittery** one,
  because unpredictable delay is harder to play against than a bigger
  steady one. A jitter figure that keeps growing means the two devices'
  clocks are drifting apart faster than the FIFO can absorb.
- **glitches** — underflow/overflow are PortAudio's own counts;
  "silence filled" is how many samples the output stream wanted that the
  FIFO could not supply, and went out as silence instead. Any of these
  above 0 is an audible click or gap, whether or not you noticed it.
  They are cumulative except "silence filled", which covers the last
  second only, as do the model and FIFO swing figures.

### Known limitations

- **Latency is dominated by using two separate physical devices for
  input and output** (e.g. a USB interface for guitar in, laptop
  speakers for output). Each device has its own independent clock, so a
  software FIFO has to absorb the drift between them — this is a real,
  measured source of both delay and jitter (tens of ms round trip is
  typical), not just a tuning issue. **The single biggest latency win
  available is routing both input and output through the same physical
  device** (e.g. an interface's own headphone output) — this lets the
  code use one unified duplex stream instead of two independent ones
  bridged by a FIFO, removing the cross-device drift entirely.
- Windows' shared audio mode (the default) has a further ~10-20ms floor
  per direction on top of that. WASAPI exclusive mode can reduce the
  *reported* number, but in testing on at least one device it produced
  worse, jitterier real-world results than shared mode despite the lower
  number — trust your ears and the stats' `fifo=` stability over the
  raw device-latency figures alone.
- **Signal quality depends on your input.** A built-in laptop mic or a
  headset mic picking up sound acoustically will sound noticeably worse
  than a real instrument-level input through a proper Hi-Z interface
  input, due to background noise and the mic's own frequency response.
- **These are amp-only models.** No cabinet simulation is applied.
- **No live amp-switching** in `live_mic` yet — stop and restart with a
  different `amp_id` to change amps.