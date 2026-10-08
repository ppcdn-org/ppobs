# AudioShift 实时变声设计方案

> **说明**：本功能是 ppobs 的原创新增，不是从 obsvlive 移植——调研确认 obsvlive 及其
> `feature/virtual-gamehost` 分支均未包含任何变声/音高处理代码，本仓库 `deps/` 与
> `plugins/obs-filters/` 下也没有现成的音高变换（pitch shift）或声码器依赖可复用。
> 本文档随 `feature/audioshift` 分支创建，在编码开始前编写，兼作实现期间的参照和后续
> PR 说明，结构沿用 `docs/virtual-background-design.md` 的既有体例。

## 1. 目标

为 ppobs 增加本地实时变声能力：以标准 OBS **音频滤镜**的形式挂载到任意音频源（麦克风、
桌面音频、媒体源等），在不改变语速的前提下改变音高，使主播的声音呈现为预设或自定义的
另一种音色（男声/女声互换、童声、低沉、机器人等娱乐向效果），再经由 OBS 原有的混音与
推流链路输出。

目标使用方式：

```text
麦克风源 -> AudioShift 变声滤镜 -> OBS 混音 -> 推流 / 录制
```

本方案采用可自部署的开源 DSP 组件，不调用云端语音转换服务，不上传语音数据，不依赖
商业 SDK。

## 2. 范围与非目标

### 2.1 第一版（P0）范围

- Windows x64
- 独立音高变换（pitch shift），时长（tempo）不变
- 可挂载到任意音频源（麦克风、桌面音频、视频/媒体源的音轨等）
- 滑块手动调节半音数（-12 ~ +12 半音），以及若干按半音值预设的"快捷选项"
- 支持单声道与双声道（立体声）音频
- 跟随 OBS 混音实际采样率，不强制重采样
- 中英文配置界面
- 处理失败或未配置依赖时自动直通（passthrough），不得使音频静音或崩溃

### 2.2 第一版不包含（P1 / P2 展望）

- **保留共振峰的"自然"变声**（即让男声听起来像真实女声而不只是"被调高的男声"）：
  需要独立的共振峰（formant）校正算法，`SoundTouch`（见第 3 节）不提供该能力。P0 的
  预设是"角色化"效果（机器人、卡通、低沉大叔等娱乐效果），不承诺性别转换的自然度。
- **机器人 / 回声等附加效果层**：P0 只做纯音高变换；环形调制（机器人音）、延迟反馈
  （空灵/山洞）等属于叠加效果，放入 P1（见 6.2 节），避免 P0 范围扩散。
- **"温柔男中音""甜美女高音"一类情感化声音模板**：这类模板需要音高 + EQ + 压缩的
  组合而不是单一滤镜能给的，且涉及前端批量应用滤镜链的改动，放入 P1（见 6.4 节）。
- **基于神经网络的音色转换**（RVC / so-vits-svc 一类的 any-to-any 音色克隆）：延迟、
  CPU/GPU 占用、模型许可证复杂度都远超 P0 可接受范围，列为 P2 研究方向，不承诺交付。
- 云端变声或在线账号服务
- 对非人声音频（游戏音效、背景音乐）做专门优化——用户若把滤镜挂到非人声源上，效果
  不保证好听，但不应崩溃或产生超出预期的噪声/爆音
- 复用或改动现有 `noise-suppress-filter.c`、`compressor-filter.c` 等滤镜的降噪/动态
  处理逻辑——AudioShift 只做音高变换，用户可按需在滤镜链中叠加现有滤镜

## 3. 推荐开源技术栈

### 3.1 推荐组合

| 组件 | 推荐方案 | 许可证/用途 |
|---|---|---|
| 音高变换算法 | **SoundTouch**（Olli Parviainen） | **LGPL-2.1**；基于 WSOLA 的时域音高/速度独立变换，成熟、延迟可控、CPU 占用低 |
| 声道交织/反交织 | 自写 helper（规模很小，放在滤镜自身文件里） | 复用项目现有代码风格，不引入额外依赖 |
| 附加效果（P1） | 自写环形调制 / 延迟反馈 | 实现简单，不需要第三方库 |

### 3.2 为什么选 SoundTouch，而不是其他方案

- **Rubber Band Library**：音质与共振峰保留能力更强，但采用 GPL 与商业双授权——若
  ppobs 以非完全 GPL 的方式商业分发，需要向 Particular Programs Ltd. 购买商业授权，
  引入额外的许可证与采购流程。作为 P1 的候选（若决定投入"自然变声"方向）保留，P0 不用。
- **WORLD vocoder / 神经声码器类方案**：音质潜力最高，但计算量大、延迟高，且训练/推理
  模型涉及的数据与权重许可证核验成本高，不适合 P0 的实时语音场景，列入 P2。
- **自研 STFT/phase-vocoder**：零第三方依赖、许可证最干净，但音高变换是一个容易"听起来
  对但细节全错"的领域（相位相干性、瞬态模糊、金属声伪影），重新实现并调到可用质量的
  工程成本显著高于直接使用一个有 20+ 年历史、已被 Audacity/VLC/foobar2000 等项目验证
  过的库。不作为 P0 默认选择，但作为"若 SoundTouch 许可证评审不通过"时的退路记录在案。
- **SoundTouch 的局限**：只做音高/速度的时域变换，**不保留共振峰**——这正是 P0 明确
  只做"角色化"预设、不承诺"自然性别转换"的原因（见 2.2）。

### 3.3 许可证核验要求

参照 `virtual-background-design.md` §3.6 确立的纪律，正式发布前必须完成：

- 确认当前 SoundTouch 官方源码仓库地址与最新稳定版本号——**已核实**：源码仓库现已
  从历史上的 `surina.net/soundtouch` 迁移到
  `https://codeberg.org/soundtouch/soundtouch`，最新稳定版本 **2.4.1**。
- 记录所用版本号、下载来源、SHA-256——**已完成**（经用户明确同意后下载，见下方
  §3.4）：实际 vendor 的是 git 提交
  `0047e0b1ecfceb041348579119bf79b73a322a3a`（"Bump version to 2.4.1"，
  2026-03-29），标签 `2.4.1` 指向同一提交。provenance 记录见
  `plugins/obs-filters/soundtouch/VENDORED-FROM.md`。
- LGPL-2.1 义务：本项目选择**把 SoundTouch 完整源码 vendor 进仓库，以对象库形式
  静态编译进 `obs-filters.dll`**，而不是动态链接一份预编译二进制——这比"动态链接 +
  随包附带许可证文本"的路径更直接地满足 LGPL-2.1 的对应源码（corresponding source）
  义务，因为完整源码本身就在这个开源仓库里，使用方天然就能获取、修改、重新编译。
  `COPYING.TXT`（LGPL-2.1 全文）随 vendored 目录一并保留在
  `plugins/obs-filters/soundtouch/COPYING.TXT`。
- Windows 平台没有预编译 SoundTouch 包随 `obs-deps` 提供，因此 `cmake/soundtouch.cmake`
  采用"先尝试 `find_package`（极少数机器可能已装有系统包），找不到则构建 vendored
  源码"的两级策略——这与 `rnnoise.cmake` 的结构完全一致，而不再是上一版文档记载的
  "只有可选检测、没有 vendor 兜底"（那时还没拿到下载许可）。

### 3.4 实际下载与裁剪记录

- 来源：`git clone --depth 1 --branch 2.4.1 https://codeberg.org/soundtouch/soundtouch.git`
- 只 vendor 了 SoundTouch 库本身的源码，**排除**了：
  - `source/SoundStretch`（命令行工具）、`source/SoundTouchDLL`（C 包装 DLL，
    AudioShift 直接从 `.cpp` 调 C++ API，不需要它）、`source/Android-lib`、
    `source/csharp-example`
  - 上游自带的构建系统文件（`CMakeLists.txt`、autotools 相关文件、`.sln`/`.vcxproj`）
    ——改用本项目自己的 `cmake/soundtouch.cmake`，镜像 `rnnoise.cmake` 的手写
    `target_sources` 对象库模式，而不是 `add_subdirectory` 整个上游项目（避免引入
    上游的 `install()`/pkgconfig/包配置生成规则污染本项目的打包流程）
  - `BPMDetect.cpp`/`BPMDetect.h`（节拍检测，AudioShift 不用；已核实
    `SoundTouch.cpp`、`TDStretch.cpp`、`RateTransposer.cpp` 均不引用它，裁剪理由
    记在 `VENDORED-FROM.md`）
- 保留的文件清单、裁剪理由、更新到新版本的操作方式，均记录在
  `plugins/obs-filters/soundtouch/VENDORED-FROM.md`，不在本文档重复。

## 4. 总体架构

### 4.1 滤镜类型：同步音频滤镜

注册为 `OBS_SOURCE_TYPE_FILTER` + `OBS_SOURCE_AUDIO`，只实现 `filter_audio`，不涉及
`OBS_SOURCE_ASYNC`/视频相关标记——这与 `gain-filter.c`/`compressor-filter.c`/
`noise-suppress-filter.c` 等现有音频滤镜完全一致，天然可挂载到任意带音频的源（麦克风、
媒体源、桌面音频捕获等），不存在虚拟背景滤镜当年需要规避的"异步滤镜只能挂异步源"限制
（那是视频滤镜特有的问题，音频滤镜本身没有这个兼容性坑）。

### 4.2 与 SoundTouch 的桥接层

SoundTouch 是 C++ 库（核心类 `soundtouch::SoundTouch`），而 `obs-filters` 现有文件均为
纯 C。参照 `matting-ort.c` 为 ONNX Runtime C API 做薄封装的思路，新增一个小的 C++
翻译单元做桥接：

```text
plugins/obs-filters/audioshift-soundtouch.cpp   extern "C" 封装 SoundTouch C++ API
plugins/obs-filters/audioshift-soundtouch.h     C 头文件，供 .c 文件 #include
```

封装面很窄，只需要：创建/销毁实例、`setSampleRate`/`setChannels`、
`setPitchSemiTones`（并固定 `setTempo(1.0)` 保证时长不变）、`putSamples`/
`receiveSamples`、`flush`/`clear`（切换预设或滤镜被禁用再启用时清空内部状态，避免
残留样本污染新一段音频）。

**已验证**：`obs-filters` target 内混合编译 `.c`/`.cpp` 没有问题——`cmake --build
--target obs-filters` 成功编译了 `audioshift-filter.c`、`audioshift-soundtouch.cpp`
以及 vendored 的 13 个 SoundTouch `.cpp` 源文件（通过独立的 `obs-soundtouch` 对象库
target，见 §12.1），链接产出 `obs-filters.dll`，`.c` 文件里对 `audioshift-soundtouch.h`
声明的 `extern "C"` 函数的调用也编译链接通过，没有报错或警告。

### 4.3 声道与采样率

- 采样率：直接调用 `audio_output_get_sample_rate(obs_get_audio())`，**不强制重采样**
  ——与 `noise-suppress-filter.c`（固定要求 48kHz，需要 `audio-resampler` 做前后转换）
  不同，SoundTouch 原生支持任意采样率，这一点简化了实现。
- 声道：`audio_output_get_channels(obs_get_audio())` 决定声道数，交给同一个
  `SoundTouch` 实例按 `setChannels(N)` 处理（而不是每声道各开一个实例），以保持立体声
  声道间的相位/时序一致性，避免分声道独立处理导致的声像漂移。
- OBS 的 `obs_audio_data->data[c]` 是**按声道分离的 planar float32**，SoundTouch 的
  `putSamples`/`receiveSamples` 要求**交织（interleaved）**样本，因此需要在
  `filter_audio` 中来回转换：
  ```text
  输入：audio->data[c][i]（planar）  -> interleave 到 scratch 缓冲区
  SoundTouch 处理（interleaved in/out）
  输出：scratch 缓冲区 -> deinterleave 回滤镜自带的 per-channel 输出缓冲
  ```
  scratch 缓冲区按 `frames * channels` 大小、在 `update()`/声道数变化时按需
  `realloc`，不在稳态的 `filter_audio` 路径里分配内存（与 `compressor-filter.c` 的
  `resize_env_buffer` 思路一致）。

## 5. 实时处理管线

### 5.1 线程模型：无需独立 worker 线程，也不需要 mutex

与虚拟背景滤镜（ONNX 神经网络推理，单帧耗时数十毫秒，必须放到独立 worker 线程避免
阻塞 OBS 渲染/音频线程）不同，SoundTouch 的 WSOLA 时域算法对典型语音帧（十毫秒级）的
处理成本极低（预计落在微秒到低个位数毫秒级，需在阶段 B 实测确认），可以**直接在
`filter_audio` 回调内同步处理**，不引入额外线程——这与 `gain-filter.c`/
`compressor-filter.c` 的同步处理模型一致。

**`soundtouch::SoundTouch` 实例只由音频线程触碰**，不需要 mutex：

- `update()`（UI 线程调用）**不直接操作** SoundTouch 实例，只把目标值写入滤镜结构体里
  几个普通字段（`pending_semitones`、`pending_sample_rate`、`pending_channels`）——这几个
  都是简单标量，和 `gain-filter.c`/`compressor-filter.c` 里 `update()` 直接写
  `multiple`/`ratio`/`threshold` 等字段一样，允许偶尔的"读到一半"撞车，后果最多是晚一次
  回调才生效，不是内存安全问题。
- `filter_audio()`（音频线程）每次调用先比较 `pending_*` 与"已生效"值：半音值变化直接
  调用 `setPitchSemiTones()`；采样率或声道数变化则需要重建 SoundTouch 实例并清空所有
  deque（视为新的一段音频，做法同 `noise-suppress-filter.c` 的 `reset_data`）。
  由于创建/销毁/设置/处理全部只发生在音频线程内部，不存在跨线程争用同一个 C++ 对象的
  场景，不需要 `compressor-filter.c` 那种为 `weak_sidechain`（一个会被两个线程交替
  写入/读取的指针）准备的 mutex——AudioShift 没有类似的跨线程指针。

### 5.2 缓冲与延迟补偿

SoundTouch 内部维护 FIFO，`putSamples`/`receiveSamples` 不保证每次调用输入输出帧数
一一对应（尤其是算法启动初期，内部缓冲尚未攒够数据时 `receiveSamples` 可能返回 0）。
处理方式参照 `noise-suppress-filter.c` 的"攒够才吐，否则延后"模式：

```text
filter_audio(audio):
  1. 若 pending_sample_rate/pending_channels 变化：重建 SoundTouch 实例，清空全部 deque
  2. 若 pending_semitones 变化：st_set_pitch_semitones(st, pending_semitones)
  3. 把 {audio->frames, audio->timestamp} 推入 info_buffer（同 noise-suppress 的用法）
  4. interleave audio->data[*][0..frames) 到 scratch
  5. st_put_samples(st, scratch, frames)；samples_fed += frames
  6. 循环 st_receive_samples 把可取到的输出帧 deinterleave 后追加进每声道的
     output_buffers deque；每次成功取出 N 帧，samples_emitted += N
  7. 查看 info_buffer 队首记录的期望帧数；若 output_buffers[0] 里攒的字节数不够：
     返回 NULL（按 filter_audio 文档允许的"推迟输出"语义），等待后续调用补足
  8. 否则弹出 info_buffer 队首记录，从 output_buffers 头部取出对应帧数，deinterleave
     回滤镜自带的 struct obs_audio_data 输出对象；timestamp 回退量
     = (samples_fed - samples_emitted) / sample_rate 换算成纳秒
     （即"已经喂给 SoundTouch 但还没吐出来的帧数"，不是一个固定常量——
     这是和 `noise-suppress-filter.c` 的关键差异：RNNoise 是固定 10ms 分块器，
     延迟是常数；SoundTouch 是可变长度 FIFO，必须用累计喂入/吐出帧数差动态算延迟，
     而不能照抄一个固定的 `ng->latency`）
  9. 返回该输出对象
```

算法引入的延迟（SoundTouch 内部序列长度相关设置，典型用于语音场景可调到数十毫秒级，
具体数值待阶段 B 用真实参数实测后记录）需要在属性页做只读展示（"预计延迟：~N ms"，
取第 8 步同一个累计差值在稳态下的典型值），让用户理解启用变声会给自己的耳返/混听
带来多大延迟，这与虚拟背景文档强调"遮罩陈旧度"对应的用户可感知延迟是同一类问题。

切换预设/手动调节半音值时，若不清空 SoundTouch 内部状态直接变更参数，个别实现会在
过渡瞬间产生明显的音高"打滑"或爆音；需要评估直接变更参数 vs. 变更前先 `flush`+
`clear` 重置的听感差异，阶段 B/C 实测后确定默认行为。

## 6. 变声效果与预设设计

### 6.1 P0：纯音高预设

预设本质上只是"半音偏移量"的命名快捷方式，背后调用的都是同一个
`setPitchSemiTones()`：

| 预设 | 半音偏移 | 说明 |
|---|---:|---|
| 自定义 | 用户滑块（-12 ~ +12，步进 0.5） | 默认选项 |
| 男声变女声 | +9 | 偏娱乐向，非自然共振峰转换（见 2.2） |
| 女声变男声 / 低沉大叔 | -8 | |
| 童声 / 萝莉 | +12 | 经典"花栗鼠"值，预期带明显"卡通化"音色 |
| 怪物 / 巨人 | -12 | |

数值依据：没有共振峰校正时，小幅偏移听起来只是"同一个声音带点抖动"，不足以让人
感觉像换了个角色；因此这组预设刻意选在"明显被调高/调低、略显卡通"的区间，而不是
"共振峰校正后才成立"的自然区间（据公开资料，纯音高工具男声变女声常用 +8~+12，
"+12 不做共振峰校正"正是典型的"花栗鼠"效果；女声变男声在 -4~-5 已能被感知为男声，
更深沉则需要更大偏移）。这些数值仍是**调研得出的估值，不是真人试听验证过的结果**
——本文档编写环境没有音频播放能力，无法实际听感确认，阶段 C 仍需用真实人声复核。
另外需要注意：很多消费级变声工具会把"调低音高"和"放慢语速"捆绑在一起实现（本质是
简单的变速播放），这会让"低沉"效果同时伴随说话变慢；AudioShift 用 SoundTouch 把
音高和时长解耦（`setTempo` 恒为 1.0，见 4.2），-12 只会变低沉，不会变慢，这点和多数
简单实现不同。

参考：[Male to Female Voice Changer: Sound Convincingly Feminine](https://voxbooster.com/blog/male-to-female-voice-changer-guide/)

具体偏移量需在阶段 C 用真实人声试听调整，上表为初始设计值，不是最终定案。

### 6.2 P1：附加效果层（不在 P0 交付范围）

在 P0 的纯音高变换稳定之后，可选叠加：

- **机器人音**：对 SoundTouch 输出做环形调制（乘以一个低频正弦载波，典型 20~60Hz），
  需要在滤镜结构体中维护一个跨调用保持的相位累加器
- **空灵 / 山洞回声**：简单延迟反馈（feedback delay line），复用
  `async-delay-filter.c` 的环形缓冲思路，但增加反馈系数而不仅仅是单次延迟

这两者都不需要新的第三方依赖，但会引入额外的增益/削波风险（见第 8 节），P0 阶段
**不内置限幅器**——若用户叠加效果后出现爆音，引导其在滤镜链中额外添加 OBS 自带的
`Limiter` 滤镜，而不是在 AudioShift 内部重新实现一遍限幅逻辑。

### 6.3 不追求的方向：自然共振峰保留变声

真正让男声"听起来像"另一个人的自然声音（而不是被调高/调低的同一个人），需要独立的
共振峰变换（formant shifting）或音色转换模型，技术路线和工程量接近另一个独立项目
（参考 Rubber Band 的 formant-preserving 选项，或 RVC 一类神经网络方案）。P0/P1 都不
承诺这个方向，留作 P2 的研究课题，视 P0 上线后的用户反馈决定是否投入。

### 6.4 P1：声音模板（温柔男中音 / 甜美女高音 等情感化预设）

用户侧经常用"温柔男中音""甜美女高音"这类带情感/音色形容词的方式描述想要的变声效果，
而不是直接说"调高 7 个半音"。这类模板**不能只靠 AudioShift 单个滤镜的音高变换实现**：

- "温柔""甜美"这种质感来自音色明暗（EQ）和动态是否平稳（压缩），不是音高本身能给的。
  纯音高变换只决定"大致音域"，决定不了"听起来柔和还是尖锐"。
- 固定半音偏移对每个人效果不一致：同样 +7 个半音，原始声音偏低和偏高的两个人，输出
  落在的实际音域中心是不同的。想要稳定地"听起来像某个目标音域"，理论上需要先实时估计
  说话人原始基频，再动态计算应偏移的半音数（类似自动校音器的思路），而不是套一个
  固定数字——这个自适应基频锚定方案工程量显著更大，不作为 P1 范围，列入 P2（见下）。

**P1 推荐架构**：声音模板不在 AudioShift 滤镜内部重新实现 EQ/压缩逻辑（避免和已有的
`eq-filter.c`/`compressor-filter.c` 重复造轮子），而是做成一层**组合预设**——选中
"温柔男中音"时，实际是按预先调好的参数表，在同一个音频源上程序化地创建/配置一组滤镜：

```text
用户选择模板"温柔男中音"
  -> AudioShift（半音：按模板设定，如 -3）
  -> 3-Band Equalizer（按模板设定：轻降高频、轻升低频，营造"温柔"质感）
  -> Compressor（按模板设定：中等比率、较快释放，让音量更平稳）
  -> （可选）轻混响效果，视听感需要再决定是否引入新滤镜
```

这一层"按模板批量创建/配置滤镜"的逻辑**不属于 `plugins/obs-filters/`**，需要在
`frontend/`（滤镜管理 UI 所在的 C++ 代码）新增一个"声音模板"入口，选中后调用 OBS
前端已有的滤镜增删 API 依次添加/配置上述滤镜链，而不是在 libobs 插件层面处理。这部分
前端改动的具体落点需要在阶段 E 单独勘查 `frontend/dialogs/OBSBasicFilters.cpp` 一类
文件后再细化，本文档暂不展开。

**P2 研究方向（不承诺）**：若 P1 的"固定参数模板"听感不够稳定（不同用户原始音域差异
导致同一模板效果飘忽），可以研究基频检测 + 自适应半音偏移——实时估计输入人声基频
（如自相关或 YIN 一类算法），计算与目标音域中心的半音差，动态写回 AudioShift 的
`setPitchSemiTones` 参数。这类似于自动校音器，工程量和复杂度明显超过 P0/P1，仅作为
后续研究方向记录，不在当前任何已排期阶段内。

## 7. OBS 属性设计

计划的滤镜属性：

- `Preset`：下拉列表（自定义 / 男声变女声 / 女声变男声 / 童声 / 怪物…），对应 6.1 表格
- `Pitch Shift (semitones)`：浮点滑块 -12 ~ +12，步进 0.5，仅在预设为"自定义"时可编辑
  （选择非自定义预设时，该滑块跟随预设值但置灰，与 `noise-suppress-filter.c` 里
  "选中方法后显示/隐藏对应参数"的 `obs_property_set_modified_callback` 模式一致）
- `Estimated Latency`：只读文本，展示当前设置下的预计算法延迟（毫秒）
- （P1 追加）`Effect Layer`：无 / 机器人 / 空灵回声，及各自的强度参数

## 8. 性能

### 8.1 已实测：CPU 占用与算法延迟

`audioshift-bench.c`（`ENABLE_TEST_INPUT` 下的基准测试，直接驱动
`audioshift-soundtouch.h` 的桥接层，不经过 OBS 滤镜回调）在本机（见
`BUILD_NOTE.md` 的环境）用 48kHz、480 帧/块（对应 OBS 典型的 10ms 音频包）实测：

| 配置 | 单块处理耗时（均值） | 单块处理耗时（最大值） | 算法引入延迟 |
|---|---:|---:|---:|
| 立体声 +9 半音 | 26.9 µs | 222.6 µs | ~91.4 ms |
| 立体声 -12 半音 | 33.1 µs | 162.5 µs | ~32.0 ms |
| 立体声 0 半音（不变调） | 17.7 µs | 87.9 µs | ~63.0 ms |
| 单声道 +9 半音 | 23.5 µs | 129.3 µs | ~91.0 ms |

CPU 占用结论：单块耗时均值在 17~33 微秒，相对 10ms（10000 微秒）的预算占比低于
0.4%，即使取最坏观测值（222.6 微秒）也只占预算的 2.2%——**CPU 占用确认可忽略**，
不需要为此单独做性能优化。

算法延迟结论（**和编码前的预期不一致，需要在阶段 C 处理**）：延迟并不是一个
数十毫秒的稳定数字，而是随半音偏移量变化，在本次实测里从 32ms 到 91ms 不等，且
即使半音偏移为 0（不变调）也有 63ms 延迟——说明延迟主要来自 SoundTouch 默认的
`SEQUENCE_MS`/`SEEKWINDOW_MS`/`OVERLAP_MS` 窗口设置（面向音乐场景调的默认值），
不是音高变换本身的必然代价。90ms 级的延迟对直播场景里"边说话边通过耳返听自己"
的场景会是能明显感知的卡顿感。阶段 C 需要评估调低这三个窗口参数（SoundTouch 文档
里明确提到可以为语音场景调小）换取更低延迟，并重新跑一遍 `audioshift-bench.c`
确认改善幅度，再据此决定 §7 的"预计延迟"展示文案是否需要默认提示用户"当前设置下
延迟较高，建议调整"。

### 8.2 仍未验证

| 指标 | 说明 |
|---|---|
| 极端半音值下的音质 | 预计出现"金属声"/颤音类伪影（WSOLA 的已知局限），需要真人试听评估可接受范围——本文档编写环境没有音频播放能力，无法确认 |
| 真实语音信号下的延迟是否与上表的合成正弦波一致 | `audioshift-bench.c` 用的是固定频率正弦波，真实语音的瞬态和谐波结构更复杂，WSOLA 的延迟特性可能不同 |

## 9. 分阶段实施

### 阶段 A：依赖引入

1. 核实 SoundTouch 当前官方仓库地址、最新稳定版本、许可证文本
2. 编写 `plugins/obs-filters/cmake/soundtouch.cmake`，镜像 `rnnoise.cmake` 的
   "先 `find_package`，没有则 vendor 源码构建对象库"模式
3. 验证 `obs-filters` target 内混合编译 `.c`/`.cpp` 可行（最小空实现验证）
4. 记录版本号、来源、SHA-256，补全 3.3 节的核验记录

### 阶段 B：滤镜核心

1. 新增 `audioshift-filter.c`/`.h`：`create`/`destroy`/`update`/`filter_audio` 骨架
2. 实现 interleave/deinterleave helper 与 FIFO 缓冲、延迟补偿
3. 单声道/立体声、动态采样率变化下的正确性验证
4. 写 `audioshift-bench.c`（`ENABLE_TEST_INPUT` 下）测 CPU 耗时和延迟

### 阶段 C：预设与界面

1. `get_properties`/`get_defaults`，预设下拉与半音滑块的联动
2. 中英文 locale 词条（`AudioShift.*`，参照 `BackgroundSegmentation.*` 的命名风格）
3. 真实人声试听，调整 6.1 表格的预设半音值

### 阶段 D：稳定性

1. 长时间直播稳定性测试（见第 10 节）
2. 滤镜启用/禁用、预设/参数热切换时的爆音与"打滑"问题排查
3. 异常输入（声道数突变、采样率突变、极短/静音缓冲区）的边界测试

### 阶段 E（P1，不在首个发布范围内）

1. 机器人 / 空灵回声效果层
2. 声音模板（见 6.4）：勘查 `frontend/dialogs/OBSBasicFilters.cpp` 一类文件，设计
   "按模板批量创建/配置 AudioShift + EQ + Compressor 滤镜链"的前端入口
3. 评估是否投入共振峰保留、自适应基频锚定或神经音色转换（P2 方向，需单独立项评估）

## 10. 测试要求

### 已落地的自动化测试

`audioshift-filter-test.c`（`ENABLE_TEST_INPUT`，纯逻辑单元测试，已跑通）：

- 四个预设的半音映射值，以及未知/空字符串预设回退到 0（而不是沿用上一个值）
- 交织：多声道 planar 输入到交织缓冲区的下标映射
- 交织时某声道 `data[c]` 为 `NULL`（OBS 未提供该声道）时安全输出 0，不解引用空指针
- 交织→反交织往返，每个声道的数据精确还原

`audioshift-bench.c`（`ENABLE_TEST_INPUT`，性能基准，已跑通，结果见 8.1）——这两个
测试覆盖的是"纯函数逻辑正确"和"CPU/延迟数量级"，**不覆盖**下面列出的功能/质量/
稳定性测试：那些都需要在真实 OBS 界面里操作真实音频源，本次会话没有启动
`obs64.exe` 验证过。

### 功能测试（仍需人工在 OBS 里验证）

- 挂载到麦克风、桌面音频、媒体源音轨
- 预设切换、手动滑块调节（包括直播中热切换）
- 启用/禁用滤镜反复切换
- 单声道源与立体声源
- 采样率变化（如系统默认设备从 48kHz 切到 44.1kHz）
- 与其他音频滤镜（降噪、压缩、限幅）叠加使用的顺序与互不干扰

### 质量测试

- 不同性别/音域真实人声在各预设下的可用性试听
- 极端半音值（±12）下的伪影可接受度
- 快速语速、气声、笑声等非稳态语音的处理效果

### 稳定性测试

- 连续直播 8 小时以上不崩溃、不漏内存
- 反复打开/关闭滤镜属性窗口
- 反复切换预设和挂载源
- SoundTouch 库缺失/版本不匹配时的优雅降级（直通 + 日志，不崩溃）

## 11. 风险与应对

| 风险 | 影响 | 应对 |
|---|---|---|
| LGPL 义务未满足 | 发行合规风险 | **已落实**：改为 vendor 完整源码静态编译进 `obs-filters.dll`（见 3.3/3.4），比动态链接更直接地满足对应源码义务，许可证文本随 `soundtouch/COPYING.TXT` 保留 |
| 极端音高下伪影明显 | 用户体验差 | 限制预设的默认偏移范围，"自定义"滑块允许用户自行承担极端效果 |
| 预设/参数热切换爆音 | 直播翻车 | 评估切换时机是否需要短暂静音过渡或内部状态重置 |
| C/C++ 混合编译引入构建问题 | 延误交付 | 阶段 A 先用最小空实现验证，再开始实际滤镜逻辑开发 |
| 叠加机器人/回声效果导致削波 | 破音、失真 | P0 不内置限幅，引导用户叠加现有 `Limiter` 滤镜；P1 做效果层时评估是否需要内部软限幅 |
| 用户挂载到非人声源（游戏音效/音乐） | 效果难听但不应崩溃 | 功能说明中明确建议用途，保证极端情况下仍是直通而非崩溃/静音 |

## 12. ppobs 代码落点

### 12.1 已新增文件

```text
plugins/obs-filters/audioshift-filter.c              滤镜主体（properties、FIFO、延迟补偿）
plugins/obs-filters/audioshift-filter.h              纯逻辑 helper（预设表、交织/反交织），供测试复用
plugins/obs-filters/audioshift-filter-test.c         单元测试（ENABLE_TEST_INPUT），只链 .h，不链 .c
plugins/obs-filters/audioshift-bench.c               真实 CPU/延迟基准（ENABLE_TEST_INPUT），见 8.1
plugins/obs-filters/audioshift-soundtouch.cpp / .h   SoundTouch C++ API 的 C 封装
plugins/obs-filters/cmake/soundtouch.cmake           依赖查找 + vendored 源码对象库构建
plugins/obs-filters/soundtouch/                      vendored SoundTouch 2.4.1 源码子集
plugins/obs-filters/soundtouch/VENDORED-FROM.md      版本/commit/裁剪范围记录（见 3.4）
```

**为什么加了 `audioshift-filter.h`**：`gain-filter.c`/`compressor-filter.c`/
`noise-suppress-filter.c` 等既有简单音频滤镜确实都没有头文件，最初也是照这个惯例做
的。但真正写单元测试时发现问题：`audioshift-filter.c` 的 `audioshift_name()` 调用
`obs_module_text()`，这个符号只有在 `obs-filters.c` 跑过
`OBS_MODULE_USE_DEFAULT_LOCALE()` 宏之后才存在——测试程序如果链接整个
`audioshift-filter.c`，链接器就会要求解析这个符号，而把 `obs-filters.c` 也链进来
又会级联要求链接它 `extern` 引用的全部 ~30 个其它滤镜的 `struct obs_source_info`。
解决办法是把三个纯逻辑函数（预设查表、交织、反交织）改成 `static inline` 直接定义在
头文件里（和 `libobs/util/deque.h` 的做法一致），测试程序只 `#include` 这个头文件，
完全不需要链接 `audioshift-filter.c`/`audioshift-soundtouch.cpp`/SoundTouch。

### 12.2 已修改文件

```text
plugins/obs-filters/CMakeLists.txt       接入 cmake/soundtouch.cmake，注册新源文件
plugins/obs-filters/obs-filters.c        extern 声明 + obs_register_source
plugins/obs-filters/data/locale/en-US.ini
plugins/obs-filters/data/locale/zh-CN.ini
```

参考实现（现有同类音频滤镜，设计/实现阶段对照）：

```text
plugins/obs-filters/gain-filter.c             最简单的同步音频滤镜模板
plugins/obs-filters/compressor-filter.c       设置的互斥保护、属性 UI 模式
plugins/obs-filters/noise-suppress-filter.c   FIFO 缓冲 + 延迟补偿 + 第三方 DSP 库接线
plugins/obs-filters/async-delay-filter.c      延迟/环形缓冲思路（P1 回声效果参考）
```

## 13. 结论

采用「SoundTouch（WSOLA 音高/速度独立变换）+ 同步音频滤镜 + 自写交织/反交织 + FIFO
延迟补偿」作为 P0 实现路线，不引入神经网络、不需要独立 worker 线程、不需要 GPU。

该方案满足：

- 本地离线处理，无云服务依赖
- 依赖单一、成熟、许可证路径清晰（LGPL-2.1，源码已 vendor 进本仓库静态编译，合规
  路径比动态链接更直接）
- 可挂载到任意音频源，与现有音频滤镜（降噪/压缩/限幅）自由叠加
- 不新增 GPU/神经网络相关的分发与兼容性负担
- 为 P1（效果层）和 P2（自然共振峰/音色转换，待评估）预留清晰的扩展边界

P0 不追求"自然"性别转换音质，只做娱乐向角色化变声；若后续产品方向明确要做高自然度的
实时性别转换，需要单独评估 Rubber Band 商业授权或神经网络音色转换方案，作为独立课题。

## 14. 验证状态

### 已完成

- 现状调研：确认 obsvlive 及 ppobs 均无可复用的音高变换代码或依赖（见文档头部说明）
- 现有音频滤镜架构、`obs_audio_data` 契约、FIFO/延迟补偿范式的代码走读（见第 4、5 节
  引用的具体文件）
- SoundTouch 官方仓库地址（`codeberg.org/soundtouch/soundtouch`）、最新版本号
  （2.4.1）、LGPL-2.1 许可证、CMake find_package 支持的核实（见 3.3）
- **已获用户明确同意并实际下载**：vendor 了 SoundTouch 2.4.1（commit
  `0047e0b1`）的库源码子集进 `plugins/obs-filters/soundtouch/`，裁剪范围记录在
  `VENDORED-FROM.md`（见 3.4）
- `plugins/obs-filters/` 完整骨架代码（滤镜本体、SoundTouch 桥接层、vendored 源码、
  CMake 接线、中英文 locale）已落地，详见上方"代码落点"一节
- **已跑通两次真实构建**：
  1. 不接入 SoundTouch 时的直通路径（`AUDIOSHIFT_HAS_SOUNDTOUCH` 未定义）
  2. **接入真实 vendored SoundTouch 后的完整路径**——`cmake --build build_x64
     --config RelWithDebInfo --target obs-filters` 编译了全部 13 个 vendored
     SoundTouch `.cpp` 文件（通过 `obs-soundtouch` 对象库 target）、
     `audioshift-soundtouch.cpp` 的 `AUDIOSHIFT_HAS_SOUNDTOUCH=1` 分支（真正调用
     `soundtouch::SoundTouch::setPitchSemiTones`/`putSamples`/`receiveSamples` 等
     API）、以及 `audioshift-filter.c`，链接产出
     `build_x64/plugins/obs-filters/RelWithDebInfo/obs-filters.dll`，没有报错或
     警告。`§4.2` 里记录的 API 签名假设（`uint` 类型、`setPitchSemiTones(double)`
     重载选择等）都在真实头文件下被验证为正确，不需要修正
- `audioshift-filter-test.c` 四项纯逻辑单元测试已跑通（退出码 0，无断言失败，见
  10 节）
- **实际 CPU 占用与算法延迟已实测**（`audioshift-bench.c`，见 8.1 完整数据）：
  CPU 占用在 48kHz/480 帧块下均值 17~33 微秒，远低于 10ms 预算，可忽略；算法延迟
  实测 32~91ms（随半音偏移量变化），比"数十毫秒级"的编码前预期更高且波动更大，
  已记录为阶段 C 需要处理的问题（调低 SoundTouch 窗口参数）而不是直接采用默认值

### 仍未验证

- 真实人声在各预设下的主观音质评估——`audioshift-bench.c` 用的是合成正弦波，只能
  验证"跑得动、延迟多少"，不能验证"听起来像不像预期的角色"；本文档编写环境没有
  音频播放能力，无法替代真人试听
- 真实语音信号下延迟是否与合成正弦波的实测结果一致（见 8.2）
- 长时间直播下的稳定性与内存表现
- 滤镜在 OBS 实际界面中能否正常添加、显示属性面板、处理真实音频流——
  `audioshift-filter-test.c`/`audioshift-bench.c` 都是独立命令行程序，直接调用
  `audioshift-soundtouch.h` 或纯逻辑函数，不经过 `obs_register_source` 之后的真实
  滤镜生命周期（`create`/`update`/`filter_audio`/`destroy` 由 OBS 调度），本次
  仍没有启动 `obs64.exe` 做运行期验证
- vendored 对象库在非 Windows/非 MSVC 平台（若 ppobs 未来扩展到那些平台）上的
  编译情况——本次验证只覆盖了 BUILD_NOTE.md 记载的 Windows + VS 2026 环境
