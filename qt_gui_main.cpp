#define OFFLINE_AEC_LIBRARY
#include "main.cpp"

#include <QApplication>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QFont>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSlider>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>

class WavTrackWriter {
public:
    ~WavTrackWriter() { close(); }

    bool open(const std::filesystem::path& path) {
        close();
        stream.open(path, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) return false;
        writeHeader(0);
        stream.flush();
        return true;
    }

    void write(const short* samples, size_t count) {
        if (!stream.is_open() || !samples || count == 0) return;
        stream.write(reinterpret_cast<const char*>(samples),
                     static_cast<std::streamsize>(count * sizeof(short)));
        dataBytes += static_cast<uint32_t>(count * sizeof(short));
    }

    void close() {
        if (!stream.is_open()) return;
        stream.seekp(0, std::ios::beg);
        writeHeader(dataBytes);
        stream.close();
        dataBytes = 0;
    }

    void checkpoint() {
        if (!stream.is_open()) return;
        const std::streampos position = stream.tellp();
        stream.seekp(0, std::ios::beg);
        writeHeader(dataBytes);
        stream.seekp(position);
        stream.flush();
    }

private:
    template <typename T>
    void writeValue(T value) {
        stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void writeHeader(uint32_t bytes) {
        stream.write("RIFF", 4);
        writeValue<uint32_t>(36u + bytes);
        stream.write("WAVEfmt ", 8);
        writeValue<uint32_t>(16);
        writeValue<uint16_t>(1);
        writeValue<uint16_t>(1);
        writeValue<uint32_t>(48000);
        writeValue<uint32_t>(48000u * sizeof(short));
        writeValue<uint16_t>(sizeof(short));
        writeValue<uint16_t>(16);
        stream.write("data", 4);
        writeValue<uint32_t>(bytes);
    }

    std::ofstream stream;
    uint32_t dataBytes = 0;
};

class AsyncThreeTrackRecorder {
public:
    struct Frame {
        std::array<short, 480> mic{};
        std::array<short, 480> reference{};
        std::array<short, 480> output{};
    };

    explicit AsyncThreeTrackRecorder(const QString& directory)
        : directory(directory.toStdWString()), worker([this] { run(); }) {}

    ~AsyncThreeTrackRecorder() {
        running = false;
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }

    void setEnabled(bool value) {
        enabled = value;
        wake.notify_all();
    }

    void push(const std::vector<short>& mic, const std::vector<short>& reference,
              const std::vector<short>& output) {
        if (!enabled || mic.size() < 480 || reference.size() < 480 || output.size() < 480) return;
        Frame frame;
        std::copy_n(mic.begin(), 480, frame.mic.begin());
        std::copy_n(reference.begin(), 480, frame.reference.begin());
        std::copy_n(output.begin(), 480, frame.output.begin());
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (queue.size() >= 500) queue.pop_front();
            queue.push_back(std::move(frame));
        }
        wake.notify_one();
    }

private:
    void run() {
        while (true) {
            Frame frame;
            bool haveFrame = false;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                wake.wait_for(lock, std::chrono::milliseconds(100), [&] {
                    return !running || !queue.empty();
                });
                if (!running && queue.empty()) break;
                if (!queue.empty()) {
                    frame = std::move(queue.front());
                    queue.pop_front();
                    haveFrame = true;
                }
            }

            if (haveFrame) {
                if (!segmentOpen && !openSegment()) continue;
                if (samplesInSegment + 480 > samplesPerSegment) {
                    closeSegment();
                    if (!openSegment()) continue;
                }
                micWriter.write(frame.mic.data(), frame.mic.size());
                referenceWriter.write(frame.reference.data(), frame.reference.size());
                outputWriter.write(frame.output.data(), frame.output.size());
                samplesInSegment += 480;
                if (++framesSinceCheckpoint >= 100) {
                    micWriter.checkpoint();
                    referenceWriter.checkpoint();
                    outputWriter.checkpoint();
                    framesSinceCheckpoint = 0;
                }
            }

            bool queueEmpty = false;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                queueEmpty = queue.empty();
            }
            if ((!enabled && queueEmpty) || (!running && queueEmpty)) closeSegment();
        }
        closeSegment();
    }

    bool openSegment() {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return false;
        const std::wstring timestamp = QDateTime::currentDateTime()
                                           .toString("yyyyMMdd_HHmmss_zzz")
                                           .toStdWString();
        const std::wstring base = timestamp + L"_part" +
                                  std::to_wstring(++segmentNumber);
        const bool micOk = micWriter.open(directory / (base + L"_mic.wav"));
        const bool referenceOk = referenceWriter.open(directory / (base + L"_loopback.wav"));
        const bool outputOk = outputWriter.open(directory / (base + L"_output.wav"));
        segmentOpen = micOk && referenceOk && outputOk;
        if (!segmentOpen) closeSegment();
        samplesInSegment = 0;
        framesSinceCheckpoint = 0;
        return segmentOpen;
    }

    void closeSegment() {
        micWriter.close();
        referenceWriter.close();
        outputWriter.close();
        segmentOpen = false;
        samplesInSegment = 0;
        framesSinceCheckpoint = 0;
    }

    static constexpr uint64_t samplesPerSegment = 48000ull * 60ull * 10ull;
    std::filesystem::path directory;
    std::atomic<bool> running{true};
    std::atomic<bool> enabled{false};
    std::thread worker;
    std::mutex queueMutex;
    std::condition_variable wake;
    std::deque<Frame> queue;
    WavTrackWriter micWriter, referenceWriter, outputWriter;
    uint64_t samplesInSegment = 0;
    unsigned int segmentNumber = 0;
    unsigned int framesSinceCheckpoint = 0;
    bool segmentOpen = false;
};

class Engine {
public:
    ~Engine() { stop(); }
    bool running() const { return live; }
    double avg() const { return ms; }
    void copy(int index, std::vector<short>& output) {
        std::lock_guard<std::mutex> lock(mu);
        output = waves[index];
    }
    void setRecording(bool enabled) { recordingEnabled = enabled; }
    void start(QString micId, QString loopbackId, QString outputId, QString mode,
               double noiseGateThreshold, QString recordDirectory, bool record) {
        stop();
        recordingEnabled = record;
        live = true;
        worker = std::thread([=] {
            run(micId, loopbackId, outputId, mode, noiseGateThreshold, recordDirectory);
        });
    }
    void stop() {
        live = false;
        if (worker.joinable()) worker.join();
    }

private:
    void run(QString micId, QString loopbackId, QString outputId, QString mode,
             double noiseGateThreshold, QString recordDirectory) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        AudioStream micStream, loopbackStream, outputStream;
        if (!micStream.Open(micId.toStdWString(), true, false, 0) ||
            !loopbackStream.Open(loopbackId.toStdWString(), true, true, 0) ||
            !outputStream.Open(outputId.toStdWString(), false, false, 0) ||
            micStream.GetSampleRate() != 48000 || loopbackStream.GetSampleRate() != 48000 ||
            outputStream.GetSampleRate() != 48000) {
            live = false;
            CoUninitialize();
            return;
        }
        AudioProcessorWrapper processor(mode.toStdString(), 48000, 480, 9600,
                                        noiseGateThreshold);
        AsyncThreeTrackRecorder recorder(recordDirectory);
        std::deque<short> delay(480, 0);
        std::vector<short> mic, loopback, output(480), reference(480);
        double average = 0;
        while (live) {
            mic.clear();
            while (live && mic.size() < 480) {
                micStream.Read(mic, 480 - mic.size());
                if (mic.size() < 480) Sleep(1);
            }
            if (!live) break;
            mic.resize(480);
            loopback.clear();
            loopbackStream.Read(loopback, 480);
            loopback.resize(480, 0);
            delay.insert(delay.end(), loopback.begin(), loopback.end());
            for (int i = 0; i < 480; ++i) {
                reference[i] = delay.front();
                delay.pop_front();
            }
            const auto start = std::chrono::steady_clock::now();
            processor.Process(mic.data(), reference.data(), output.data());
            const double elapsed = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - start).count();
            average = average ? (0.98 * average + 0.02 * elapsed) : elapsed;
            ms = average;
            outputStream.Write(output);
            recorder.setEnabled(recordingEnabled);
            recorder.push(mic, reference, output);
            std::lock_guard<std::mutex> lock(mu);
            waves[0] = mic;
            waves[1] = reference;
            waves[2] = output;
        }
        CoUninitialize();
    }

    std::atomic<bool> live{false};
    std::atomic<double> ms{0};
    std::atomic<bool> recordingEnabled{false};
    std::thread worker;
    std::mutex mu;
    std::vector<short> waves[3];
};

class BackgroundWidget : public QWidget {
public:
    void setBackground(const QString& path, double configuredOpacity) {
        image = path.isEmpty() ? QImage() : QImage(path);
        if (configuredOpacity > 1.0) configuredOpacity /= 100.0;
        opacity = std::clamp(configuredOpacity, 0.0, 1.0);
        rebuildCache();
        update();
    }
    QSize sourceImageSize() const { return image.size(); }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        rebuildCache();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#101722"));
        if (image.isNull() || opacity <= 0.0) return;

        const qreal currentDpr = devicePixelRatioF();
        if (cachedLogicalSize != size() || std::abs(cachedDpr - currentDpr) > 0.01) {
            rebuildCache();
        }
        if (cache.isNull()) return;

        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setOpacity(opacity);
        painter.drawImage(QPointF(0, 0), cache);
    }

private:
    void rebuildCache() {
        cache = QImage();
        cachedLogicalSize = size();
        cachedDpr = devicePixelRatioF();
        if (image.isNull() || width() <= 0 || height() <= 0) return;

        const int targetWidth = std::max(1, static_cast<int>(std::ceil(width() * cachedDpr)));
        const int targetHeight = std::max(1, static_cast<int>(std::ceil(height() * cachedDpr)));
        const double targetRatio = static_cast<double>(targetWidth) / targetHeight;
        const double sourceRatio = static_cast<double>(image.width()) / image.height();

        QRect crop = image.rect();
        if (sourceRatio > targetRatio) {
            const int cropWidth = std::max(1, static_cast<int>(std::round(image.height() * targetRatio)));
            crop.setLeft((image.width() - cropWidth) / 2);
            crop.setWidth(cropWidth);
        } else if (sourceRatio < targetRatio) {
            const int cropHeight = std::max(1, static_cast<int>(std::round(image.width() / targetRatio)));
            crop.setTop((image.height() - cropHeight) / 2);
            crop.setHeight(cropHeight);
        }

        QImage working = image.copy(crop).convertToFormat(QImage::Format_ARGB32_Premultiplied);

        // Repeated 2x reductions act as a low-pass filter before the final
        // interpolation. This avoids aliasing when a large illustration is
        // reduced to a relatively small window.
        while (working.width() / 2 >= targetWidth && working.height() / 2 >= targetHeight) {
            working = working.scaled(working.width() / 2, working.height() / 2,
                                     Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        cache = working.scaled(targetWidth, targetHeight, Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation);
        cache.setDevicePixelRatio(cachedDpr);
    }

    QImage image;
    QImage cache;
    QSize cachedLogicalSize;
    qreal cachedDpr = 1.0;
    qreal opacity = 0.0;
};

class Wave : public QWidget {
public:
    Wave(Engine* engine, int index, QColor color)
        : engine(engine), index(index), color(color) {
        setMinimumHeight(104);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor(18, 27, 39, 118));
        const qreal center = height() / 2.0;
        painter.setPen(QPen(QColor("#2b3b4e"), 1));
        painter.drawLine(0, center, width(), center);
        painter.setPen(QPen(QColor("#223043"), 1, Qt::DashLine));
        painter.drawLine(0, height() * 0.25, width(), height() * 0.25);
        painter.drawLine(0, height() * 0.75, width(), height() * 0.75);

        std::vector<short> samples;
        engine->copy(index, samples);
        if (samples.empty()) return;
        int peak = 0;
        for (short sample : samples) peak = std::max(peak, std::abs(static_cast<int>(sample)));
        displayPeak = std::max(static_cast<double>(peak), displayPeak * 0.92);
        displayPeak = std::clamp(displayPeak, 384.0, 32768.0);
        const qreal scale = (center - 9.0) / displayPeak;

        QPainterPath path;
        const int pointCount = std::max(2, std::min(width(), static_cast<int>(samples.size())));
        for (int point = 0; point < pointCount; ++point) {
            const int sampleIndex = point * (samples.size() - 1) / (pointCount - 1);
            const qreal x = point * (width() - 1.0) / (pointCount - 1);
            const qreal y = std::clamp(center - samples[sampleIndex] * scale, 5.0, height() - 5.0);
            if (point == 0) path.moveTo(x, y); else path.lineTo(x, y);
        }
        painter.setPen(QPen(color, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
    }

private:
    Engine* engine;
    int index;
    QColor color;
    double displayPeak = 2048.0;
};

class Window : public QMainWindow {
public:
    Window() {
        setWindowTitle("Speex Echo Canceller");
        resize(960, 680);
        setMinimumSize(640, 520);
        loadStateIcons();
        setWindowIcon(stoppedIcon);

        background = new BackgroundWidget;
        setCentralWidget(background);
        auto* root = new QVBoxLayout(background);
        root->setContentsMargins(18, 16, 18, 16);
        root->setSpacing(9);

        mic = deviceBox(devices(true));
        reference = deviceBox(devices(false));
        output = deviceBox(devices(false));
        mode = new QComboBox;
        mode->addItems({"WebRTC AEC3", "SpeexDSP（完整）", "SpeexDSP（线性）",
                        "Speex 线性 + 后置降噪", "RealAEC"});
        mode->setCurrentIndex(3);
        noiseGate = new QSlider(Qt::Horizontal);
        noiseGate->setRange(-800, 0);
        noiseGate->setSingleStep(1);
        noiseGate->setPageStep(10);
        noiseGate->setValue(-800);
        noiseGate->setToolTip("10 ms 帧 RMS 阈值，范围 -80.0～0.0 dBFS，步进 0.1 dB");
        noiseGateValue = new QLabel("-80.0 dBFS");
        noiseGateValue->setObjectName("sliderValue");
        noiseGateValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        noiseGateValue->setMinimumWidth(96);
        auto* noiseGateControl = new QWidget;
        auto* noiseGateLayout = new QHBoxLayout(noiseGateControl);
        noiseGateLayout->setContentsMargins(0, 0, 0, 0);
        noiseGateLayout->setSpacing(8);
        noiseGateLayout->addWidget(noiseGate, 1);
        noiseGateLayout->addWidget(noiseGateValue);
        status = new QLabel("已停止");
        status->setObjectName("status");
        status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        auto* settings = new QGridLayout;
        settings->setHorizontalSpacing(16);
        settings->setVerticalSpacing(8);
        settings->setColumnStretch(0, 1);
        settings->setColumnStretch(1, 1);
        settings->addWidget(field("麦克风", mic), 0, 0);
        settings->addWidget(field("系统回环", reference), 0, 1);
        settings->addWidget(field("输出设备", output), 1, 0);
        settings->addWidget(field("处理模式", mode), 1, 1);
        settings->addWidget(field("声音阈值", noiseGateControl), 2, 0);
        settings->addWidget(field("当前状态", status), 2, 1);
        root->addLayout(settings);

        auto* controls = new QHBoxLayout;
        startButton = new QPushButton("启动引擎");
        stopButton = new QPushButton("停止引擎");
        stopButton->setEnabled(false);
        autoStartBox = new QCheckBox("开机自启动（后台）");
        recordBox = new QCheckBox("录音");
        controls->addWidget(startButton);
        controls->addWidget(stopButton);
        controls->addWidget(autoStartBox);
        controls->addWidget(recordBox);
        controls->addStretch();
        root->addLayout(controls);

        addWave(root, "麦克风", QColor("#4dabf7"));
        addWave(root, "回环参考", QColor("#ffd43b"));
        addWave(root, "AEC 输出", QColor("#51cf66"));
        loadConfig();

        connect(startButton, &QPushButton::clicked, this, [&] { startEngine(); });
        connect(stopButton, &QPushButton::clicked, this, [&] { stopEngine(); });
        gateApplyTimer = new QTimer(this);
        gateApplyTimer->setSingleShot(true);
        connect(noiseGate, &QSlider::valueChanged, this, [&](int value) {
            noiseGateValue->setText(QString("%1 dBFS").arg(value / 10.0, 0, 'f', 1));
            gateApplyTimer->start(350);
        });
        connect(gateApplyTimer, &QTimer::timeout, this, [&] {
            if (engine.running()) startEngine();
            else saveConfig(false);
        });

        trayStatus = new QAction("状态：已停止", this);
        trayStatus->setEnabled(false);
        auto* menu = new QMenu;
        menu->addAction(trayStatus);
        menu->addSeparator();
        menu->addAction("显示窗口", this, [&] { showNormal(); raise(); activateWindow(); });
        trayStop = menu->addAction("停止引擎", this, [&] { stopEngine(); });
        trayStop->setEnabled(false);
        trayAutoStart = menu->addAction("开机自启动（后台）");
        trayAutoStart->setCheckable(true);
        trayAutoStart->setChecked(autoStartBox->isChecked());
        trayRecord = menu->addAction("录音");
        trayRecord->setCheckable(true);
        trayRecord->setChecked(recordBox->isChecked());
        menu->addAction("关闭程序", qApp, &QApplication::quit);
        tray.setContextMenu(menu);
        tray.setToolTip("Speex Echo Canceller · 已停止");
        tray.setIcon(stoppedIcon);
        tray.show();
        connect(&tray, &QSystemTrayIcon::activated, this,
                [&](QSystemTrayIcon::ActivationReason reason) {
                    if (reason == QSystemTrayIcon::DoubleClick) showFromTray();
                });

        timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [&] { refreshUi(); });
        timer->start(60);
        windowSaveTimer = new QTimer(this);
        windowSaveTimer->setSingleShot(true);
        connect(windowSaveTimer, &QTimer::timeout, this, [&] {
            saveConfig(engine.running());
        });

        connect(autoStartBox, &QCheckBox::toggled, this, [&](bool enabled) {
            const QSignalBlocker blocker(trayAutoStart);
            trayAutoStart->setChecked(enabled);
            setAutoStartRegistration(enabled);
            saveConfig(engine.running());
        });
        connect(trayAutoStart, &QAction::toggled, this, [&](bool enabled) {
            const QSignalBlocker blocker(autoStartBox);
            autoStartBox->setChecked(enabled);
            setAutoStartRegistration(enabled);
            saveConfig(engine.running());
        });
        connect(recordBox, &QCheckBox::toggled, this, [&](bool enabled) {
            const QSignalBlocker blocker(trayRecord);
            trayRecord->setChecked(enabled);
            engine.setRecording(enabled);
            saveConfig(engine.running());
        });
        connect(trayRecord, &QAction::toggled, this, [&](bool enabled) {
            const QSignalBlocker blocker(recordBox);
            recordBox->setChecked(enabled);
            engine.setRecording(enabled);
            saveConfig(engine.running());
        });
        connect(qApp, &QCoreApplication::aboutToQuit, this, [&] {
            saveConfig(engine.running());
        });

        // Refreshes a stale Run entry if the executable was moved after setup.
        setAutoStartRegistration(autoStartBox->isChecked());
        QTimer::singleShot(0, this, [&] {
            layoutInitializationComplete = true;
            if (restoreEngineOnLaunch) startEngine();
        });
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        QMessageBox prompt(this);
        prompt.setWindowTitle("关闭程序");
        prompt.setIcon(QMessageBox::Question);
        prompt.setText("要彻底退出程序，还是缩小到系统托盘？");
        auto* minimizeButton = prompt.addButton("缩小到系统托盘", QMessageBox::AcceptRole);
        auto* exitButton = prompt.addButton("彻底退出", QMessageBox::DestructiveRole);
        auto* dismissButton = prompt.addButton("取消", QMessageBox::RejectRole);
        dismissButton->hide();
        prompt.setEscapeButton(dismissButton);
        prompt.setDefaultButton(qobject_cast<QPushButton*>(minimizeButton));
        prompt.exec();

        if (prompt.clickedButton() == minimizeButton) {
            hide();
            event->ignore();
        } else if (prompt.clickedButton() == exitButton) {
            event->accept();
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        } else {
            event->ignore();
        }
    }
    void resizeEvent(QResizeEvent* event) override {
        QMainWindow::resizeEvent(event);
        if (layoutInitializationComplete && isVisible() && !isMaximized() && !isFullScreen()) {
            userAdjustedWindowSize = true;
            if (windowSaveTimer) windowSaveTimer->start(500);
        }
    }

private:
    static QStringList modeKeys() {
        return {"webrtc", "speex", "speex_linear", "speex_linear_denoise", "real_aec"};
    }
    static std::string configPath() {
        return QDir(QCoreApplication::applicationDirPath()).filePath("config.ini").toStdString();
    }
    static QString recordDirectory() {
        return QDir(QCoreApplication::applicationDirPath()).filePath("record");
    }
    void showFromTray() {
        showNormal();
        raise();
        activateWindow();
    }
    void startEngine() {
        engine.start(mic->currentData().toString(), reference->currentData().toString(),
                     output->currentData().toString(), modeKeys()[mode->currentIndex()],
                     DbfsToNormalizedRms(noiseGateThresholdDbfs()),
                     recordDirectory(), recordBox->isChecked());
        saveConfig(true);
    }
    void stopEngine() {
        engine.stop();
        saveConfig(false);
    }
    void setAutoStartRegistration(bool enabled) {
        HKEY runKey = nullptr;
        const wchar_t* keyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_SET_VALUE,
                            nullptr, &runKey, nullptr) != ERROR_SUCCESS) {
            return;
        }
        const wchar_t* entryName = L"SpeexEchoCanceller";
        if (enabled) {
            const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
            const std::wstring command = QString("\"%1\" --background").arg(executable).toStdWString();
            RegSetValueExW(runKey, entryName, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(command.c_str()),
                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(runKey, entryName);
        }
        RegCloseKey(runKey);
    }
    QWidget* field(const QString& text, QWidget* control) {
        auto* container = new QWidget;
        auto* layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        auto* label = new QLabel(text);
        label->setObjectName("fieldLabel");
        layout->addWidget(label);
        layout->addWidget(control);
        return container;
    }
    void addWave(QVBoxLayout* root, const QString& text, const QColor& color) {
        auto* label = new QLabel(text);
        label->setObjectName("waveLabel");
        root->addWidget(label);
        auto* wave = new Wave(&engine, waveIndex++, color);
        root->addWidget(wave, 1);
        waveWidgets.push_back(wave);
    }
    QComboBox* deviceBox(const std::vector<AudioDeviceInfo>& items) {
        auto* box = new QComboBox;
        box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        for (const auto& item : items)
            box->addItem(QString::fromStdWString(item.name), QString::fromStdWString(item.id));
        return box;
    }
    std::vector<AudioDeviceInfo> devices(bool capture) {
        return WASAPIUtils::EnumerateDevices(capture);
    }
    static QIcon iconFromHalf(const QImage& sheet, bool rightHalf) {
        if (sheet.isNull()) return {};
        const int halfWidth = sheet.width() / 2;
        QImage source = sheet.copy(QRect(rightHalf ? halfWidth : 0, 0, halfWidth, sheet.height()));
        QRect opaque;
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                if (qAlpha(source.pixel(x, y)) > 8) opaque |= QRect(x, y, 1, 1);
        if (!opaque.isValid()) return {};
        const int padding = std::max(4, opaque.width() / 28);
        opaque.adjust(-padding, -padding, padding, padding);
        opaque = opaque.intersected(source.rect());
        return QIcon(QPixmap::fromImage(source.copy(opaque)));
    }
    void loadStateIcons() {
        const QImage sheet(QCoreApplication::applicationDirPath() + "/assets/tray_icon_states.png");
        stoppedIcon = iconFromHalf(sheet, false);
        runningIcon = iconFromHalf(sheet, true);
        if (stoppedIcon.isNull()) stoppedIcon = style()->standardIcon(QStyle::SP_MediaStop);
        if (runningIcon.isNull()) runningIcon = style()->standardIcon(QStyle::SP_MediaPlay);
    }
    void refreshUi() {
        const bool running = engine.running();
        QString stateText = running
            ? QString("运行中 · 平均 %1 ms / 10 ms").arg(engine.avg(), 0, 'f', 3)
            : "已停止";
        if (running && recordBox->isChecked()) stateText += " · 录音中";
        const QIcon& stateIcon = running ? runningIcon : stoppedIcon;
        startButton->setEnabled(!running);
        stopButton->setEnabled(running);
        trayStop->setEnabled(running);
        status->setText(stateText);
        trayStatus->setText("状态：" + stateText);
        tray.setToolTip("Speex Echo Canceller · " + stateText);
        tray.setIcon(stateIcon);
        setWindowIcon(stateIcon);
        for (Wave* wave : waveWidgets) wave->update();
    }
    void loadConfig() {
        AppConfig config;
        if (!LoadConfig(configPath(), config)) return;
        auto select = [](QComboBox* box, const std::wstring& id) {
            const int index = box->findData(QString::fromStdWString(id));
            if (index >= 0) box->setCurrentIndex(index);
        };
        select(mic, config.micDeviceId);
        select(reference, config.loopbackDeviceId);
        select(output, config.outputDeviceId);
        const int saved = modeKeys().indexOf(QString::fromStdString(config.aecType));
        mode->setCurrentIndex(saved < 0 ? 3 : saved);
        setNoiseGateThresholdDbfs(config.noiseGateThresholdDbfs);
        autoStartBox->setChecked(config.autoStart);
        recordBox->setChecked(config.recordingEnabled);
        engine.setRecording(config.recordingEnabled);
        restoreEngineOnLaunch = config.engineWasRunning;
        backgroundImageSetting = config.backgroundImage;
        backgroundOpacitySetting = config.backgroundOpacity;
        QString imagePath = QString::fromStdWString(backgroundImageSetting).trimmed();
        if (!imagePath.isEmpty() && QDir::isRelativePath(imagePath)) {
            imagePath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(imagePath);
        }
        background->setBackground(imagePath, backgroundOpacitySetting);
        savedWindowWidth = config.windowWidth;
        savedWindowHeight = config.windowHeight;
        if (savedWindowWidth > 0 && savedWindowHeight > 0) {
            restoreSavedWindowSize();
        } else {
            adaptWindowToBackground(background->sourceImageSize());
        }
    }
    void saveConfig(bool engineRunning) {
        AppConfig config;
        config.micDeviceId = mic->currentData().toString().toStdWString();
        config.loopbackDeviceId = reference->currentData().toString().toStdWString();
        config.outputDeviceId = output->currentData().toString().toStdWString();
        config.aecType = modeKeys()[mode->currentIndex()].toStdString();
        config.autoStart = autoStartBox->isChecked();
        config.engineWasRunning = engineRunning;
        config.backgroundImage = backgroundImageSetting;
        config.backgroundOpacity = backgroundOpacitySetting;
        if (userAdjustedWindowSize && !isMaximized() && !isFullScreen()) {
            savedWindowWidth = width();
            savedWindowHeight = height();
        }
        config.windowWidth = savedWindowWidth;
        config.windowHeight = savedWindowHeight;
        config.recordingEnabled = recordBox->isChecked();
        config.noiseGateThresholdDbfs = noiseGateThresholdDbfs();
        SaveConfig(configPath(), config);
    }
    double noiseGateThresholdDbfs() const {
        return noiseGate->value() / 10.0;
    }
    void setNoiseGateThresholdDbfs(double value) {
        const int sliderValue = static_cast<int>(std::round(std::clamp(value, -80.0, 0.0) * 10.0));
        noiseGate->setValue(sliderValue);
        noiseGateValue->setText(QString("%1 dBFS").arg(sliderValue / 10.0, 0, 'f', 1));
    }
    void restoreSavedWindowSize() {
        QScreen* screen = QGuiApplication::primaryScreen();
        if (!screen) return;
        const QRect available = screen->availableGeometry();
        resize(std::clamp(savedWindowWidth, minimumWidth(), available.width()),
               std::clamp(savedWindowHeight, minimumHeight(), available.height()));
    }
    void adaptWindowToBackground(const QSize& imageSize) {
        if (!imageSize.isValid() || imageSize.height() <= 0) return;

        const int minWidth = minimumWidth();
        const int minHeight = minimumHeight();
        QScreen* screen = QGuiApplication::primaryScreen();
        if (!screen) return;
        const QRect available = screen->availableGeometry();
        const int maxWidth = std::max(minWidth,
            std::min(1400, static_cast<int>(available.width() * 0.90)));
        const int maxHeight = std::max(minHeight,
            std::min(950, static_cast<int>(available.height() * 0.90)));
        const double imageRatio = static_cast<double>(imageSize.width()) / imageSize.height();
        const double safeRatio = std::clamp(imageRatio, 0.30, 3.00);

        int targetHeight = std::clamp(780, minHeight, maxHeight);
        int targetWidth = static_cast<int>(std::round(targetHeight * safeRatio));
        if (targetWidth < minWidth) {
            targetWidth = minWidth;
            targetHeight = static_cast<int>(std::round(targetWidth / safeRatio));
        }
        if (targetWidth > maxWidth) {
            targetWidth = maxWidth;
            targetHeight = static_cast<int>(std::round(targetWidth / safeRatio));
        }
        if (targetHeight > maxHeight) {
            targetHeight = maxHeight;
            targetWidth = static_cast<int>(std::round(targetHeight * safeRatio));
        }
        resize(std::clamp(targetWidth, minWidth, maxWidth),
               std::clamp(targetHeight, minHeight, maxHeight));
    }

    Engine engine;
    BackgroundWidget* background = nullptr;
    QComboBox *mic = nullptr, *reference = nullptr, *output = nullptr, *mode = nullptr;
    QSlider* noiseGate = nullptr;
    QLabel* noiseGateValue = nullptr;
    QPushButton *startButton = nullptr, *stopButton = nullptr;
    QCheckBox *autoStartBox = nullptr, *recordBox = nullptr;
    QLabel* status = nullptr;
    QTimer* timer = nullptr;
    QTimer* windowSaveTimer = nullptr;
    QTimer* gateApplyTimer = nullptr;
    QSystemTrayIcon tray;
    QAction *trayStatus = nullptr, *trayStop = nullptr, *trayAutoStart = nullptr,
            *trayRecord = nullptr;
    QIcon stoppedIcon, runningIcon;
    std::vector<Wave*> waveWidgets;
    int waveIndex = 0;
    bool restoreEngineOnLaunch = false;
    std::wstring backgroundImageSetting;
    double backgroundOpacitySetting = 0.0;
    int savedWindowWidth = 0;
    int savedWindowHeight = 0;
    bool layoutInitializationComplete = false;
    bool userAdjustedWindowSize = false;
};

int main(int argc, char** argv) {
    CoInitialize(nullptr);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setFont(QFont("Microsoft YaHei UI", 10));

    const wchar_t* mutexName = L"Local\\SpeexEchoCanceller.Gui.SingleInstance";
    HANDLE singleInstance = CreateMutexW(nullptr, FALSE, mutexName);
    if (!singleInstance) {
        QMessageBox::critical(nullptr, "启动失败", "无法创建程序单实例锁。程序将退出。");
        CoUninitialize();
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(singleInstance);
        QMessageBox::information(nullptr, "程序已在运行",
                                 "Speex Echo Canceller 已经在运行，请在任务栏右下角查看。 ");
        CoUninitialize();
        return 0;
    }
    app.setStyleSheet(R"(
        QWidget { color: #e7f0f8; font-family: 'Microsoft YaHei UI', 'Microsoft YaHei'; font-size: 14px; }
        QMainWindow { background: #101722; }
        QLabel { background: transparent; }
        QLabel#fieldLabel, QLabel#waveLabel { color: rgba(224, 237, 248, 220); font-size: 12px; font-weight: 600; }
        QLabel#waveLabel { padding-top: 1px; }
        QLabel#status { color: #bcecff; background: rgba(12, 22, 33, 92); border-radius: 6px; padding: 5px 9px; font-weight: 600; }
        QCheckBox { background: rgba(12, 22, 33, 62); border-radius: 6px; padding: 6px 9px; }
        QComboBox { background: rgba(20, 31, 44, 122); border: 1px solid rgba(145, 187, 222, 135); border-radius: 7px; padding: 7px 9px; }
        QComboBox:hover { background: rgba(27, 44, 60, 155); border-color: rgba(157, 210, 245, 215); }
        QLabel#sliderValue { color: #bcecff; background: rgba(12, 22, 33, 92); border-radius: 6px; padding: 5px 7px; font-family: 'Microsoft YaHei UI', 'Microsoft YaHei'; }
        QSlider::groove:horizontal { height: 7px; background: rgba(20, 31, 44, 150); border: 1px solid rgba(145, 187, 222, 100); border-radius: 4px; }
        QSlider::sub-page:horizontal { background: rgba(42, 165, 220, 205); border-radius: 4px; }
        QSlider::handle:horizontal { width: 18px; margin: -6px 0; background: #bcecff; border: 2px solid #2388bb; border-radius: 9px; }
        QSlider::handle:horizontal:hover { background: #ffffff; border-color: #39b7ef; }
        QComboBox QAbstractItemView { background: rgba(18, 29, 42, 218); selection-background-color: rgba(28, 116, 165, 205); }
        QPushButton { background: rgba(15, 119, 181, 158); border: 1px solid rgba(145, 216, 250, 110); border-radius: 7px; padding: 9px 20px; font-weight: 600; }
        QPushButton:hover { background: rgba(22, 143, 207, 190); }
        QPushButton:disabled { background: rgba(38, 49, 64, 105); color: rgba(190, 204, 216, 185); border-color: rgba(130, 150, 170, 55); }
        QMenu { background: rgba(15, 24, 35, 215); border: 1px solid rgba(110, 150, 185, 175); padding: 5px; }
        QMenu::item { padding: 7px 28px 7px 12px; border-radius: 4px; }
        QMenu::item:selected { background: #1c6895; }
    )");
    Window window;
    if (!app.arguments().contains("--background")) window.show();
    const int result = app.exec();
    CloseHandle(singleInstance);
    CoUninitialize();
    return result;
}
