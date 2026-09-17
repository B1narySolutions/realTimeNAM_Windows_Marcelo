// Live real-time instrument/mic-in, amp-processed-out, using PortAudio.
// Runs on NATIVE Windows (via MSYS2 MinGW64) — NOT WSL.
//
// Runs two INDEPENDENT PortAudio streams (input and output), each at its
// own device's native sample rate, bridged by a thread-safe FIFO — needed
// when the input and output devices are locked to different fixed rates
// (e.g. an original PreSonus AudioBox USB at 44100 Hz feeding speakers
// that only offer 48000 Hz). The NAM amp model runs internally at
// 48000 Hz, so the input side is resampled up to that; if the output
// device's native rate also happens to be 48000, no downsampling is
// needed on that side at all.
//
// Every second, prints an ESTIMATED ROUND TRIP latency: the sum of each
// stream's reported device-level latency (from PortAudio) PLUS the extra
// delay sitting in our own internal FIFO bridging the two streams — that
// FIFO delay is real, audible latency that PortAudio's own per-stream
// numbers do NOT include by themselves. Also prints each stream's CPU
// load and running counts of underflows/overflows (buffer starvation or
// overrun events — even a few usually means an audible click happened).
//
// NOTE: an earlier version of this file tried WASAPI exclusive mode plus
// a small fixed 64-frame buffer to chase a lower reported number, but it
// sounded WORSE in practice (likely fighting this device's driver in a
// way that added real jitter the numbers didn't capture) — reverted back
// to plain shared-mode streams with PortAudio's own default buffer
// sizing, which is what's in this file.
//
// Build (from an MSYS2 MinGW64 shell, from the repo root):
//   g++ -std=c++17 -O2 -Ibuild/generated -Isrc \
//     -Ilibs/NeuralAmpModelerCore -Ilibs/NeuralAmpModelerCore/NAM \
//     -Ilibs/NeuralAmpModelerCore/Dependencies/eigen \
//     -Ilibs/NeuralAmpModelerCore/Dependencies/nlohmann \
//     -DNAM_SAMPLE_FLOAT -DNAM_USE_INLINE_GEMM \
//     tools/live_mic.cpp src/audio/nam_audio.cpp src/models/amp_models.cpp \
//     src/models/a2_lite.cpp src/audio/nam_processor.cpp \
//     libs/NeuralAmpModelerCore/NAM/dsp.cpp -lportaudio -o live_mic.exe
//
// Run: ./live_mic.exe amp_id input_device_index output_device_index
//   e.g. ./live_mic.exe 1 15 13
// Press Enter to stop.

#include "audio/nam_audio.h"
#include "models/amp_models.h"
#include <portaudio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {

// Thread-safe FIFO of mono float samples, bridging the input stream's
// callback thread and the output stream's callback thread.
class SharedFifo {
public:
    void Push(const float *samples, std::size_t count) {
        std::lock_guard<std::mutex> lock(mu_);
        buf_.insert(buf_.end(), samples, samples + count);
    }

    std::size_t Pop(float *out, std::size_t max_count) {
        std::lock_guard<std::mutex> lock(mu_);
        const std::size_t n = std::min(max_count, buf_.size());
        std::copy(buf_.begin(), buf_.begin() + static_cast<long>(n), out);
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(n));
        return n;
    }

    std::size_t Size() {
        std::lock_guard<std::mutex> lock(mu_);
        return buf_.size();
    }

private:
    std::mutex mu_;
    std::vector<float> buf_;
};

// Simple streaming linear-interpolation resampler.
class LinearResampler {
public:
    LinearResampler(double in_rate, double out_rate) : ratio_(in_rate / out_rate) {}

    void Push(const float *samples, std::size_t count) {
        buffer_.insert(buffer_.end(), samples, samples + count);
    }

    std::size_t Pop(float *out, std::size_t max_count) {
        std::size_t written = 0;
        while (written < max_count) {
            const std::size_t idx = static_cast<std::size_t>(pos_);
            if (idx + 1 >= buffer_.size())
                break;
            const double frac = pos_ - static_cast<double>(idx);
            out[written] = static_cast<float>(buffer_[idx] * (1.0 - frac) + buffer_[idx + 1] * frac);
            pos_ += ratio_;
            ++written;
        }
        std::size_t keep_from = static_cast<std::size_t>(pos_);
        if (keep_from > buffer_.size())
            keep_from = buffer_.size();
        if (keep_from > 0) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(keep_from));
            pos_ -= static_cast<double>(keep_from);
        }
        return written;
    }

    // Input-rate samples still held, i.e. latency sitting in the resampler.
    std::size_t Pending() const {
        const std::size_t consumed = static_cast<std::size_t>(pos_);
        return buffer_.size() > consumed ? buffer_.size() - consumed : 0;
    }

private:
    double ratio_;
    double pos_ = 0.0;
    std::vector<float> buffer_;
};

// Lock-free min/max for the audio callbacks; contention is nil in practice.
void AtomicMax(std::atomic<std::uint64_t> &target, std::uint64_t value) {
    std::uint64_t prev = target.load(std::memory_order_relaxed);
    while (value > prev && !target.compare_exchange_weak(prev, value, std::memory_order_relaxed)) {
    }
}

void AtomicMin(std::atomic<std::uint64_t> &target, std::uint64_t value) {
    std::uint64_t prev = target.load(std::memory_order_relaxed);
    while (value < prev && !target.compare_exchange_weak(prev, value, std::memory_order_relaxed)) {
    }
}

constexpr std::uint64_t kNoMin = std::numeric_limits<std::uint64_t>::max();

// Counters updated from the (real-time) audio callbacks. Only cheap atomic
// stores, increments and one clock read per callback happen on the audio
// thread; all printing happens elsewhere. Fields marked "per interval" are
// reset by the printer, so each line describes the second it covers rather
// than the whole run.
struct Stats {
    std::atomic<std::uint64_t> input_underflows{0};
    std::atomic<std::uint64_t> input_overflows{0};
    std::atomic<std::uint64_t> output_underflows{0};
    std::atomic<std::uint64_t> output_overflows{0};

    // Where audio is queued up right now, in samples. Every one of these is
    // latency between the mic and the speaker, and none of it is included in
    // PortAudio's own per-stream latency numbers.
    std::atomic<std::uint64_t> resampler_pending{0}; // input-rate samples awaiting resampling
    std::atomic<std::uint64_t> nam_pending{0};       // 48k samples awaiting a full model block

    // Buffer sizes PortAudio actually chose, in frames.
    std::atomic<std::uint64_t> input_frames{0};
    std::atomic<std::uint64_t> output_frames{0};

    // Per interval: how far the bridging FIFO swung, seen by the output side.
    std::atomic<std::uint64_t> fifo_min{kNoMin};
    std::atomic<std::uint64_t> fifo_max{0};

    // Per interval: what the model cost and what we failed to deliver.
    std::atomic<std::uint64_t> dsp_ns_total{0};
    std::atomic<std::uint64_t> dsp_ns_worst_callback{0};
    std::atomic<std::uint64_t> dsp_blocks{0};
    std::atomic<std::uint64_t> starved_samples{0};
};

struct InputCallbackData {
    NamAudio *audio;
    LinearResampler to_48k;
    std::vector<float> nam_in_fifo;
    SharedFifo *output_fifo;
    Stats *stats;
    int channels; // interleaved channels the input stream was opened with
};

struct OutputCallbackData {
    SharedFifo *output_fifo;
    Stats *stats;
    int channels; // interleaved channels the output stream was opened with
};

int InputCallback(const void *input_buffer, void *, unsigned long frame_count,
                   const PaStreamCallbackTimeInfo *, PaStreamCallbackFlags status_flags, void *user_data) {
    auto *data = static_cast<InputCallbackData *>(user_data);
    if (status_flags & paInputUnderflow)
        data->stats->input_underflows.fetch_add(1, std::memory_order_relaxed);
    if (status_flags & paInputOverflow)
        data->stats->input_overflows.fetch_add(1, std::memory_order_relaxed);

    const auto *in = static_cast<const float *>(input_buffer);
    const std::size_t n = frame_count;
    if (!in)
        return paContinue;

    std::vector<float> mono_in(n);
    const std::size_t in_channels = static_cast<std::size_t>(data->channels);
    for (std::size_t i = 0; i < n; ++i)
        mono_in[i] = in[i * in_channels]; // first channel of the interleaved input

    data->to_48k.Push(mono_in.data(), n);
    std::vector<float> resampled(n * 2 + 8);
    const std::size_t got = data->to_48k.Pop(resampled.data(), resampled.size());
    data->nam_in_fifo.insert(data->nam_in_fifo.end(), resampled.begin(), resampled.begin() + static_cast<long>(got));

    std::array<float, NamAudio::kBlockSize> chunk{}, left{}, right{};
    const auto dsp_start = std::chrono::steady_clock::now();
    std::uint64_t blocks = 0;
    while (data->nam_in_fifo.size() >= NamAudio::kBlockSize) {
        std::copy(data->nam_in_fifo.begin(), data->nam_in_fifo.begin() + NamAudio::kBlockSize, chunk.begin());
        data->nam_in_fifo.erase(data->nam_in_fifo.begin(), data->nam_in_fifo.begin() + NamAudio::kBlockSize);
        data->audio->Process(chunk.data(), left.data(), right.data(), NamAudio::kBlockSize, /*bypass_model=*/false);
        data->output_fifo->Push(left.data(), NamAudio::kBlockSize);
        ++blocks;
    }
    const auto dsp_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - dsp_start).count());

    data->stats->input_frames.store(n, std::memory_order_relaxed);
    data->stats->resampler_pending.store(data->to_48k.Pending(), std::memory_order_relaxed);
    data->stats->nam_pending.store(data->nam_in_fifo.size(), std::memory_order_relaxed);
    data->stats->dsp_ns_total.fetch_add(dsp_ns, std::memory_order_relaxed);
    data->stats->dsp_blocks.fetch_add(blocks, std::memory_order_relaxed);
    AtomicMax(data->stats->dsp_ns_worst_callback, dsp_ns);
    return paContinue;
}

int OutputCallback(const void *, void *output_buffer, unsigned long frame_count,
                    const PaStreamCallbackTimeInfo *, PaStreamCallbackFlags status_flags, void *user_data) {
    auto *data = static_cast<OutputCallbackData *>(user_data);
    if (status_flags & paOutputUnderflow)
        data->stats->output_underflows.fetch_add(1, std::memory_order_relaxed);
    if (status_flags & paOutputOverflow)
        data->stats->output_overflows.fetch_add(1, std::memory_order_relaxed);

    auto *out = static_cast<float *>(output_buffer);
    const std::size_t n = frame_count;

    std::vector<float> mono_out(n, 0.0f);
    const std::size_t got = data->output_fifo->Pop(mono_out.data(), n);
    if (got < n) // ran dry: the rest of this buffer goes out as silence
        data->stats->starved_samples.fetch_add(n - got, std::memory_order_relaxed);

    const std::uint64_t fifo_after = data->output_fifo->Size();
    data->stats->output_frames.store(n, std::memory_order_relaxed);
    AtomicMin(data->stats->fifo_min, fifo_after);
    AtomicMax(data->stats->fifo_max, fifo_after);

    const std::size_t out_channels = static_cast<std::size_t>(data->channels);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t ch = 0; ch < out_channels; ++ch)
            out[i * out_channels + ch] = mono_out[i]; // same mono signal to every channel
    }
    return paContinue;
}

struct StatsView {
    PaStream *input_stream;
    PaStream *output_stream;
    SharedFifo *output_fifo;
    Stats *stats;
    double in_rate;
    double out_rate;
};

// Milliseconds a count of samples represents at a given rate.
double Ms(double samples, double rate) { return samples / rate * 1000.0; }

void PrintStatsLoop(StatsView view, std::atomic<bool> *keep_running) {
    Stats *stats = view.stats;
    const auto started = std::chrono::steady_clock::now();

    while (keep_running->load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!keep_running->load(std::memory_order_relaxed))
            break;

        const double elapsed_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        // Fold the depth we are about to print into the interval's range, so
        // "now" always sits inside the min..max the same line reports.
        const std::size_t fifo_now = view.output_fifo->Size();
        AtomicMin(stats->fifo_min, fifo_now);
        AtomicMax(stats->fifo_max, fifo_now);

        // Take and reset the per-interval accumulators in one go, so the line
        // describes the second just ended.
        const std::uint64_t fifo_min_raw = stats->fifo_min.exchange(kNoMin, std::memory_order_relaxed);
        const std::uint64_t fifo_max = stats->fifo_max.exchange(0, std::memory_order_relaxed);
        const std::uint64_t dsp_ns_total = stats->dsp_ns_total.exchange(0, std::memory_order_relaxed);
        const std::uint64_t dsp_worst_ns = stats->dsp_ns_worst_callback.exchange(0, std::memory_order_relaxed);
        const std::uint64_t dsp_blocks = stats->dsp_blocks.exchange(0, std::memory_order_relaxed);
        const std::uint64_t starved = stats->starved_samples.exchange(0, std::memory_order_relaxed);
        const std::uint64_t fifo_min = fifo_min_raw == kNoMin ? 0 : fifo_min_raw;

        const PaStreamInfo *in_info = Pa_GetStreamInfo(view.input_stream);
        const PaStreamInfo *out_info = Pa_GetStreamInfo(view.output_stream);
        const double device_in_ms = in_info ? in_info->inputLatency * 1000.0 : 0.0;
        const double device_out_ms = out_info ? out_info->outputLatency * 1000.0 : 0.0;

        const std::uint64_t resampler_smp = stats->resampler_pending.load(std::memory_order_relaxed);
        const std::uint64_t nam_smp = stats->nam_pending.load(std::memory_order_relaxed);
        const std::uint64_t in_frames = stats->input_frames.load(std::memory_order_relaxed);
        const std::uint64_t out_frames = stats->output_frames.load(std::memory_order_relaxed);

        const double resampler_ms = Ms(static_cast<double>(resampler_smp), view.in_rate);
        const double nam_ms = Ms(static_cast<double>(nam_smp), NamAudio::kSampleRate);
        const double fifo_now_ms = Ms(static_cast<double>(fifo_now), NamAudio::kSampleRate);
        const double fifo_min_ms = Ms(static_cast<double>(fifo_min), NamAudio::kSampleRate);
        const double fifo_max_ms = Ms(static_cast<double>(fifo_max), NamAudio::kSampleRate);
        // Everything between the mic and the speaker. The device figures come
        // from PortAudio; the middle three are ours and are invisible to it.
        const double round_trip_ms = device_in_ms + resampler_ms + nam_ms + fifo_now_ms + device_out_ms;

        // A callback must finish before the next buffer is due; that deadline
        // is what the model's cost has to fit inside.
        const double in_budget_ms = in_frames > 0 ? Ms(static_cast<double>(in_frames), view.in_rate) : 0.0;
        const double dsp_avg_ms = dsp_blocks > 0 ? static_cast<double>(dsp_ns_total) / dsp_blocks / 1e6 : 0.0;
        const double dsp_worst_ms = static_cast<double>(dsp_worst_ns) / 1e6;
        const double dsp_headroom = in_budget_ms > 0.0 ? dsp_worst_ms / in_budget_ms * 100.0 : 0.0;

        std::ostringstream line;
        line << std::fixed << std::setprecision(1);
        line << "[stats " << std::setprecision(0) << elapsed_s << "s] " << std::setprecision(1)
             << "ROUND TRIP " << round_trip_ms << " ms mic to speaker\n";
        line << "  1 mic in     device buffer " << device_in_ms << " ms"
             << "  (" << in_frames << " frames per callback, cpu "
             << Pa_GetStreamCpuLoad(view.input_stream) * 100.0 << "%)\n";
        line << "  2 resampler  " << resampler_ms << " ms held"
             << "  (" << resampler_smp << " samples at " << std::setprecision(0) << view.in_rate
             << std::setprecision(1) << " Hz)\n";
        line << "  3 block fill " << nam_ms << " ms held"
             << "  (" << nam_smp << " of " << NamAudio::kBlockSize << " samples toward the next model block)\n";
        line << "  4 model      " << std::setprecision(3) << dsp_avg_ms << " ms per block avg, "
             << dsp_worst_ms << " ms worst callback" << std::setprecision(1)
             << "  (" << dsp_headroom << "% of the " << in_budget_ms << " ms deadline, "
             << dsp_blocks << " blocks/s)\n";
        line << "  5 fifo       " << fifo_now_ms << " ms now, swung " << fifo_min_ms << " to " << fifo_max_ms
             << " ms  (" << fifo_now << " samples, jitter " << (fifo_max_ms - fifo_min_ms) << " ms)\n";
        line << "  6 speaker    device buffer " << device_out_ms << " ms"
             << "  (" << out_frames << " frames per callback, cpu "
             << Pa_GetStreamCpuLoad(view.output_stream) * 100.0 << "%)\n";
        line << "  glitches     underflow in=" << stats->input_underflows.load(std::memory_order_relaxed)
             << " out=" << stats->output_underflows.load(std::memory_order_relaxed)
             << " | overflow in=" << stats->input_overflows.load(std::memory_order_relaxed)
             << " out=" << stats->output_overflows.load(std::memory_order_relaxed)
             << " | silence filled " << starved << " samples this second\n";
        std::cout << line.str() << std::flush;
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " amp_id(1=Fender 2=Vox 3=Marshall) input_device_index output_device_index\n";
        std::cerr << "Run list_devices.exe to find the right indices.\n";
        return 2;
    }
    const int amp_id = std::atoi(argv[1]);
    if (amp_id < 1 || amp_id > 3) {
        std::cerr << "amp_id must be 1, 2, or 3\n";
        return 2;
    }
    const int input_device = std::atoi(argv[2]);
    const int output_device = std::atoi(argv[3]);

    NamAudio audio;
    if (!audio.LoadAmpModel(static_cast<AmpId>(amp_id))) {
        std::cerr << "Failed to load amp model " << amp_id << "\n";
        return 1;
    }

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "PortAudio init failed: " << Pa_GetErrorText(err) << "\n";
        return 1;
    }

    const PaDeviceInfo *in_info = Pa_GetDeviceInfo(input_device);
    const PaDeviceInfo *out_info = Pa_GetDeviceInfo(output_device);
    if (!in_info || !out_info) {
        std::cerr << "Invalid device index. Run list_devices.exe to check.\n";
        Pa_Terminate();
        return 1;
    }
    // Open each device with the channel count it actually has, capped at
    // stereo: a mono-only input (e.g. a built-in laptop mic) rejects a
    // hardcoded 2-channel request with "Invalid number of channels".
    const int in_channels = std::min(2, in_info->maxInputChannels);
    const int out_channels = std::min(2, out_info->maxOutputChannels);
    if (in_channels < 1) {
        std::cerr << "Device " << input_device << " (" << in_info->name << ") has no input channels.\n";
        Pa_Terminate();
        return 1;
    }
    if (out_channels < 1) {
        std::cerr << "Device " << output_device << " (" << out_info->name << ") has no output channels.\n";
        Pa_Terminate();
        return 1;
    }
    const double in_rate = in_info->defaultSampleRate;
    const double out_rate = out_info->defaultSampleRate;
    std::cout << "Input:  [" << input_device << "] " << in_info->name << " (" << in_rate << " Hz, "
              << in_channels << " ch)\n";
    std::cout << "Output: [" << output_device << "] " << out_info->name << " (" << out_rate << " Hz, "
              << out_channels << " ch)\n";
    std::cout << "NAM model runs at " << NamAudio::kSampleRate << " Hz; resampling input as needed.\n";

    Stats stats;
    SharedFifo output_fifo;
    InputCallbackData in_data{&audio, LinearResampler(in_rate, static_cast<double>(NamAudio::kSampleRate)), {}, &output_fifo, &stats, in_channels};
    OutputCallbackData out_data{&output_fifo, &stats, out_channels};

    PaStreamParameters input_params{};
    input_params.device = input_device;
    input_params.channelCount = in_channels;
    input_params.sampleFormat = paFloat32;
    input_params.suggestedLatency = in_info->defaultLowInputLatency;
    input_params.hostApiSpecificStreamInfo = nullptr;

    PaStreamParameters output_params{};
    output_params.device = output_device;
    output_params.channelCount = out_channels;
    output_params.sampleFormat = paFloat32;
    output_params.suggestedLatency = out_info->defaultLowOutputLatency;
    output_params.hostApiSpecificStreamInfo = nullptr;

    PaStream *input_stream = nullptr;
    err = Pa_OpenStream(&input_stream, &input_params, nullptr, in_rate,
                         paFramesPerBufferUnspecified, paNoFlag, InputCallback, &in_data);
    if (err != paNoError) {
        std::cerr << "Failed to open input stream: " << Pa_GetErrorText(err) << "\n";
        Pa_Terminate();
        return 1;
    }

    PaStream *output_stream = nullptr;
    err = Pa_OpenStream(&output_stream, nullptr, &output_params, out_rate,
                         paFramesPerBufferUnspecified, paNoFlag, OutputCallback, &out_data);
    if (err != paNoError) {
        std::cerr << "Failed to open output stream: " << Pa_GetErrorText(err) << "\n";
        Pa_CloseStream(input_stream);
        Pa_Terminate();
        return 1;
    }

    Pa_StartStream(input_stream);
    Pa_StartStream(output_stream);

    std::atomic<bool> keep_running{true};
    StatsView stats_view{input_stream, output_stream, &output_fifo, &stats, in_rate, out_rate};
    std::thread stats_thread(PrintStatsLoop, stats_view, &keep_running);

    std::cout << "Running " << AmpName(static_cast<AmpId>(amp_id)) << " live. Press Enter to stop.\n";
    std::cin.get();

    keep_running.store(false, std::memory_order_relaxed);
    stats_thread.join();

    Pa_StopStream(input_stream);
    Pa_StopStream(output_stream);
    Pa_CloseStream(input_stream);
    Pa_CloseStream(output_stream);
    Pa_Terminate();
    return 0;
}