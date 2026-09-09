#include <algorithm>
#include <iostream>
#include <vector>
#include <cmath>
#include <string>
#include <deque>
#include <mutex>
#include <io.h>
#include <fcntl.h>
#include <fstream>
#include <filesystem>
#include <map>
#include <memory>
#include <array>
#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"
#include "realaec_sdk.h" // RealAEC SDK
#include "AudioUtils.h"

// MinGW fix: Provide storage for KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
// It is declared in headers but not defined in libraries linked by MinGW sometimes.
extern "C" const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// 使用 M_PI 需要一些配置，或者直接定义
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ===================== 全局控制 =====================

std::atomic<bool> g_running(true);

BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

// ===================== 接口定义 =====================

class IAudioProcessor {
public:
    virtual ~IAudioProcessor() = default;
    virtual void Process(const short* micSignal, const short* spkSignal, short* outSignal) = 0;
    virtual std::wstring GetName() const = 0;
};

// Hard frame gate applied after the selected AEC/denoise chain. The threshold
// is normalized RMS amplitude: 0 disables the gate, 1 represents full scale.
static void ApplyNoiseGate(short* samples, size_t count, double threshold) {
    if (!samples || count == 0 || threshold <= 0.0) return;
    threshold = std::clamp(threshold, 0.0, 1.0);
    double sumSquares = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double normalized = static_cast<double>(samples[i]) / 32768.0;
        sumSquares += normalized * normalized;
    }
    const double frameRms = std::sqrt(sumSquares / static_cast<double>(count));
    if (frameRms < threshold) std::fill(samples, samples + count, 0);
}

// ===================== SpeexAEC 实现 =====================

class SpeexAEC : public IAudioProcessor {
public:
    SpeexAEC(int sampleRate, int frameSize, int filterLength, bool enablePostDenoise = true,
             bool enableAgc = true)
        : m_frameSize(frameSize), m_sampleRate(sampleRate),
          m_enablePostDenoise(enablePostDenoise), m_enableAgc(enableAgc) {
        
        m_echoState = speex_echo_state_init(frameSize, filterLength);
        if (!m_echoState) {
            std::cerr << "Failed to init Speex Echo State!" << std::endl;
            return;
        }

        speex_echo_ctl(m_echoState, SPEEX_ECHO_SET_SAMPLING_RATE, &m_sampleRate);

        if (m_enablePostDenoise) {
            m_preprocessState = speex_preprocess_state_init(frameSize, sampleRate);
            if (!m_preprocessState) {
                std::cerr << "Failed to init Speex Preprocess State!" << std::endl;
                return;
            }

            speex_preprocess_ctl(m_preprocessState, SPEEX_PREPROCESS_SET_ECHO_STATE, m_echoState);

            int denoise = 1;
            speex_preprocess_ctl(m_preprocessState, SPEEX_PREPROCESS_SET_DENOISE, &denoise);

            int agc = m_enableAgc ? 1 : 0;
            speex_preprocess_ctl(m_preprocessState, SPEEX_PREPROCESS_SET_AGC, &agc);
        }

        std::cout << "Speex AEC Initialized: Rate=" << sampleRate 
                  << ", Frame=" << frameSize 
                  << ", FilterLen=" << filterLength << std::endl;
    }

    ~SpeexAEC() {
        if (m_preprocessState) speex_preprocess_state_destroy(m_preprocessState);
        if (m_echoState) speex_echo_state_destroy(m_echoState);
    }

    void Process(const short* micSignal, const short* spkSignal, short* outSignal) override {
        if (!m_echoState) {
            std::copy(micSignal, micSignal + m_frameSize, outSignal);
            return;
        }
        speex_echo_cancellation(m_echoState, micSignal, spkSignal, outSignal);
        
        if (m_enablePostDenoise && m_preprocessState) {
            speex_preprocess_run(m_preprocessState, outSignal);
        }
    }

    std::wstring GetName() const override { 
        if (!m_enablePostDenoise) return L"SpeexDSP (Linear Only)";
        return m_enableAgc ? L"SpeexDSP (Full)" : L"SpeexDSP (Linear + Post Denoise)";
    }

private:
    SpeexEchoState* m_echoState = nullptr;
    SpeexPreprocessState* m_preprocessState = nullptr;
    int m_frameSize;
    int m_sampleRate;
    bool m_enablePostDenoise;
    bool m_enableAgc;
};

// ===================== Real AEC 实现 =====================

class RealAEC : public IAudioProcessor {
public:
    RealAEC(int sampleRate, int frameSize) 
        : m_frameSize(frameSize), m_sampleRate(sampleRate) {
        std::cout << "RealAEC constructor calling REAL_AEC_create..." << std::endl;
        m_state = REAL_AEC_create(frameSize, sampleRate);
        std::cout << "RealAEC constructor calling REAL_AEC_create success..." << std::endl;
        if (!m_state) {
            std::cerr << "Failed to init Real AEC State!" << std::endl;
        } else {
            std::cout << "Real AEC Initialized: Rate=" << sampleRate 
                      << ", Frame=" << frameSize << std::endl;
        }
    }

    ~RealAEC() {
        if (m_state) REAL_AEC_delete(m_state);
    }

    void Process(const short* micSignal, const short* spkSignal, short* outSignal) override {
        if (!m_state) {
            std::copy(micSignal, micSignal + m_frameSize, outSignal);
            return;
        }

        REAL_AEC_process(m_state, const_cast<short*>(micSignal), const_cast<short*> (spkSignal), outSignal);
    }

    std::wstring GetName() const override { return L"RealAEC (Third Party)"; }

private:
    void* m_state = nullptr;
    int m_frameSize;
    int m_sampleRate;
};

// ===================== WebRTC AEC3 实现 (Placeholder/Stub) =====================

#if __has_include("webrtc/modules/audio_processing/include/audio_processing.h")
#define HAS_WEBRTC 1
#include "webrtc/modules/audio_processing/include/audio_processing.h"
#include "webrtc/modules/audio_processing/include/audio_processing_statistics.h"
#elif __has_include("modules/audio_processing/include/audio_processing.h")
#define HAS_WEBRTC 1
#include "modules/audio_processing/include/audio_processing.h"
#else
#define HAS_WEBRTC 0
#endif

class WebRtcAEC : public IAudioProcessor {
public:
    WebRtcAEC(int sampleRate, int channels, int frameSize) 
        : m_sampleRate(sampleRate), m_channels(channels), m_frameSize(frameSize) {
#if HAS_WEBRTC
        std::cout << "WebRTC AEC3 Initialized. FrameSize=" << frameSize << std::endl;
        
        if (frameSize != sampleRate / 100) {
            std::cerr << "ERROR: WebRTC APM requires 10ms frames! (Expected " 
                      << (sampleRate / 100) << ", got " << frameSize << ")" << std::endl;
        }

        webrtc::AudioProcessing::Config config;
        
        // Enable AEC
        config.echo_canceller.enabled = true;
        config.echo_canceller.mobile_mode = false; // Use robust AEC3 (Desktop)
        config.echo_canceller.export_linear_aec_output = false;
        // Tuning for better voice quality
        
        // 1. Noise Suppression: Use kLow to preserve more voice details and avoid "underwater" sound
        config.noise_suppression.enabled = true;
        config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kLow;

        // 2. Gain Controller: Use AdaptiveDigital if analog control is not available
        // Reduce compression gain to avoid breathing effects
        config.gain_controller1.enabled = true;
        config.gain_controller1.mode = webrtc::AudioProcessing::Config::GainController1::kAdaptiveDigital;
        config.gain_controller1.compression_gain_db = 6; // Default is 9, lower is more natural
        
        config.high_pass_filter.enabled = true;
        // Ensure transient suppression is off (it handles keyboard clicks etc, but might affect voice)
        config.transient_suppression.enabled = false;
        // 3. 进阶：调整AEC3的专属参数（WebRTC新版默认用AEC3）

        m_apm = webrtc::AudioProcessingBuilder().Create();
        m_apm->ApplyConfig(config);
#else
        std::cerr << "WARNING: WebRTC headers not found. Running in Passthrough mode." << std::endl;
#endif
    }

    void Process(const short* micSignal, const short* spkSignal, short* outSignal) override {
#if HAS_WEBRTC
        if (!m_apm) {
             std::copy(micSignal, micSignal + m_frameSize, outSignal);
             return;
        }

        // WebRTC expects Int16 chunks (10ms)
        // We need to copy const inputs to non-const buffers because APM API requires it (for in-place support)
        
        // 1. Process Reverse Stream (Render/Speaker)
        // Copy spkSignal to a mutable buffer
        std::vector<short> attenuatedSpk(m_frameSize);
        for (int i = 0; i < m_frameSize; ++i) {
            attenuatedSpk[i] = spkSignal[i];
        }
        // std::vector<short> far_end(spkSignal, spkSignal + m_frameSize);
        
        webrtc::StreamConfig config(m_sampleRate, m_channels); // has_keyboard is deprecated and removed
        
        // APM uses pointers to channels
        short* far_end_ptr = attenuatedSpk.data();
        m_apm->ProcessReverseStream(far_end_ptr, config, config, far_end_ptr);

        // 2. Process Capture Stream (Mic)
        // Copy micSignal to a mutable buffer
        std::vector<short> near_end(micSignal, micSignal + m_frameSize);
        short* near_end_ptr = near_end.data();
        
        // Output buffer
        short* out_ptr = outSignal; // Caller provided buffer

        // ProcessStream(const int16_t* const src, const StreamConfig& input_config, const StreamConfig& output_config, int16_t* const dest);
        m_apm->ProcessStream(near_end_ptr, config, config, out_ptr);
        
#else
        // Passthrough for stub
        std::copy(micSignal, micSignal + m_frameSize, outSignal);
#endif
    }

    std::wstring GetName() const override { 
#if HAS_WEBRTC
        return L"WebRTC APM (AEC3)"; 
#else
        return L"WebRTC AEC3 (Passthrough)"; 
#endif
    }

private:
    int m_sampleRate;
    int m_channels;
    int m_frameSize;
#if HAS_WEBRTC
    rtc::scoped_refptr<webrtc::AudioProcessing> m_apm;
#endif
};

// ===================== 统一封装 =====================

class AudioProcessorWrapper {
public:
    AudioProcessorWrapper(const std::string& type, int sampleRate, int frameSize, int filterLen,
                          double noiseGateThreshold = 0.0)
        : m_frameSize(frameSize),
          m_noiseGateThreshold(std::clamp(noiseGateThreshold, 0.0, 1.0)) {
        std::cout << "AudioProcessorWrapper initializing with type: " << type << std::endl;
        if (type == "webrtc") {
            m_processor = std::make_unique<WebRtcAEC>(sampleRate, 1, frameSize);
            m_isWebRtc = true;
        } else if (type == "speex_linear") {
            m_processor = std::make_unique<SpeexAEC>(sampleRate, frameSize, filterLen, false);
            m_isWebRtc = false;
        } else if (type == "speex_linear_denoise") {
            // Keep the linear AEC's gain unchanged, then remove residual noise after AEC.
            m_processor = std::make_unique<SpeexAEC>(sampleRate, frameSize, filterLen, true, false);
            m_isWebRtc = false;
        } else if (type == "real_aec") {
            m_processor = std::make_unique<RealAEC>(sampleRate, frameSize);
            m_isWebRtc = false;
        } else {
            // Default to Speex Full
            m_processor = std::make_unique<SpeexAEC>(sampleRate, frameSize, filterLen, true);
            m_isWebRtc = false;
        }
    }

    void Process(const short* mic, const short* spk, short* out) {
        if (m_isWebRtc) {
#if !HAS_WEBRTC
             std::copy(mic, mic + m_frameSize, out);
#else
             m_processor->Process(mic, spk, out);
#endif
        } else {
            m_processor->Process(mic, spk, out);
        }
        ApplyNoiseGate(out, static_cast<size_t>(m_frameSize), m_noiseGateThreshold);
    }
    
    std::wstring GetName() const { return m_processor->GetName(); }

private:
    std::unique_ptr<IAudioProcessor> m_processor;
    int m_frameSize;
    double m_noiseGateThreshold;
    bool m_isWebRtc;
};

// ===================== 离线 WAV 处理 =====================

struct Pcm16MonoWav {
    int sampleRate = 0;
    std::vector<short> samples;
};

bool ReadPcm16MonoWav(const std::string& filename, Pcm16MonoWav& wav) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) return false;

    std::array<char, 12> riff{};
    file.read(riff.data(), riff.size());
    if (!file || std::string(riff.data(), 4) != "RIFF" ||
        std::string(riff.data() + 8, 4) != "WAVE") {
        return false;
    }

    uint16_t format = 0, channels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0, dataSize = 0;
    bool foundData = false;
    while (file && !foundData) {
        std::array<char, 4> chunkId{};
        uint32_t chunkSize = 0;
        file.read(chunkId.data(), chunkId.size());
        file.read(reinterpret_cast<char*>(&chunkSize), sizeof(chunkSize));
        if (!file) break;

        const std::string id(chunkId.data(), chunkId.size());
        if (id == "fmt ") {
            if (chunkSize < 16) return false;
            uint32_t byteRate = 0;
            uint16_t blockAlign = 0;
            file.read(reinterpret_cast<char*>(&format), sizeof(format));
            file.read(reinterpret_cast<char*>(&channels), sizeof(channels));
            file.read(reinterpret_cast<char*>(&sampleRate), sizeof(sampleRate));
            file.read(reinterpret_cast<char*>(&byteRate), sizeof(byteRate));
            file.read(reinterpret_cast<char*>(&blockAlign), sizeof(blockAlign));
            file.read(reinterpret_cast<char*>(&bitsPerSample), sizeof(bitsPerSample));
            file.seekg(static_cast<std::streamoff>(chunkSize) - 16 + (chunkSize & 1), std::ios::cur);
        } else if (id == "data") {
            dataSize = chunkSize;
            foundData = true;
        } else {
            file.seekg(chunkSize + (chunkSize & 1), std::ios::cur);
        }
    }

    if (!foundData || format != 1 || channels != 1 || bitsPerSample != 16 || sampleRate == 0 ||
        dataSize % sizeof(short) != 0) {
        return false;
    }
    wav.sampleRate = static_cast<int>(sampleRate);
    wav.samples.resize(dataSize / sizeof(short));
    file.read(reinterpret_cast<char*>(wav.samples.data()), dataSize);
    return static_cast<bool>(file);
}

bool WritePcm16MonoWav(const std::string& filename, int sampleRate, const std::vector<short>& samples) {
    WavWriter writer(filename, sampleRate, 1, 16);
    writer.Write(samples);
    writer.Close();
    return true;
}

int RunOfflineAec(const std::string& type, const std::string& micFilename,
                  const std::string& loopbackFilename, const std::string& outputFilename) {
    Pcm16MonoWav mic, loopback;
    if (!ReadPcm16MonoWav(micFilename, mic) || !ReadPcm16MonoWav(loopbackFilename, loopback)) {
        std::cerr << "Offline mode requires mono 16-bit PCM WAV input files." << std::endl;
        return 1;
    }
    if (mic.sampleRate != loopback.sampleRate || mic.samples.size() != loopback.samples.size()) {
        std::cerr << "Offline inputs must have matching sample rates and durations." << std::endl;
        return 1;
    }

    const int frameSize = mic.sampleRate / 100;
    if (frameSize <= 0 || mic.samples.size() % frameSize != 0) {
        std::cerr << "Offline inputs must contain an exact number of 10 ms frames." << std::endl;
        return 1;
    }

    AudioProcessorWrapper processor(type, mic.sampleRate, frameSize, mic.sampleRate / 5);
    std::vector<short> output(mic.samples.size());
    for (size_t offset = 0; offset < mic.samples.size(); offset += frameSize) {
        processor.Process(mic.samples.data() + offset, loopback.samples.data() + offset,
                          output.data() + offset);
    }
    WritePcm16MonoWav(outputFilename, mic.sampleRate, output);
    std::cout << "Offline AEC complete: " << outputFilename << " (" << processor.GetName().c_str() << ")" << std::endl;
    return 0;
}

// ===================== 配置管理 =====================

struct AppConfig {
    std::wstring micDeviceId;
    std::wstring loopbackDeviceId;
    std::wstring outputDeviceId;
    std::string aecType;
    bool autoStart = false;
    bool engineWasRunning = false;
    std::wstring backgroundImage;
    double backgroundOpacity = 0.0;
    int windowWidth = 0;
    int windowHeight = 0;
    bool recordingEnabled = false;
    double noiseGateThreshold = 0.0;
};

static std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0);
    if (length <= 0) return std::wstring(value.begin(), value.end());
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), length);
    return result;
}

static std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return std::string(value.begin(), value.end());
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

bool LoadConfig(const std::string& filename, AppConfig& config) {
    std::ifstream file(std::filesystem::u8path(filename));
    if (!file.is_open()) return false;

    std::string line;
    std::map<std::string, std::string> settings;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        size_t pos = line.find(L'=');
        if (pos != std::string::npos) {
            settings[line.substr(0, pos)] = line.substr(pos + 1);
        }
    }

    if (settings.count("mic_id")) config.micDeviceId = Utf8ToWide(settings["mic_id"]);
    if (settings.count("loopback_id")) config.loopbackDeviceId = Utf8ToWide(settings["loopback_id"]);
    if (settings.count("output_id")) config.outputDeviceId = Utf8ToWide(settings["output_id"]);
    if (settings.count("aec_type")) {
        config.aecType = settings["aec_type"];
    } else {
        config.aecType = "webrtc"; // Default to WebRTC
    }
    if (settings.count("auto_start")) {
        config.autoStart = settings["auto_start"] == "1" || settings["auto_start"] == "true";
    }
    if (settings.count("engine_running")) {
        config.engineWasRunning = settings["engine_running"] == "1" || settings["engine_running"] == "true";
    }
    if (settings.count("background_image")) {
        config.backgroundImage = Utf8ToWide(settings["background_image"]);
    }
    if (settings.count("background_opacity")) {
        try {
            config.backgroundOpacity = std::stod(settings["background_opacity"]);
        } catch (...) {
            config.backgroundOpacity = 0.0;
        }
    }
    if (settings.count("window_width")) {
        try { config.windowWidth = std::stoi(settings["window_width"]); } catch (...) { config.windowWidth = 0; }
    }
    if (settings.count("window_height")) {
        try { config.windowHeight = std::stoi(settings["window_height"]); } catch (...) { config.windowHeight = 0; }
    }
    if (settings.count("recording_enabled")) {
        config.recordingEnabled = settings["recording_enabled"] == "1" ||
                                  settings["recording_enabled"] == "true";
    }
    if (settings.count("noise_gate_threshold")) {
        try {
            config.noiseGateThreshold = std::clamp(
                std::stod(settings["noise_gate_threshold"]), 0.0, 1.0);
        } catch (...) {
            config.noiseGateThreshold = 0.0;
        }
    }

    // Command line override for aec_type
    // This is a bit hacky to put here, but convenient.
    // In a real app, parse args first, then load config, then override.
    return !config.micDeviceId.empty() && !config.loopbackDeviceId.empty() && !config.outputDeviceId.empty();
}

void PrintUsage() {
    std::cout << "Usage: SpeexEchoCanceller.exe [aec_type]" << std::endl;
    std::cout << "  aec_type options:" << std::endl;
    std::cout << "  - webrtc: WebRTC AEC3 (High Quality, 10ms only)" << std::endl;
    std::cout << "  - speex: Speex AEC (Standard)" << std::endl;
    std::cout << "  - speex_linear: Speex AEC (Linear Filter Only, No NLP)" << std::endl;
    std::cout << "  - speex_linear_denoise: Speex Linear AEC + Post Denoise (No AGC)" << std::endl;
    std::cout << "  - real_aec: Third-party Real AEC" << std::endl;
}

void SaveConfig(const std::string& filename, const AppConfig& config) {
    std::ofstream file(std::filesystem::u8path(filename));
    if (!file.is_open()) return;

    file << "mic_id=" << WideToUtf8(config.micDeviceId) << '\n';
    file << "loopback_id=" << WideToUtf8(config.loopbackDeviceId) << '\n';
    file << "output_id=" << WideToUtf8(config.outputDeviceId) << '\n';
    file << "aec_type=" << config.aecType << '\n';
    file << "auto_start=" << (config.autoStart ? "1" : "0") << '\n';
    file << "engine_running=" << (config.engineWasRunning ? "1" : "0") << '\n';
    file << "background_image=" << WideToUtf8(config.backgroundImage) << '\n';
    file << "background_opacity=" << config.backgroundOpacity << '\n';
    file << "window_width=" << config.windowWidth << '\n';
    file << "window_height=" << config.windowHeight << '\n';
    file << "recording_enabled=" << (config.recordingEnabled ? "1" : "0") << '\n';
    file << "noise_gate_threshold=" << config.noiseGateThreshold << '\n';
}

// ===================== 主逻辑 =====================

// 辅助函数：让用户选择设备
std::wstring SelectDevice(bool isCapture, const std::wstring& prompt) {
    auto devices = WASAPIUtils::EnumerateDevices(isCapture);
    std::wcout << L"\n--- " << prompt << L" ---" << std::endl;
    for (size_t i = 0; i < devices.size(); ++i) {
        std::wcout << i << L": " << devices[i].name << std::endl;
    }
    
    std::wcout << L"Enter device index (default 0): ";
    int idx = 0;
    if (std::wcin >> idx) {
        if (idx >= 0 && idx < devices.size()) {
            return devices[idx].id;
        }
    } else {
        std::wcin.clear();
        std::wcin.ignore(1000, '\n');
    }
    if (!devices.empty()) return devices[0].id;
    return L"";
}

#ifndef OFFLINE_AEC_LIBRARY
int main(int argc, char* argv[]) {
    if (argc == 6 && std::string(argv[1]) == "--offline") {
        return RunOfflineAec(argv[2], argv[3], argv[4], argv[5]);
    }

    // 设置控制台为 UTF-8 编码，解决 CLion Debug 乱码/空格问题
    // 不要使用 _O_U16TEXT，因为管道重定向时行为不一致
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    
    if (!SetConsoleCtrlHandler(ConsoleHandler, TRUE)) {
        std::cerr << "Error: Could not set control handler." << std::endl;
        return 1;
    }

    WASAPIUtils::InitializeCOM();

    bool autoUseConfig = false;
    bool enableRecording = false;

    // Check command line args
    std::string cmdAecType = "";
    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "speex" || arg1 == "webrtc" || arg1 == "speex_linear" ||
            arg1 == "speex_linear_denoise" || arg1 == "real_aec") {
            cmdAecType = arg1;
        } else if (arg1 == "--help" || arg1 == "-h") {
            // PrintUsage(); // Re-implement with cout below if needed, or just let loop handle
            std::cout << "Usage: SpeexEchoCanceller.exe [aec_type]" << std::endl;
            std::cout << "  aec_type options:" << std::endl;
            std::cout << "  - webrtc: WebRTC AEC3 (High Quality, 10ms only)" << std::endl;
            std::cout << "  - speex: Speex AEC (Standard)" << std::endl;
            std::cout << "  - speex_linear: Speex AEC (Linear Filter Only, No NLP)" << std::endl;
            std::cout << "  - speex_linear_denoise: Speex Linear AEC + Post Denoise (No AGC)" << std::endl;
            std::cout << "  - real_aec: Third-party Real AEC" << std::endl;
            return 0;
        }
    }

    AppConfig config;
    if (LoadConfig("config.ini", config)) {
        if (!cmdAecType.empty()) {
            config.aecType = cmdAecType; // Override config with cmd arg
        }
        
        std::cout << "Loaded config from config.ini:" << std::endl;
        std::cout << "  Mic: " << ToUtf8(config.micDeviceId) << std::endl;
        std::cout << "  Loopback: " << ToUtf8(config.loopbackDeviceId) << std::endl;
        std::cout << "  Output: " << ToUtf8(config.outputDeviceId) << std::endl;
        std::cout << "  AEC Type: " << config.aecType << std::endl;
        
        // Simplified flow: auto use if config exists
        autoUseConfig = true; 
    } else {
        if (!cmdAecType.empty()) {
             config.aecType = cmdAecType;
        }
    }

    if (autoUseConfig) {
         // std::cout << "Using loaded config..." << std::endl;
    }
    // 简单检查参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o") {
            autoUseConfig = true;
        } else if (arg == "-r") {
            enableRecording = true;
        }
    }

    config.aecType = "webrtc"; // Default
    bool configLoaded = false;
    const std::string CONFIG_FILE = "config.ini";

    if (LoadConfig(CONFIG_FILE, config)) {
        if (autoUseConfig) {
            std::cout << "Configuration loaded and used automatically (-o detected)." << std::endl;
            configLoaded = true;
        } else {
            std::cout << "Found saved configuration. Use it? (y/n): ";
            char choice;
            std::cin >> choice;
            // 清理输入缓冲区，防止影响后续输入
            std::cin.ignore(1000, '\n');
            
            if (choice == 'y' || choice == 'Y') {
                configLoaded = true;
            }
        }
    }

    // 1. 配置参数
    const int SAMPLE_RATE = 48000; // 统一使用 48kHz
    // WebRTC APM 严格要求 10ms 帧长 (480 samples @ 48kHz)
    const int FRAME_SIZE = SAMPLE_RATE / 100; // 10ms = 480 samples
    const int FILTER_LEN = SAMPLE_RATE / 5;  // 200ms = 9600 samples

    std::cout << "Initializing Speex AEC Demo..." << std::endl;
    std::cout << "Sample Rate: " << SAMPLE_RATE << ", Frame Size: " << FRAME_SIZE << std::endl;

    std::wstring micDeviceId, loopbackDeviceId, outputDeviceId;

    if (configLoaded) {
        micDeviceId = config.micDeviceId;
        loopbackDeviceId = config.loopbackDeviceId;
        outputDeviceId = config.outputDeviceId;
    } else {
        // 2. 选择设备
        // Near-end Mic
        // We need to reimplement SelectDevice to use cout/cin and ToUtf8
        auto SelectDeviceUtf8 = [](bool isCapture, const std::string& prompt) -> std::wstring {
            auto devices = WASAPIUtils::EnumerateDevices(isCapture);
            std::cout << "\n--- " << prompt << " ---" << std::endl;
            for (size_t i = 0; i < devices.size(); ++i) {
                std::cout << i << ": " << ToUtf8(devices[i].name) << std::endl;
            }
            
            std::cout << "Enter device index (default 0): ";
            int idx = 0;
            if (std::cin >> idx) {
                if (idx >= 0 && idx < devices.size()) {
                    return devices[idx].id;
                }
            } else {
                std::cin.clear();
                std::cin.ignore(1000, '\n');
            }
            if (!devices.empty()) return devices[0].id;
            return L"";
        };

        micDeviceId = SelectDeviceUtf8(true, "Select Microphone (Near-end Input)");
        
        // Far-end Loopback Source (Speaker)
        loopbackDeviceId = SelectDeviceUtf8(false, "Select Speaker for Background Sound (Far-end Loopback)");

        // Output (Virtual Mic / Line Out)
        outputDeviceId = SelectDeviceUtf8(false, "Select Output Device (Virtual Mic/Line Out)");
        
        // 保存配置
        if (!micDeviceId.empty() && !loopbackDeviceId.empty() && !outputDeviceId.empty()) {
            config.micDeviceId = micDeviceId;
            config.loopbackDeviceId = loopbackDeviceId;
            config.outputDeviceId = outputDeviceId;
            SaveConfig(CONFIG_FILE, config);
            std::cout << "Configuration saved to " << CONFIG_FILE << std::endl;
        }
    }

    if (micDeviceId.empty() || loopbackDeviceId.empty() || outputDeviceId.empty()) {
        std::cerr << "Error: Invalid device selection." << std::endl;
        return 1;
    }

    // 3. 打开流
    AudioStream micStream, loopbackStream, outStream;

    std::cout << "Opening Microphone..." << std::endl;
    // 尝试打开麦克风，不强制采样率，使用设备默认
    if (!micStream.Open(micDeviceId, true, false, 0)) {
        std::cerr << "Failed to open Microphone!" << std::endl;
        return 1;
    }
    int actualSampleRate = micStream.GetSampleRate();
    std::cout << "Microphone opened at " << actualSampleRate << " Hz" << std::endl;

    std::cout << "Opening Loopback..." << std::endl;
    if (!loopbackStream.Open(loopbackDeviceId, true, true, 0)) {
        std::cerr << "Failed to open Loopback!" << std::endl;
        return 1;
    }
    if (loopbackStream.GetSampleRate() != actualSampleRate) {
        std::cerr << "Warning: Loopback sample rate (" << loopbackStream.GetSampleRate() 
                  << " Hz) does not match Microphone (" << actualSampleRate << " Hz)!" << std::endl;
        std::cerr << "AEC requires matching sample rates. Please configure your devices in Windows Sound Settings." << std::endl;
        // 在此演示中，我们不处理重采样，直接继续可能会导致崩溃或噪音
    }

    std::cout << "Opening Output..." << std::endl;
    if (!outStream.Open(outputDeviceId, false, false, 0)) {
        std::cerr << "Failed to open Output!" << std::endl;
        return 1;
    }
    std::cout << "Output opened successfully." << std::endl;
    
    // 4. 创建 AEC 处理器
    std::cout << "Creating AEC Processor type: " << config.aecType << std::endl;
    AudioProcessorWrapper processor(config.aecType, SAMPLE_RATE, FRAME_SIZE, FILTER_LEN,
                                    config.noiseGateThreshold);
    std::cout << "AEC Processor created: " << ToUtf8(processor.GetName()) << std::endl;

    // 5. 缓冲区
    // WebRTC APM 每次处理 10ms
    // 我们需要从音频流中读取数据，攒够 10ms 就送入 AEC，然后输出
    
    std::vector<short> micBuffer(FRAME_SIZE);
    std::vector<short> loopbackBuffer(FRAME_SIZE);
    std::vector<short> outBuffer(FRAME_SIZE);

    // 延迟缓冲区
    const int DELAY_MS = 10;
    const size_t DELAY_SAMPLES = SAMPLE_RATE * DELAY_MS / 1000;
    std::deque<short> loopbackDelayBuffer;
    // 预先填充 0，保证刚开始就能取数据（虽然是静音）
    // 或者我们采用“等待积累”策略，看需求。这里采用“缓存积累”策略：
    // 即：新数据进队，如果队列长度 > DELAY + FRAME，则出队给 AEC。
    // 这样 AEC 会在最初的 300ms 收到静音（如果我们在 else 分支填 0），或者我们直接在队列初始化时填满 0。
    // 为了简单且稳定，我们初始化填满 0。这样逻辑最简单，相当于 Reference 信号前面多了 300ms 静音。
    // 但是等等，如果是“Reference 延迟 300ms 后送入”，意味着 Reference 应该比 Mic 晚 300ms？
    // 不，通常是为了补偿 Mic 的延迟，让 Reference "等一等" Mic。
    // 所以 Reference 应该被缓存。
    // 初始化填充 0 的话，意味着前 300ms 取出来的是 0，这是对的。
    loopbackDelayBuffer.resize(DELAY_SAMPLES, 0);
    
    // Recording writers
    std::unique_ptr<WavWriter> micWriter;
    std::unique_ptr<WavWriter> loopbackWriter;
    std::unique_ptr<WavWriter> outWriter;

    if (enableRecording) {
        micWriter = std::make_unique<WavWriter>("mic_input.wav", SAMPLE_RATE, 1, 16);
        loopbackWriter = std::make_unique<WavWriter>("loopback_input.wav", SAMPLE_RATE, 1, 16);
        outWriter = std::make_unique<WavWriter>("aec_output.wav", SAMPLE_RATE, 1, 16);
        std::cout << "Recording enabled. Files: mic_input.wav, loopback_input.wav, aec_output.wav" << std::endl;
    }
    
    std::cout << "Starting processing loop... Press Ctrl+C to stop." << std::endl;
    
    // 简单的统计
    long long processedFrames = 0;
    long long framesPassed = 0;
    const long long WARMUP_FRAMES = DELAY_SAMPLES / FRAME_SIZE;
    
    // 主循环
    while (g_running) {
        // 1. 从 Loopback 读取 (参考信号)
        // 这是一个简化模型：假设 mic 和 loopback 同步。实际上它们是独立的时钟。
        // WebRTC AEC3 对时钟漂移有一定容忍度，Speex 也有。
        // 为了演示，我们采用阻塞读取 Mic，非阻塞读取 Loopback (或者尽量对齐)
        
        // 读取 10ms Mic 数据 (阻塞)
        micBuffer.clear();
        while (micBuffer.size() < FRAME_SIZE && g_running) {
             int read = micStream.Read(micBuffer, FRAME_SIZE - micBuffer.size());
             if (read == 0) {
                 Sleep(1); // Wait 1ms
             }
        }
        if (!g_running) break;

        // 如果在预热期（延迟未满），丢弃麦克风数据（静音）
        if (framesPassed < WARMUP_FRAMES) {
             std::fill(micBuffer.begin(), micBuffer.end(), 0);
             framesPassed++;
        }
        
        // 读取 10ms Loopback 数据
        loopbackBuffer.clear();
        loopbackStream.Read(loopbackBuffer, FRAME_SIZE);
        if (loopbackBuffer.size() < FRAME_SIZE) {
            loopbackBuffer.resize(FRAME_SIZE, 0);
        }

        // --- Loopback Delay Logic Start ---
        // 1. 将新数据放入队列尾部
        loopbackDelayBuffer.insert(loopbackDelayBuffer.end(), loopbackBuffer.begin(), loopbackBuffer.end());

        // 2. 从队列头部取出延迟后的数据作为 AEC 的参考信号
        // 我们已经在初始化时填充了 DELAY_SAMPLES 个 0，所以这里直接取即可
        std::vector<short> delayedLoopback(FRAME_SIZE);
        if (loopbackDelayBuffer.size() >= FRAME_SIZE) {
            for (int i = 0; i < FRAME_SIZE; ++i) {
                delayedLoopback[i] = loopbackDelayBuffer.front();
                loopbackDelayBuffer.pop_front();
            }
        } else {
            // Should not happen if initialized correctly
            std::fill(delayedLoopback.begin(), delayedLoopback.end(), 0);
        }

        std::vector<short> attenuatedSpk(FRAME_SIZE);
        for (int i = 0; i < FRAME_SIZE; ++i) {
            attenuatedSpk[i] = delayedLoopback[i];
        }

        // --- Loopback Delay Logic End ---
        
        // 2. AEC 处理
        processor.Process(micBuffer.data(), attenuatedSpk.data(), outBuffer.data());
        
        // 3. 写入输出
        if (enableRecording) {
            if (micWriter) micWriter->Write(micBuffer);
            if (loopbackWriter) loopbackWriter->Write(attenuatedSpk); // 记录实际送入 AEC 的参考信号
            if (outWriter) outWriter->Write(outBuffer);
        }
        
        // 播放到虚拟麦克风
        outStream.Write(outBuffer);
        
        processedFrames++;
    }
    
    std::cout << "\nStopping..." << std::endl;
    
    return 0;
}
#endif
