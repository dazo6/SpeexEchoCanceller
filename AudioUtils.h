#pragma once

#include <initguid.h> // 必须在包含 devpkey.h 和其他定义 GUID 的头文件之前包含
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <cwchar>
#include <fstream>

// Helper to convert wstring to UTF-8 string
inline std::string ToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

// 链接必要的库（如果在 CMake 中没配好，这里作为备份）
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Mmdevapi.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Uuid.lib")

// 简单的宏用于错误检查
#define EXIT_ON_ERROR(hres)  \
              if (FAILED(hres)) { goto Exit; }
#define SAFE_RELEASE(punk)  \
              if ((punk) != nullptr)  \
                { (punk)->Release(); (punk) = nullptr; }

// 音频设备信息结构
struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isCapture; // true for mic, false for speaker
};

class WASAPIUtils {
public:
    // 初始化 COM (在主线程调用一次)
    static void InitializeCOM() {
        CoInitialize(NULL);
    }

    static void UninitializeCOM() {
        CoUninitialize();
    }

    // 列出设备
    static std::vector<AudioDeviceInfo> EnumerateDevices(bool capture) {
        std::vector<AudioDeviceInfo> devices;
        IMMDeviceEnumerator* pEnumerator = NULL;
        IMMDeviceCollection* pCollection = NULL;
        
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
        if (FAILED(hr)) return devices;

        EDataFlow dataFlow = capture ? eCapture : eRender;
        hr = pEnumerator->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, &pCollection);
        if (FAILED(hr)) { SAFE_RELEASE(pEnumerator); return devices; }

        UINT count;
        pCollection->GetCount(&count);

        for (UINT i = 0; i < count; i++) {
            IMMDevice* pDevice = NULL;
            IPropertyStore* pProps = NULL;
            LPWSTR pwszID = NULL;

            hr = pCollection->Item(i, &pDevice);
            if (FAILED(hr)) continue;

            hr = pDevice->GetId(&pwszID);
            if (FAILED(hr)) { SAFE_RELEASE(pDevice); continue; }

            hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
            if (FAILED(hr)) { 
                CoTaskMemFree(pwszID); 
                SAFE_RELEASE(pDevice); 
                continue; 
            }

            PROPVARIANT varName;
            PropVariantInit(&varName);
            // 尝试获取 FriendlyName
            hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
            
            // 如果获取失败，就用 ID 代替
            // (Old MinGW might not have PKEY_Device_DeviceDesc)
            
            // 如果还是空的，就用 ID 代替
            AudioDeviceInfo info;
            info.id = pwszID;
            if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                info.name = varName.pwszVal;
            } else {
                 info.name = L"Unknown Device (ID only)";
            }
            info.isCapture = capture;
            devices.push_back(info);

            PropVariantClear(&varName);
            CoTaskMemFree(pwszID);
            SAFE_RELEASE(pProps);
            SAFE_RELEASE(pDevice);
        }

        SAFE_RELEASE(pCollection);
        SAFE_RELEASE(pEnumerator);
        return devices;
    }
};

// 音频流类
class AudioStream {
public:
    AudioStream() : _pClient(NULL), _pCaptureClient(NULL), _pRenderClient(NULL), _pDevice(NULL) {}
    ~AudioStream() { Close(); }

    // 初始化流
    // deviceId: 设备ID，如果是 loopback 模式，传入 扬声器(Render) 的设备ID
    // isCapture: true 为录音，false 为播放
    // isLoopback: true 为捕获系统声音（必须 isCapture=true 且 deviceId 指向一个 Render 设备）
    // targetSampleRate: 期望的采样率 (e.g. 48000)
    bool Open(std::wstring deviceId, bool isCapture, bool isLoopback, int targetSampleRate) {
        HRESULT hr = S_OK;
        // Local resources that need cleanup
        IMMDeviceEnumerator* pEnumerator = nullptr;
        WAVEFORMATEX* pwfx = nullptr;
        
        bool success = false;

        _isCapture = isCapture;
        _isLoopback = isLoopback;
        _targetSampleRate = targetSampleRate;

        do {
            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL,
                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
            if (FAILED(hr)) break;

            if (deviceId.empty()) {
                hr = pEnumerator->GetDefaultAudioEndpoint(
                    (isCapture && !isLoopback) ? eCapture : eRender, eConsole, &_pDevice);
            } else {
                hr = pEnumerator->GetDevice(deviceId.c_str(), &_pDevice);
            }
            if (FAILED(hr)) break;

            hr = _pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&_pClient);
            if (FAILED(hr)) break;

            // 获取设备混合格式
            hr = _pClient->GetMixFormat(&pwfx);
            if (FAILED(hr)) break;
            
            // 保存格式信息
            _channels = pwfx->nChannels;
            _sampleRate = pwfx->nSamplesPerSec;
            _bitsPerSample = pwfx->wBitsPerSample; // 通常是 32 float
            
            // 简单检查
            if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                WAVEFORMATEXTENSIBLE* pEx = (WAVEFORMATEXTENSIBLE*)pwfx;
                if (IsEqualGUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, pEx->SubFormat)) {
                    _isFloat = true;
                } else {
                    _isFloat = false; // 可能是 PCM 16
                }
            } else if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                 _isFloat = true;
            } else {
                 _isFloat = false;
            }

            std::cout << "Device Format: " << _sampleRate << "Hz, " << _channels << " Channels, " << (_isFloat ? "Float" : "PCM") << std::endl;

            // 初始化 Audio Client
            DWORD flags = 0;
            if (isLoopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

            REFERENCE_TIME hnsRequestedDuration = 10000000 / 20; // 50ms buffer (100ns units)

            hr = _pClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                flags,
                hnsRequestedDuration,
                0,
                pwfx,
                NULL);
            if (FAILED(hr)) break;

            if (isCapture) {
                hr = _pClient->GetService(__uuidof(IAudioCaptureClient), (void**)&_pCaptureClient);
            } else {
                hr = _pClient->GetService(__uuidof(IAudioRenderClient), (void**)&_pRenderClient);
            }
            if (FAILED(hr)) break;

            hr = _pClient->Start();
            if (FAILED(hr)) break;

            success = true;
        } while (0);

        if (pwfx) CoTaskMemFree(pwfx);
        SAFE_RELEASE(pEnumerator);

        if (!success) {
            Close();
            return false;
        }
        return true;
    }

    bool Start() {
        if (_pClient) {
            return SUCCEEDED(_pClient->Start());
        }
        return false;
    }

    bool Stop() {
        if (_pClient) {
            _pClient->Stop();
        }
        return true;
    }

    void Close() {
        Stop();
        SAFE_RELEASE(_pCaptureClient);
        SAFE_RELEASE(_pRenderClient);
        SAFE_RELEASE(_pClient);
        SAFE_RELEASE(_pDevice);
    }

    // 获取当前可用的帧数
    UINT32 GetAvailableFrames() {
        UINT32 packetLength = 0;
        if (_isCapture) {
            _pCaptureClient->GetNextPacketSize(&packetLength);
        } else {
            UINT32 padding = 0;
            _pClient->GetCurrentPadding(&padding);
            UINT32 bufferSize = 0;
            _pClient->GetBufferSize(&bufferSize);
            packetLength = bufferSize - padding;
        }
        return packetLength;
    }

    // 读取数据 (Capture only)
    // 返回读取的采样数 (frames * channels)
    // 注意：这个函数会把数据转换为 Mono Int16 格式！
    // 如果 internal buffer 还有数据，会继续填充 outputBuffer
    int Read(std::vector<short>& outputBuffer, int maxFramesToRead) {
        if (!_pCaptureClient) return 0;

        BYTE* pData;
        UINT32 numFramesAvailable;
        DWORD flags;
        HRESULT hr;
        int totalFramesRead = 0;

        // 这里的 outputBuffer 是外部提供的，假设它足够大或者我们 append
        // 实际上为了配合 Speex，我们通常需要固定数量的帧
        
        while (totalFramesRead < maxFramesToRead) {
            hr = _pCaptureClient->GetNextPacketSize(&numFramesAvailable);
            if (FAILED(hr) || numFramesAvailable == 0) break;

            hr = _pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
            if (FAILED(hr)) break;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // 静音数据
                for (UINT32 i = 0; i < numFramesAvailable; ++i) {
                    outputBuffer.push_back(0);
                }
            } else {
                // 转换并复制数据
                // 假设是 Float32 Stereo 或 Mono
                ProcessAndAppendData(pData, numFramesAvailable, outputBuffer);
            }

            hr = _pCaptureClient->ReleaseBuffer(numFramesAvailable);
            totalFramesRead += numFramesAvailable;
        }
        return totalFramesRead;
    }

    // 写入数据 (Render only)
    // inputData: Mono Int16
    bool Write(const std::vector<short>& inputData) {
        if (!_pRenderClient) return false;

        UINT32 numFrames = inputData.size(); // 假设 Mono，所以 frames = samples
        BYTE* pData;
        HRESULT hr = _pRenderClient->GetBuffer(numFrames, &pData);
        if (FAILED(hr)) return false;

        // 将 Mono Int16 转换为设备格式 (通常是 Stereo Float32)
        ConvertToDeviceFormat(inputData, pData, numFrames);

        hr = _pRenderClient->ReleaseBuffer(numFrames, 0);
        return SUCCEEDED(hr);
    }

    int GetSampleRate() const { return _sampleRate; }

private:
    IMMDevice* _pDevice;
    IAudioClient* _pClient;
    IAudioCaptureClient* _pCaptureClient;
    IAudioRenderClient* _pRenderClient;
    
    bool _isCapture;
    bool _isLoopback;
    int _sampleRate;
    int _channels;
    int _bitsPerSample;
    bool _isFloat;
    int _targetSampleRate;

    // Helper: 从原始字节流转换为 Mono Int16
    void ProcessAndAppendData(BYTE* pData, UINT32 numFrames, std::vector<short>& out) {
        // 简单实现：如果是 Stereo，取平均；如果是 Float，转 Int16
        float* pFloat = (float*)pData;
        short* pShort = (short*)pData;

        for (UINT32 i = 0; i < numFrames; ++i) {
            float sampleVal = 0;
            if (_isFloat) {
                if (_channels == 1) {
                    sampleVal = pFloat[i];
                } else if (_channels == 2) {
                    sampleVal = (pFloat[i * 2] + pFloat[i * 2 + 1]) * 0.5f;
                }
            } else {
                // PCM 16bit
                if (_channels == 1) {
                    sampleVal = pShort[i] / 32768.0f;
                } else if (_channels == 2) {
                    sampleVal = (pShort[i * 2] + pShort[i * 2 + 1]) * 0.5f / 32768.0f;
                }
            }

            // Clamp and convert
            if (sampleVal > 1.0f) sampleVal = 1.0f;
            if (sampleVal < -1.0f) sampleVal = -1.0f;
            out.push_back((short)(sampleVal * 32767.0f));
        }
    }

    // Helper: Mono Int16 -> 设备格式 (通常 Float32 Stereo)
    void ConvertToDeviceFormat(const std::vector<short>& in, BYTE* pOut, UINT32 numFrames) {
        float* pFloatOut = (float*)pOut;
        short* pShortOut = (short*)pOut;

        for (UINT32 i = 0; i < numFrames; ++i) {
            short s = in[i];
            float f = s / 32768.0f;

            if (_isFloat) {
                if (_channels == 1) {
                    pFloatOut[i] = f;
                } else if (_channels == 2) {
                    pFloatOut[i * 2] = f;
                    pFloatOut[i * 2 + 1] = f;
                }
            } else {
                // PCM 16
                if (_channels == 1) {
                    pShortOut[i] = s;
                } else if (_channels == 2) {
                    pShortOut[i * 2] = s;
                    pShortOut[i * 2 + 1] = s;
                }
            }
        }
    }
};

class WavWriter {
public:
    WavWriter(const std::string& filename, int sampleRate, int channels, int bitsPerSample)
        : _sampleRate(sampleRate), _channels(channels), _bitsPerSample(bitsPerSample) {
        _file.open(filename, std::ios::binary);
        if (_file.is_open()) {
            WriteHeader();
        }
    }

    ~WavWriter() {
        Close();
    }

    void Close() {
        if (_file.is_open()) {
            UpdateHeader();
            _file.close();
        }
    }

    void Write(const short* data, size_t count) {
        if (!_file.is_open()) return;
        _file.write(reinterpret_cast<const char*>(data), count * sizeof(short));
        _dataSize += count * sizeof(short);
        
        // 定期更新 Header 和 Flush，防止异常退出导致文件损坏
        // 每写入约 128KB (约 1.3秒 @ 48k mono 16bit) 更新一次
        if (_dataSize - _lastUpdateSize > 128 * 1024) {
            UpdateHeader();
            _file.seekp(0, std::ios::end); // 回到文件末尾
            _lastUpdateSize = _dataSize;
        }
    }

    void Write(const std::vector<short>& data) {
        Write(data.data(), data.size());
    }

private:
    std::ofstream _file;
    int _sampleRate;
    int _channels;
    int _bitsPerSample;
    size_t _dataSize = 0;
    size_t _lastUpdateSize = 0;

    void WriteHeader() {
        _file.write("RIFF", 4);
        int32_t chunkSize = 36; // Placeholder
        _file.write(reinterpret_cast<char*>(&chunkSize), 4);
        _file.write("WAVE", 4);
        _file.write("fmt ", 4);
        int32_t subchunk1Size = 16;
        _file.write(reinterpret_cast<char*>(&subchunk1Size), 4);
        int16_t audioFormat = 1; // PCM
        _file.write(reinterpret_cast<char*>(&audioFormat), 2);
        int16_t numChannels = _channels;
        _file.write(reinterpret_cast<char*>(&numChannels), 2);
        int32_t sampleRate = _sampleRate;
        _file.write(reinterpret_cast<char*>(&sampleRate), 4);
        int32_t byteRate = _sampleRate * _channels * _bitsPerSample / 8;
        _file.write(reinterpret_cast<char*>(&byteRate), 4);
        int16_t blockAlign = _channels * _bitsPerSample / 8;
        _file.write(reinterpret_cast<char*>(&blockAlign), 2);
        int16_t bitsPerSample = _bitsPerSample;
        _file.write(reinterpret_cast<char*>(&bitsPerSample), 2);
        _file.write("data", 4);
        int32_t subchunk2Size = 0; // Placeholder
        _file.write(reinterpret_cast<char*>(&subchunk2Size), 4);
    }

    void UpdateHeader() {
        _file.seekp(4, std::ios::beg);
        int32_t chunkSize = 36 + _dataSize;
        _file.write(reinterpret_cast<char*>(&chunkSize), 4);
        
        _file.seekp(40, std::ios::beg);
        int32_t subchunk2Size = _dataSize;
        _file.write(reinterpret_cast<char*>(&subchunk2Size), 4);
    }
};
