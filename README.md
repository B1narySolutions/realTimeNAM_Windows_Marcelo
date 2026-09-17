# NAM Seed3

Neural amp modeling with three A2-Lite amp captures — Fender '65 Twin Reverb,
Vox AC30 Chimey, and Marshall JCM800 (gain 5).

You can use it two ways:

- **On the pedal** — firmware for the Daisy Seed3, built and flashed from macOS.
- **On your computer**, with no hardware at all — play a guitar through an amp
  model in real time, or render a WAV file through one. Works on macOS and
  Windows, using the exact same model code the pedal runs.

---

## macOS

Install [Homebrew](https://brew.sh) and Apple's command line tools, then set up
the repo:

```sh
xcode-select --install
brew install portaudio
git clone https://github.com/made9647/capstone_NAM.git
cd capstone_NAM
make install
```

`make install` is the whole setup: it installs the ARM compiler, `dfu-util` and
`jq`, fetches libDaisy and NAM Core at pinned versions, downloads the three amp
captures to `models/local/`, and converts them into the weight data the build
uses. Re-running it is cheap — it skips anything already there.

Now jump to [Play through your computer](#play-through-your-computer) or
[Flash the pedal](#flash-the-pedal).

---

## Windows

Windows runs the computer-side tools (live mic and WAV rendering). Building
pedal firmware is macOS only.

**1.** Install [MSYS2](https://www.msys2.org/) and open **"MSYS2 MinGW64"** from
the Start Menu — specifically that one, not "MSYS2 MSYS" or "MSYS2 UCRT64". It
is the shell that builds real native Windows programs, which is what lets the
tools reach your sound hardware.

**2.** Install the packages:

```sh
pacman -Syu
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-portaudio mingw-w64-x86_64-python \
  mingw-w64-x86_64-jq make git curl perl
```

(If `pacman -Syu` asks you to close and reopen the terminal partway through, do
so, then run it again to finish.)

**3.** Clone into a real Windows path — `C:\...`, not `\\wsl$\...`:

```sh
git clone https://github.com/made9647/capstone_NAM.git /c/Users/<you>/capstone_NAM
cd /c/Users/<you>/capstone_NAM
```

**4.** Fetch the model code and the amp captures:

```sh
git clone https://github.com/sdatkinson/NeuralAmpModelerCore.git libs/NeuralAmpModelerCore
git -C libs/NeuralAmpModelerCore checkout --detach 2563c0fd4cb1f9ce457d89a761738ea15097e1f3
git -C libs/NeuralAmpModelerCore submodule update --init --recursive
bash scripts/download_models.sh
```

(`make install` does these two steps for you on macOS. It does not support MSYS2
yet, hence doing them by hand here.)

---

## Play through your computer

No pedal needed. Find your audio devices:

```sh
make list-devices
```

Each device has an index. Note the one for your input (mic or interface) and
your output (speakers or headphones). On Windows, use the entries marked
**Windows WASAPI**, and re-run this before every session — indices shift
whenever a device is plugged in or unplugged.

Then start playing through an amp:

```sh
make live-mic IN=0 OUT=1        # your input and output indices
make live-mic IN=0 OUT=1 AMP=2  # AMP: 1=Fender, 2=Vox, 3=Marshall (default 1)
```

**Use headphones.** A mic and speakers on the same machine will feed back
through the amp model. Press Enter to stop. To change amps, stop it and re-run
with a different `AMP`.

While it runs it prints where your latency is coming from, once a second — see
[Reading the stats](#reading-the-stats).

To process a recording instead of playing live:

```sh
make render-wav IN=solo.wav OUT=fender.wav AMP=1
```

The input must be mono, 16-bit, 48 kHz. Convert anything else with `ffmpeg`:

```sh
ffmpeg -i yourfile.wav -ar 48000 -ac 1 -sample_fmt s16 solo.wav
```

### Plugging in a real guitar

A laptop's mic jack expects mic-level signal, not a guitar pickup — plugged in
directly, a guitar sounds thin and weak. Use a **USB audio interface with a Hi-Z
instrument input** (PreSonus AudioBox, Focusrite Scarlett Solo, and so on):

1. Install the interface's own driver software if it has one, and restart.
2. Plug the guitar into **Input 1** — only the first input channel is read.
3. Set that input's **Mic/Line/Inst switch to Inst**, or it will sound weak.
4. Turn **Input 1's gain** up from zero.
5. Re-run `make list-devices` with it plugged in to get its index.

Mismatched sample rates between your input and output devices are handled
automatically — each stream runs at its own device's rate, resampled internally
to the 48 kHz the models need.

---

## Flash the pedal

macOS only. Build the firmware, then put the Seed3 in DFU mode: hold **BOOT**,
press and release **RESET**, then release **BOOT**.

```sh
make            # build firmware with all three models embedded
make upload     # flash it over USB
make monitor    # open the serial monitor
```

It starts on Fender. In the monitor, press a key to switch — no Enter needed:

| Key | Selection |
| --- | --- |
| `0` | Clean bypass |
| `1` | Fender '65 Twin Reverb |
| `2` | Vox AC30 Chimey |
| `3` | Marshall JCM800 (gain 5) |

Audio runs at 48 kHz; the left input is processed and sent to both outputs.
Switching briefly interrupts playback. Exit the monitor with **Ctrl-A**, **K**,
then **Y**. If several serial ports are connected, use
`make monitor PORT=/dev/cu.usbmodem…`.

---

## Reading the stats

Once a second, `live-mic` prints the whole signal path in order, with the delay
each stage is holding:

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
delay. Stages 2, 3 and 5 are ours and invisible to the audio system's own
latency numbers, so the two device figures alone understate what you hear.

- **1 mic in / 6 speaker** — each device's own buffering. On laptop hardware
  these two usually dominate everything else combined, and no code change moves
  them.
- **2 resampler / 3 block fill** — audio held while bridging your input's sample
  rate to 48 kHz and while filling the model's next 48-sample block. Both are
  well under a millisecond.
- **4 model** — what the amp model costs, against the deadline it has to meet.
  Not part of the round trip; it is time pressure, not queued audio. Low single
  digits means the model is nowhere near the bottleneck; climbing toward 100%
  means dropouts are coming.
- **5 fifo** — audio queued between the input and output streams. The swing
  matters more than the size: a steady number, even a biggish one, plays better
  than a small jittery one. Growing jitter means the two devices' clocks are
  drifting apart.
- **glitches** — any number above zero is an audible click or gap.

**The biggest latency win available** is using one physical device for both
input and output — an interface's own headphone out, rather than guitar into an
interface and sound out of laptop speakers. Two devices means two independent
clocks, which forces the FIFO in stage 5 to absorb the drift between them.

---

## Development

| Command | Purpose |
| --- | --- |
| `make` | Build the firmware with all three models embedded. |
| `make install` | Full setup: tools, libraries, amp models, model conversion. |
| `make test` | Compare against upstream NAM and run host audio tests. |
| `make list-devices` | List audio devices with their indices. |
| `make live-mic IN=<in> OUT=<out> [AMP=1]` | Play live through an amp model. |
| `make render-wav IN=<in.wav> OUT=<out.wav> [AMP=1]` | Render a WAV through an amp model. |
| `make upload` | Flash over USB in DFU mode. |
| `make monitor` | Open the USB serial monitor. |
| `make format` | Format C/C++ source in `src/` and `tests/`. |
| `make compiledb` | Generate the clangd compilation database. |
| `make clean` | Remove build outputs, keeping models and dependencies. |
| `make help` | List commands. |

`make format` and `make compiledb` need `brew install clang-format compiledb`.
Host tests need a C++20 compiler.

The amp captures come from Tone3000 and live in Git-ignored `models/local/` —
local use is permitted, redistribution is not.

### Known limitations

- These are amp-only models: no cabinet simulation, and no hardware gain
  calibration.
- `live-mic` cannot switch amps while running — stop it and re-run.
- Windows' default shared audio mode adds roughly 10–20 ms per direction.
  Exclusive mode reports lower numbers but tested worse in practice.
