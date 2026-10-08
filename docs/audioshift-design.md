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

- 确认当前 SoundTouch 官方源码仓库地址与最新稳定版本号（历史上托管在
  `surina.net/soundtouch`，近年源码仓库有迁移，**需在阶段 A 实际核实一次**，不能假定
  旧文档记忆的地址仍然准确）
- 记录所用版本号、下载来源、SHA-256
- 确认 LGPL-2.1 的动态链接义务已满足：以共享库形式链接（而非静态整体链接进
  `obs-filters` 二进制），随发行包附带 LGPL 许可证全文和版权声明，并保留用户按 LGPL
  条款获取/替换该库对应版本源码的途径
- 若 Windows 平台的预编译 SoundTouch 包不随 `obs-deps` 提供，参照 `rnnoise.cmake` 的
  "先 `find_package`，找不到则使用仓库内 vendored 源码构建对象库"模式自行引入源码并
  核验许可证文件完整保留

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

CMake 需要确认 `obs-filters` target 允许在同一 target 内混合编译 `.c`/`.cpp`
（OBS 其他插件如 `obs-webrtc` 已是纯 C++，技术上没有阻碍，但需要在阶段 A 用一个最小
的空实现验证构建系统按预期分别调用 C/C++ 编译器、且导出符号可从 `.c` 文件正确调用）。

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

### 5.1 线程模型：无需独立 worker 线程

与虚拟背景滤镜（ONNX 神经网络推理，单帧耗时数十毫秒，必须放到独立 worker 线程避免
阻塞 OBS 渲染/音频线程）不同，SoundTouch 的 WSOLA 时域算法对典型语音帧（十毫秒级）的
处理成本极低（预计落在微秒到低个位数毫秒级，需在阶段 B 实测确认），可以**直接在
`filter_audio` 回调内同步处理**，不引入额外线程、不需要跨线程的 mutex 保护音频数据
本身——这与 `gain-filter.c`/`compressor-filter.c` 的同步处理模型一致。

仍然需要一个轻量 `pthread_mutex_t`（或等价的原子/锁）保护**设置**：`update()`
在 UI 线程被调用修改音高/预设参数，而 `filter_audio` 在音频线程读取，二者存在竞争，
做法与 `compressor-filter.c` 对设置的互斥保护一致。

### 5.2 缓冲与延迟补偿

SoundTouch 内部维护 FIFO，`putSamples`/`receiveSamples` 不保证每次调用输入输出帧数
一一对应（尤其是算法启动初期，内部缓冲尚未攒够数据时 `receiveSamples` 可能返回 0）。
处理方式参照 `noise-suppress-filter.c` 的"攒够才吐，否则延后"模式：

```text
filter_audio(audio):
  1. interleave audio->data[*][0..frames) 到 scratch
  2. st_put_samples(st, scratch, frames)
  3. 循环 st_receive_samples 把可取到的输出帧追加进滤镜自带的 output 缓冲（DARRAY）
  4. 若 output 缓冲中累积帧数 < audio->frames：返回 NULL（按 filter_audio 文档允许的
     "推迟输出"语义），等待后续调用补足
  5. 否则从 output 缓冲头部取出 audio->frames 帧，deinterleave 回自带的
     struct obs_audio_data 输出对象，timestamp 按累计算法延迟回退
     （= audio->timestamp - latency_ns，latency_ns 由已缓冲但未吐出的帧数folosi
     当前采样率换算得到，做法与 noise-suppress 的 `ng->latency` 字段一致）
  6. 返回该输出对象
```

算法引入的恒定延迟（SoundTouch 内部序列长度相关设置，典型用于语音场景可调到数十毫秒
级，具体数值待阶段 B 用真实参数实测后记录）需要在属性页做只读展示（"预计延迟：~N ms"），
让用户理解启用变声会给自己的耳返/混听带来多大延迟，这与虚拟背景文档强调"遮罩陈旧度"
对应的用户可感知延迟是同一类问题。

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
| 男声变女声 | +7 | 偏娱乐向，非自然共振峰转换（见 2.2） |
| 女声变男声 / 低沉大叔 | -7 | |
| 童声 / 萝莉 | +10 | 极端值，预期带"卡通化"音色 |
| 怪物 / 巨人 | -10 | 极端值 |

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

## 7. OBS 属性设计

计划的滤镜属性：

- `Preset`：下拉列表（自定义 / 男声变女声 / 女声变男声 / 童声 / 怪物…），对应 6.1 表格
- `Pitch Shift (semitones)`：浮点滑块 -12 ~ +12，步进 0.5，仅在预设为"自定义"时可编辑
  （选择非自定义预设时，该滑块跟随预设值但置灰，与 `noise-suppress-filter.c` 里
  "选中方法后显示/隐藏对应参数"的 `obs_property_set_modified_callback` 模式一致）
- `Estimated Latency`：只读文本，展示当前设置下的预计算法延迟（毫秒）
- （P1 追加）`Effect Layer`：无 / 机器人 / 空灵回声，及各自的强度参数

## 8. 性能

本节在编码前只能给出**预期范围与待验证项**，不编造尚未测得的具体数字：

| 指标 | 预期 | 验证方式 |
|---|---|---|
| 单声道/立体声典型 CPU 占用 | 远低于 ONNX 推理滤镜，预计可忽略不计 | 阶段 B 用 `matting-bench.c` 同类手法写一个 `audioshift-bench.c` 基准测试 |
| 算法引入延迟 | 预计数十毫秒级，具体取决于 SoundTouch 的 `SEQUENCE_MS`/`SEEKWINDOW_MS`/`OVERLAP_MS` 设置 | 阶段 B 实测并记录，决定是否需要针对语音场景调低默认序列长度以换取更低延迟 |
| 极端半音值下的音质 | 预计出现"金属声"/颤音类伪影（WSOLA 的已知局限），需要试听评估可接受范围 | 阶段 C 真实人声试听 |

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
2. 评估是否投入共振峰保留或神经音色转换（P2 方向，需单独立项评估）

## 10. 测试要求

### 功能测试

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
| LGPL 动态链接义务未满足 | 发行合规风险 | 严格动态链接 + 随包附带许可证文本，阶段 A 落实 |
| 极端音高下伪影明显 | 用户体验差 | 限制预设的默认偏移范围，"自定义"滑块允许用户自行承担极端效果 |
| 预设/参数热切换爆音 | 直播翻车 | 评估切换时机是否需要短暂静音过渡或内部状态重置 |
| C/C++ 混合编译引入构建问题 | 延误交付 | 阶段 A 先用最小空实现验证，再开始实际滤镜逻辑开发 |
| 叠加机器人/回声效果导致削波 | 破音、失真 | P0 不内置限幅，引导用户叠加现有 `Limiter` 滤镜；P1 做效果层时评估是否需要内部软限幅 |
| 用户挂载到非人声源（游戏音效/音乐） | 效果难听但不应崩溃 | 功能说明中明确建议用途，保证极端情况下仍是直通而非崩溃/静音 |

## 12. ppobs 代码落点（计划）

### 12.1 计划新增文件

```text
plugins/obs-filters/audioshift-filter.c / .h        滤镜主体（properties、FIFO、延迟补偿）
plugins/obs-filters/audioshift-soundtouch.cpp / .h   SoundTouch C++ API 的 C 封装
plugins/obs-filters/cmake/soundtouch.cmake           依赖查找/vendor 接线

plugins/obs-filters/audioshift-bench.c               性能基准（ENABLE_TEST_INPUT）
plugins/obs-filters/audioshift-filter-test.c         滤镜逻辑单元测试（ENABLE_TEST_INPUT）
```

### 12.2 计划修改文件

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
- 依赖单一、成熟、许可证路径清晰（LGPL-2.1，动态链接合规即可商用）
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

### 仍未验证（均为阶段 A 及之后的工作）

- SoundTouch 当前官方仓库地址、版本号、许可证文本的实际核实
- `obs-filters` target 混合编译 `.c`/`.cpp` 的可行性
- 实际 CPU 占用、算法延迟的真实测量值（第 8 节数字均为预期范围，非实测）
- 真实人声在各预设下的主观音质评估
- 长时间直播下的稳定性与内存表现
