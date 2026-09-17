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
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
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

private:
    double ratio_;
    double pos_ = 0.0;
    std::vector<float> buffer_;
};

// Counters updated from the (real-time) audio callbacks. Only cheap atomic
// increments happen on the audio thread; all printing happens elsewhere.
struct Stats {
    std::atomic<std::uint64_t> input_underflows{0};
    std::atomic<std::uint64_t> input_overflows{0};
    std::atomic<std::uint64_t> output_underflows{0};
    std::atomic<std::uint64_t> output_overflows{0};
};

struct InputCallbackData {
    NamAudio *audio;
    LinearResampler to_48k;
    std::vector<float> nam_in_fifo;
    SharedFifo *output_fifo;
    Stats *stats;
};

struct OutputCallbackData {
    SharedFifo *output_fifo;
    Stats *stats;
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
    for (std::size_t i = 0; i < n; ++i)
        mono_in[i] = in[i * 2]; // left channel of interleaved stereo input

    data->to_48k.Push(mono_in.data(), n);
    std::vector<float> resampled(n * 2 + 8);
    const std::size_t got = data->to_48k.Pop(resampled.data(), resampled.size());
    data->nam_in_fifo.insert(data->nam_in_fifo.end(), resampled.begin(), resampled.begin() + static_cast<long>(got));

    std::array<float, NamAudio::kBlockSize> chunk{}, left{}, right{};
    while (data->nam_in_fifo.size() >= NamAudio::kBlockSize) {
        std::copy(data->nam_in_fifo.begin(), data->nam_in_fifo.begin() + NamAudio::kBlockSize, chunk.begin());
        data->nam_in_fifo.erase(data->nam_in_fifo.begin(), data->nam_in_fifo.begin() + NamAudio::kBlockSize);
        data->audio->Process(chunk.data(), left.data(), right.data(), NamAudio::kBlockSize, /*bypass_model=*/false);
        data->output_fifo->Push(left.data(), NamAudio::kBlockSize);
    }
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
    data->output_fifo->Pop(mono_out.data(), n);

    for (std::size_t i = 0; i < n; ++i) {
        out[i * 2 + 0] = mono_out[i];
        out[i * 2 + 1] = mono_out[i];
    }
    return paContinue;
}

void PrintStatsLoop(PaStream *input_stream, PaStream *output_stream, SharedFifo *output_fifo,
                     Stats *stats, std::atomic<bool> *keep_running) {
    while (keep_running->load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!keep_running->load(std::memory_order_relaxed))
            break;

        const PaStreamInfo *in_info = Pa_GetStreamInfo(input_stream);
        const PaStreamInfo *out_info = Pa_GetStreamInfo(output_stream);
        const double in_latency_ms = in_info ? in_info->inputLatency * 1000.0 : -1.0;
        const double out_latency_ms = out_info ? out_info->outputLatency * 1000.0 : -1.0;
        const double in_cpu = Pa_GetStreamCpuLoad(input_stream) * 100.0;
        const double out_cpu = Pa_GetStreamCpuLoad(output_stream) * 100.0;
        const std::size_t fifo_samples = output_fifo->Size();
        const double fifo_ms = fifo_samples / static_cast<double>(NamAudio::kSampleRate) * 1000.0;
        const double estimated_round_trip_ms = in_latency_ms + out_latency_ms + fifo_ms;

        std::cout << std::fixed << std::setprecision(1)
                  << "[stats] EST. ROUND TRIP=" << estimated_round_trip_ms << "ms"
                  << " (device in=" << in_latency_ms << "ms + device out=" << out_latency_ms
                  << "ms + our fifo=" << fifo_ms << "ms)"
                  << " | cpu in=" << in_cpu << "% out=" << out_cpu << "%"
                  << " | fifo=" << fifo_samples << " samples"
                  << " | underflow in=" << stats->input_underflows.load() << " out=" << stats->output_underflows.load()
                  << " | overflow in=" << stats->input_overflows.load() << " out=" << stats->output_overflows.load()
                  << std::endl;
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
    const double in_rate = in_info->defaultSampleRate;
    const double out_rate = out_info->defaultSampleRate;
    std::cout << "Input:  [" << input_device << "] " << in_info->name << " (" << in_rate << " Hz)\n";
    std::cout << "Output: [" << output_device << "] " << out_info->name << " (" << out_rate << " Hz)\n";
    std::cout << "NAM model runs at " << NamAudio::kSampleRate << " Hz; resampling input as needed.\n";

    Stats stats;
    SharedFifo output_fifo;
    InputCallbackData in_data{&audio, LinearResampler(in_rate, static_cast<double>(NamAudio::kSampleRate)), {}, &output_fifo, &stats};
    OutputCallbackData out_data{&output_fifo, &stats};

    PaStreamParameters input_params{};
    input_params.device = input_device;
    input_params.channelCount = 2;
    input_params.sampleFormat = paFloat32;
    input_params.suggestedLatency = in_info->defaultLowInputLatency;
    input_params.hostApiSpecificStreamInfo = nullptr;

    PaStreamParameters output_params{};
    output_params.device = output_device;
    output_params.channelCount = 2;
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
    std::thread stats_thread(PrintStatsLoop, input_stream, output_stream, &output_fifo, &stats, &keep_running);

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