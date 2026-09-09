#define OFFLINE_AEC_LIBRARY
#include "main.cpp"

#include <algorithm>
#include <chrono>
#include <iomanip>

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 5) {
        std::cerr << "Usage: aec_benchmark <mic.wav> <loopback.wav> [iterations] [mode]\n";
        return 2;
    }

    Pcm16MonoWav mic, loopback;
    if (!ReadPcm16MonoWav(argv[1], mic) || !ReadPcm16MonoWav(argv[2], loopback) ||
        mic.sampleRate != 48000 || loopback.sampleRate != 48000 ||
        mic.samples.size() != loopback.samples.size() || mic.samples.size() % 480 != 0) {
        std::cerr << "Inputs must be matching 48 kHz mono PCM16 WAV files with whole 10 ms frames.\n";
        return 1;
    }

    const int iterations = argc == 4 ? std::max(1, std::atoi(argv[3])) : 5;
    std::vector<std::string> modes = {
        "webrtc", "speex", "speex_linear", "speex_linear_denoise", "real_aec"
    };
    if (argc == 5) {
        const std::string requestedMode = argv[4];
        if (std::find(modes.begin(), modes.end(), requestedMode) == modes.end()) {
            std::cerr << "Unknown mode: " << requestedMode << '\n';
            return 2;
        }
        modes = {requestedMode};
    }
    std::vector<short> output(480);

    std::cout << "mode,frames,mean_ms,p95_ms,max_ms,realtime_factor\n";
    for (const auto& mode : modes) {
        std::vector<double> timings;
        timings.reserve((mic.samples.size() / 480) * iterations);
        for (int iteration = 0; iteration < iterations; ++iteration) {
            AudioProcessorWrapper processor(mode, 48000, 480, 9600);
            for (size_t offset = 0; offset < mic.samples.size(); offset += 480) {
                const auto start = std::chrono::steady_clock::now();
                processor.Process(mic.samples.data() + offset,
                                  loopback.samples.data() + offset, output.data());
                timings.push_back(std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start).count());
            }
        }
        std::sort(timings.begin(), timings.end());
        double total = 0;
        for (double value : timings) total += value;
        const double mean = total / timings.size();
        const size_t p95Index = std::min(timings.size() - 1,
                                         static_cast<size_t>(timings.size() * 0.95));
        std::cout << mode << ',' << timings.size() << ',' << std::fixed
                  << std::setprecision(4) << mean << ',' << timings[p95Index] << ','
                  << timings.back() << ',' << mean / 10.0 << '\n';
    }
    return 0;
}
