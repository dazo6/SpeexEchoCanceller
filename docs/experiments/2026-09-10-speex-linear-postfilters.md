# Speex Linear 后置神经处理实验记录

日期：2026-09-10  
状态：实验结束，不纳入正式产品

## 目的

验证下列方案作为 `Speex Linear` 后置处理器时的降噪效果、声音损失、处理耗时和原生集成可行性：

- RNNoise
- DeepFilterNet
- 深度残余回声抑制（RES，候选模型 DTLN-AEC）

实验不修改 GUI、正式音频链、发布包或默认构建。测试产生的模型、第三方仓库、虚拟环境、可执行文件和 WAV 均在记录完成后删除。

## 测试环境与输入

- Windows x64
- AMD64 Family 25，12 个逻辑处理器
- MinGW 13.1
- 48 kHz、单声道、PCM16
- 输入：项目实录经过 Speex Linear 后的约 23.29 秒音频
- 基线每帧：480 samples，即 10 ms
- Speex Linear 已测耗时：约 0.0696 ms/帧（包含当时的后置 Gate 测量开销）

由于实录中没有独立的干净近端语音真值，本实验不能计算严格的 PESQ、STOI 或真实 SNR 改善。质量结论使用整体电平、分帧能量、波形相关性、静音帧比例以及试听进行代理评估。

## 测试版本

### RNNoise

- 来源：[xiph/rnnoise](https://github.com/xiph/rnnoise)
- Git 提交：`70f1d256acd4b34a572f999a05c87bf00b67730d`
- 模型 SHA-256：`0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37`
- 构建：原生 C，`-O3 -DNDEBUG -march=native`
- 处理方式：逐个 480-sample 帧调用 `rnnoise_process_frame()`
- Python 运行时：不需要

### DeepFilterNet

- 来源：[Rikorose/DeepFilterNet](https://github.com/Rikorose/DeepFilterNet)
- 版本：`v0.5.6`
- 程序：官方 `deep-filter-0.5.6-x86_64-pc-windows-msvc.exe`
- 后端：原生 Rust/libDF，DeepFilterNet3 内置模型
- Python 运行时：不需要
- 日志报告模型 lookahead 为 2 帧
- 48 kHz 下 FFT 960 samples、hop 480 samples，总算法延迟约为：

```text
(FFT - hop) + lookahead × hop
= (960 - 480) + 2 × 480
= 1440 samples
= 30 ms
```

离线测试使用 `-D` 裁掉输出起始延迟；实时接入时不能消除这 30 ms 算法延迟。

### 深度 RES / DTLN-AEC

- 来源：[breizhn/DTLN-aec](https://github.com/breizhn/DTLN-aec)
- Git 提交：`9d24e128b4f409db18227b8babb343016625921f`
- 模型格式：TensorFlow Lite
- 模型采样率：16 kHz
- 官方仓库测试入口依赖 Python/TensorFlow，没有提供可直接用于当前 MinGW C++ 项目的原生基准程序

根据“测试方案必须能以原生库接入正式软件”的约束，本轮没有安装 Python 依赖，也没有运行 DTLN-AEC，因此不提供推测性性能或质量数据。

## 性能结果

### RNNoise

连续运行 7 轮，每轮 2329 帧；只统计 `rnnoise_process_frame()`，不含文件 I/O：

| 指标 | 结果 |
|---|---:|
| 平均耗时/10 ms 帧 | 0.373 ms |
| 各轮平均值的中位数 | 0.372 ms |
| 典型 P95 | 0.543 ms |
| 观测最大单帧 | 7.868 ms |
| RNNoise 实时因子 | 0.0373 |
| Speex Linear + RNNoise 总耗时 | 约 0.443 ms/帧 |
| 总链路 10 ms 预算占用 | 约 4.43% |
| 实测输出延迟 | 约 20 ms |

RNNoise 本身的 CPU 性能足够用于实时处理，但默认完整模型使原生测试程序增加约 14.9 MB。

### DeepFilterNet3

使用官方原生程序预热后连续运行 7 轮。官方程序只提供整段 RTF，不提供单帧 P95/最大值：

| 指标 | 结果 |
|---|---:|
| 平均 RTF | 0.1131 |
| RTF 中位数 | 0.1058 |
| 等效平均耗时/10 ms | 1.131 ms |
| 等效中位耗时/10 ms | 1.058 ms |
| Speex Linear + DeepFilterNet 平均总耗时 | 约 1.201 ms/帧 |
| 总链路 10 ms 预算占用 | 约 12.0% |
| 算法延迟 | 约 30 ms |
| 官方原生程序大小 | 约 26.9 MB |

CPU 性能可以达到实时要求，但开销约为 RNNoise 的 3 倍，并引入额外 30 ms 算法延迟。

## 降噪与声音损失结果

Speex Linear 基线：

- 整体 RMS：`-42.721 dBFS`
- 峰值：`-10.367 dBFS`
- 完全静音帧：`0.043%`

分段代理指标定义：

- “低能量 30%”：按 Speex Linear 输入帧 RMS 排序后最低的 30%，用于观察安静/底噪段
- “高能量 30%”：最高的 30%，用于观察主要声音活动段
- “高能量相关性”：延迟对齐后，处理前后高能量帧的波形相关系数；越接近 1，波形改变越小

| 方案 | 整体电平变化 | 低能量 30% 变化 | 高能量 30% 变化 | 高能量相关性 | 完全静音帧 |
|---|---:|---:|---:|---:|---:|
| RNNoise | -10.24 dB | -11.78 dB | -10.66 dB | 0.460 | 19.85% |
| DeepFilterNet 默认 | -32.62 dB | -33.97 dB | -32.59 dB | 0.238 | 46.00% |
| DeepFilterNet 最大衰减 12 dB | -11.86 dB | -11.98 dB | -11.86 dB | 0.998 | 0.13% |
| DeepFilterNet 最大衰减 20 dB | -19.46 dB | -19.72 dB | -19.45 dB | 0.981 | 0.13% |

RNNoise 自身的 VAD 将约 `65.5%` 的输入帧判为语音，但输出仍将约 `19.85%` 的帧变成完全静音。低能量和高能量区域的衰减只相差约 1.1 dB，说明当前模型没有仅压制底噪，而是明显损伤了有效声音。

DeepFilterNet 默认参数在该样本上过度抑制，近一半帧被静音。设置 12 dB 或 20 dB 最大衰减可以保住波形，但低能量和高能量区域几乎等量下降；结果主要是把干声按固定比例混回，并没有形成理想的选择性降噪。

噪声抑制器不读取 loopback 参考，因此本实验不把相关性降低直接宣称为回声消除改善。后处理器自身的延迟也会影响未经补偿的远端投影指标。

## 结论

1. **RNNoise 暂不接入。** 性能余量充足，但当前官方模型在这段真实 Speex Linear 输出上使有效声音下降约 10.7 dB，波形相关性低，并制造大量完全静音帧。
2. **DeepFilterNet 暂不接入。** 默认模式过度抑制严重；限制衰减后主要表现为整体降音量。其约 12% 的实时预算占用和约 30 ms 算法延迟也明显高于 RNNoise。
3. **深度 RES 暂不测试、不接入。** 后续只有在建立不依赖 Python 的 TensorFlow Lite C++ 实时基准链后才重新评估。
4. 当前正式产品继续使用既有 `Speex Linear + 后置降噪 + dBFS Gate` 方案；本次实验不改变产品代码、构建或发布产物。
