# obsvlive 虚拟背景直播设计方案

> **移植说明**：本文档随 `feature/virtual-background` 从 obsvlive 移植到 ppobs。
> ppobs 未引入 obsvlive 的 face swap 滤镜，只移植了它带进来的 ONNX Runtime
> CMake 接线（`plugins/obs-filters/cmake/onnxruntime.cmake`、`OBS::onnxruntime`
> target、`FACE_SWAP_HAS_ORT` 宏——宏名保留原样，以免将来与 obsvlive 同步时
> 冲突）。下文提到的 `face-swap-*.c` 仅作设计对照，在 ppobs 中并不存在。

## 1. 目标

为 obsvlive 增加本地实时虚拟背景能力：从摄像头视频中分离真人前景，将真人合成到图片或视频背景中，然后通过 OBS 原有的场景和推流链路输出。

目标使用方式：

```text
摄像头 -> 人像分割/抠像 -> 前景 Alpha -> 图片或视频背景 -> OBS 推流
```

本方案采用可自部署的开源组件，不调用云端抠像服务，不依赖商业 SDK，不上传摄像头画面。

## 2. 范围与非目标

### 2.1 第一版范围

- Windows x64
- 摄像头真人前景分割
- 图片背景
- 视频背景
- OBS 场景预览、录制和推流
- CPU 推理可用，GPU 加速作为可选增强
- 中英文配置界面
- 推理失败时自动回退为原始摄像头画面

### 2.2 第一版不包含

- 云端抠像或在线账号服务
- 3D 虚拟人
- 人脸换脸
- 多人分别抠像
- 专业级发丝级抠像保证
- 依赖 NVIDIA 专有 SDK 的必选功能

## 3. 推荐开源技术栈

### 3.1 推荐组合

| 组件 | 推荐方案 | 许可证/用途 |
|---|---|---|
| 人像抠像模型 | MODNet，转换为 ONNX | Apache-2.0；逐帧抠像，无时序状态 |
| 视频时序抠像 | RobustVideoMatting（RVM） | **GPL-3.0**；循环网络，时序稳定性和发丝质量更好 |
| 推理运行时 | ONNX Runtime | MIT；本地 CPU 推理，后续可接 DirectML |
| 图像处理 | obsvlive 现有预处理代码 + OBS Graphics API | 复用现有代码，不引入 OpenCV 运行时 |
| 图片背景 | OBS `image_source` 或 `gs_image_file_t` | 由 OBS 管理纹理和生命周期 |
| 视频背景 | OBS `ffmpeg_source` | 由 OBS 管理解码、循环和时间推进 |
| GPU 合成 | OBS effect/shader | 在 GPU 上完成前景、背景和 Alpha 合成 |

### 3.2 模型选择结论

**同时支持 MODNet 和 RVM**，由用户按场景选择。obsvlive 以 GPL-3.0 开源发行，因此 RVM 的 GPL-3.0 许可证可接受。

两者的 ONNX 契约差异很大，已抽象为 `matting-models.c` 中的数据表：

| | MODNet | RVM |
|---|---|---|
| 输入数 | 1（`src`） | 6（`src` + `r1i`~`r4i` + `downsample_ratio`） |
| 循环状态 | 无 | 4 组，`rXo` 逐帧回环到 `rXi` |
| 归一化 | `(x-127.5)/127.5`，即 `[-1,1]` | `x/255`，即 `[0,1]` |
| matte 输出 | 按形状识别（导出命名不统一） | 显式取 `pha`（`fgr` 同尺寸，会误判） |
| 输入尺寸 | 动态，由本项目按 256 驱动 | 动态，由本项目按 256 驱动 |
| 许可证 | Apache-2.0 | GPL-3.0 |

两个家族的导出都使用符号化的空间维度，因此 256 是本项目选定的工作点，而非模型限制。matte 最终会在 GPU 上放大到视频分辨率，512 的输入张量是 256 的 4 倍开销，而多出的细节大多被羽化和放大过程抵消。若实测发现发丝边缘过粗，提高 `matting-models.c` 中的尺寸常量是第一个可尝试的调整——该常量只影响驱动方式，会话创建时仍以模型自身的声明为准校验。

### 3.3 已验证的模型实测规格

使用 `docs/inspect-onnx.py` 解析实际模型文件得到（该脚本直接读取 protobuf，不依赖 `onnx` 包）：

MODNet（PyTorch 1.7 导出，24.7 MB）：

```text
INPUTS  (1): input   float32  [batch_size, 3, height, width]
OUTPUTS (1): output  float32  [batch_size, 1, height, width]
```

关键结论：**所有空间维度均为符号化（动态）**。这推翻了设计初稿中「固定 512×512」的假设，也暴露了一个实现缺陷——按形状识别 matte 输出时，动态维度被 ORT 报告为 `-1`，早期实现将其相乘得到 0，导致永远匹配不上，模型会被直接拒绝。该缺陷已修复，并由 `matting-models-test.c` 中的 `test_dynamic_shape_matte_is_accepted` 固化。

RVM 的循环状态要求帧按顺序提交。源被隐藏、场景切换或播放暂停后，残留状态描述的是已不在画面上的内容，因此超过 `BACKGROUND_STATE_GAP_NS`（500ms）无推理时会清空状态重新开始。

### 3.5 许可证更正记录

本文档早期版本曾记载「RVM 官方仓库采用 MIT」，**该记载有误**。RVM 仓库 README 的 News 明确写明：

> [Sep 16 2021] Code is re-released under GPL-3.0 license.

GPL-3.0 具有传染性。当前项目以 GPL-3.0 开源，因此不受影响；若未来改为闭源或专有分发，则必须移除 RVM 支持，仅保留 MODNet。

### 3.6 许可证核验规则

“开源”和“免授权”不等于可以不履行许可证义务。正式发布前必须为以下每个文件建立许可证记录：

- 模型代码仓库
- 预训练权重
- ONNX 转换脚本和转换后的模型
- ONNX Runtime 二进制包
- 任何第三方 DLL、模型依赖和示例资源

只接受以下条件的依赖进入默认发行包：

- 许可证允许商业使用、修改和再分发
- 不要求向上游支付授权费
- 许可证文本和版权声明可随产品发布
- 模型权重的许可证与模型代码许可证一致或有独立明确声明
- 不包含“仅研究用途”“禁止商业使用”或不明确的权重授权
- 与本项目当前的 GPL-3.0 发行方式相容

每次升级模型或运行时都必须重新核验许可证和 SHA-256。模型文件不能只根据代码仓库许可证推断为可再分发。

当前两个模型均**不随产品打包、也不纳入本仓库**，由使用方自行放入配置目录，因此权重再分发义务不落在本项目。这一点对 RVM 尤其重要：它是 GPL-3.0，仓库一旦收录其权重就构成再分发，需随发行包附带许可证文本。

模型由使用方自行保管（本项目开发环境中存放于上级私有仓库 `aiInfluencer/doc/models/`），运行时从下面 3.5 节所述的目录加载。

若未来改为随包分发，需重新核验并在发行包内附带各自的许可证文本。

### 3.7 已核验依赖记录

按 3.6 的要求，当前发行包内的推理运行时核验记录如下：

| 项目 | 内容 |
|---|---|
| 组件 | ONNX Runtime（`Microsoft.ML.OnnxRuntime`） |
| 版本 | 1.23.2（与 `plugins/obs-filters/cmake/onnxruntime.cmake` 中的 pin 一致） |
| 来源 | nuget.org 官方包 |
| 许可证 | MIT，允许商用、修改和再分发，许可证文本随包提供 |
| 签名 | `onnxruntime.dll` 带有效 Authenticode 签名，签发者 Microsoft Corporation |
| nupkg SHA-256 | `25FE172F7FCDF34F2B5B02C8B997F6B88ABC282956849D4AF4335AF23D0C4E4A` |
| DLL SHA-256 | `DEC964AB1EE36CC9B0AE247D13B376627992FC57DEC0454354017AB8FD84F1EA` |
| 本地路径 | `.deps/onnxruntime-1.23.2/` |

获取与配置方式见 3.4；该包解压到约定位置后会被自动探测，无需手动传
`ONNXRUNTIME_ROOT`。打包前仍应确认 `bin/64bit/onnxruntime.dll` 存在，因为缺失时
构建不会失败，滤镜只是静默退化为直通。

模型权重（MODNet/RVM）不随发行包分发，需使用方自行放入 3.8 所述目录。

### 3.8 模型目录

模型放在 **OBS 配置目录**下，属性页会显示实际解析出的完整路径：

规则是"OBS 配置目录 + background-models"：

```text
便携版：<OBS 目录>\config\background-models\
安装版：%APPDATA%\background-models\
```

选择配置目录而非插件 data 目录，因为它是用户可写位置：添加模型无需管理员权限，
升级替换 OBS 目录也不会丢失。早期版本曾把模型放在 data 目录下，但按机器安装时该
目录属于安装目录，普通用户无法写入，实测中已因此无法加载模型。

**不要使用 `os_get_config_path_ptr`。** 它在 Windows 上固定解析到 `CSIDL_APPDATA`，
完全不理会便携模式——便携版用户会被指向当前登录用户的漫游配置目录，这既违背便携版
"所有数据随目录走"的前提，也会导致 U 盘换机器后模型丢失。

路径由 `obs_module_config_path` 推导：它返回的是
`<root>/obs-studio/plugin_config/obs-filters`，在 `/obs-studio/` 处截断即可得到
配置根。**不能按固定层数截断**——便携版在配置根与 `obs-studio` 之间多一层 `config`，
固定层数必然在两种布局中错一个（实测：按层数截断会让安装版落到
`%APPDATA%\background-models`，跑到了 `obs-studio` 外面）。

实现中同时搜索两个位置，以兼容两种合理理解：

```text
<配置根>/background-models
<配置根>/obs-studio/background-models
```

前者是属性页展示并自动创建的目录。

注意日志中的 "model '<名称>' unavailable or incompatible, passing through" 同时覆盖
"文件不存在"和"模型不兼容"两种情况，排查时应先确认路径再怀疑模型。属性页会直接显示
当前生效的目录，优先看那里。

## 4. 总体架构

### 4.1 P0 架构：抠像滤镜 + OBS 场景背景

第一版不在滤镜内部解码背景视频，而是使用 OBS 已有源：

```text
OBS 场景
  ├── 背景图片或视频源
  └── 人物源（摄像头 / 视频 / 图片）
        └── background_segmentation_filter
```

滤镜只负责将人物源转换为带透明 Alpha 的真人前景。背景源位于真人源下方，由 OBS 负责图片显示、视频解码、循环和场景切换。

滤镜注册为**同步滤镜**（`OBS_SOURCE_VIDEO`，只实现 `video_render`），而不是异步帧滤镜。原因是 OBS 有两道兼容性限制：

- `libobs/obs-source.c` 的 `filter_compatible()`：滤镜能力位必须是源能力位的子集
- `frontend/dialogs/OBSBasicFilters.cpp` 的 `filter_compatible()`：异步滤镜只出现在异步源的滤镜菜单中

如果声明 `OBS_SOURCE_ASYNC`，滤镜将只能挂在摄像头、媒体源等异步源上，**无法挂到图片源**，也就无法用一张静态图片预览抠像效果。改为同步滤镜后，摄像头、视频文件、图片、窗口捕获、色源均可使用。

代价是模型输入需要从 GPU 回读：把目标源渲染到一个模型尺寸的 `gs_texrender`，再通过 `gs_stagesurface` 拷回 CPU。该回读按推理帧率限频，不是每帧执行，因此开销与推理频率成正比而非帧率。

该约束由 `background-compat-test.c` 固化，防止回归。

该架构的优点：

- 改动最小，降低滤镜内部 source 生命周期风险
- 图片和视频背景无需重复实现解码器
- 背景音频可由 OBS 单独控制
- 用户仍能使用 OBS 的场景、转场、录制和推流能力
- 发生抠像错误时可以直接禁用滤镜

### 4.2 P1 架构：一体化背景替换滤镜

在 P0 稳定后，再增加 `background_replace_filter`，允许用户在滤镜属性中选择图片或视频。内部可通过 OBS source 创建 `image_source`/`ffmpeg_source`，但必须处理：

- 内部 source 的创建和释放
- 背景路径热切换
- 视频循环和暂停
- 禁止背景音频泄漏
- source tick 与 render 的线程约束
- 避免递归渲染

一体化滤镜不是 MVP 的前置条件。

## 5. 实时处理管线

### 5.1 线程职责

```text
OBS Graphics 线程（video_render）
  1. 按推理帧率限频，将目标源渲染到模型尺寸的 texrender
  2. gs_stage_texture + map，把 RGBA 回读到 CPU 双缓冲
  3. 上传最近一次可用的 Alpha Mask 为 GPU 纹理
  4. Shader 完成 Alpha 合成，不等待模型推理

AI Worker 线程
  1. RGBA 转模型输入张量（归一化 + CHW）
  2. ONNX Runtime 推理
  3. 输出 mask 后处理
  4. 发布带时间戳的 mask
```

禁止在 `video_render` 中直接运行模型推理，禁止等待 worker，禁止因单帧推理失败阻塞 OBS 渲染链路。

### 5.2 缓冲与时间戳

输入和输出采用双缓冲设计：

```text
rgba[2]   : 模型尺寸的 RGBA 回读结果
mask[2]   : Alpha mask、尺寸、时间戳、序号
```

发布 mask 时必须一次性发布全部元数据，避免 mask 与时间戳不匹配。

已实现策略：

- worker 未取走上一帧时跳过本次回读，队列深度恒为 1，不累积延迟
- 没有新 mask 时复用上一份 mask（按 serial 判断是否需要重新上传纹理）
- mask 超过 1 秒未更新时回退原始画面
- 回读尺寸固定为模型输入尺寸，源分辨率变化不影响缓冲区

## 6. 人像分割与 Alpha 后处理

### 6.1 模型输出

模型输出为单通道浮点 mask：

```text
0.0 = 背景
1.0 = 真人前景
```

模型输出必须转换为适合 GPU 上传的格式，例如归一化的 8-bit 单通道纹理，或 RGBA 纹理的 R 通道。

### 6.2 P0 后处理

- 可调阈值
- 轻度腐蚀/膨胀
- 边缘羽化
- 双线性上采样
- mask EMA 时域平滑
- 置信度过低时回退上一帧
- 人物消失时渐隐
- 背景色测量 + 边缘去溢色（见 6.4）

### 6.4 非绿幕素材的边缘处理

`doc/dealer/AI10-Nina.mp4` 不是绿幕拍摄，背景是一面接近白色、略偏蓝的灰墙
（实测左上约 227,233,240，左下约 235,237,243，存在纵向渐变）。直接做 straight
alpha 合成时，手臂边缘会出现白色虚影。实测确认有两个独立成因：

**成因一：边缘像素混入墙色（静态也存在）**

半透明边缘像素本身就是 `observed = a*F + (1-a)*B` 的混合结果。把它按原样合成到
新背景上，墙色会被一起带过去。实测该边缘带比人物本体亮约 78（亮度值）。

处理方式：每帧用「确定是背景」的像素测量墙色中位数，再在 shader 中解出
`F = (observed - (1-a)*B) / a`。不能硬编码为纯白，否则会留下偏蓝的色偏。

除法在 alpha 趋近 0 时会放大 matte 误差，因此需要：

- 先把 matte 向内收缩 1 像素，丢掉最脏的一圈（那圈几乎是纯墙色，没有可恢复的人物颜色）
- 用 `despill_floor` 限制除数下界
- 用 `despill_strength` 限制修正幅度

三者缺一都会从「白边」过冲成同样明显的「黑边」。实测调参后边缘误差从 78 降到 3.6。

**成因二：matte 滞后于快速运动（仅动态出现）**

推理运行在视频线程之后，15 FPS 推理 / 30 FPS 视频时约滞后 2 帧。手臂快速挥动时，
matte 仍把手臂已经离开的区域标记为前景，那片区域实际上是墙，合成后就是跟在手臂
后面的白色拖影。实测滞后 2 帧时拖影亮度约 99（干净背景应为 20）。

处理方式：在 shader 中按「像素与实测墙色的距离」对 coverage 做渐变抑制。拖影像素
本身就是墙色，因此会被抑制；而实测人物像素与墙色的距离始终远大于容差，代价仅为
约 0.03% 的人物像素。这比等待 matte 追上要快，也不需要提高推理频率。

两项修正都依赖实测墙色：测量失败（画面中背景太少）时必须同时关闭，否则会凭空
猜一个颜色去染边或啃掉人物。

该方案针对的是「单色但非绿幕」的棚拍素材。若换成复杂背景，成因二的颜色键会失效，
应改走 RVM 时序抠像路线（见 3.2 P1）。

### 6.3 P1 后处理

- 运动补偿后的 mask 对齐
- 边缘保持上采样
- 头发区域细化
- 前景颜色去溢出
- 背景颜色和人物边缘的轻度融合
- RVM 循环状态推理

## 7. GPU 合成

新增 OBS effect，例如：

```text
plugins/obs-filters/data/background_replace.effect
```

核心逻辑：

```text
foreground = source texture
background = scene background texture
alpha      = mask texture
output     = lerp(background, foreground, alpha)
```

必须明确处理：

- straight alpha 与 premultiplied alpha
- 输入和输出色彩空间
- 背景宽高比和裁剪模式
- mask 的 UV 映射
- 视频背景尺寸变化
- OBS 允许的直接渲染路径

默认提供三种背景显示模式：

- 裁剪填充
- 等比适配
- 拉伸填充

## 8. obsvlive 代码落点

### 8.1 已新增文件

```text
plugins/obs-filters/background-segmentation-filter.c   滤镜主体
plugins/obs-filters/background-mask.c / .h             遮罩后处理
plugins/obs-filters/matting-ort.c / .h                 抠像专用 ORT 封装
plugins/obs-filters/matting-models.c / .h              模型家族规格表
plugins/obs-filters/data/background_alpha.effect       GPU 合成 Shader

plugins/obs-filters/background-mask-test.c             遮罩后处理测试
plugins/obs-filters/background-compat-test.c           滤镜挂载兼容性测试
plugins/obs-filters/matting-models-test.c              模型规格与识别测试
```

**未复用 `face-swap-ort.*`。** 该封装的输入数组固定为 `input_names[2]`，且 `input_count > 2` 会直接拒绝，同时每次调用结束释放全部输出。RVM 需要 6 个输入，并且要求 4 个输出在下一帧作为输入回传，必须跨调用持有 `OrtValue`。两者契约不兼容，因此新增 `matting-ort.c`，而不是改动 face swap 正在使用的代码路径。

### 8.2 修改文件

```text
plugins/obs-filters/CMakeLists.txt
plugins/obs-filters/obs-filters.c
plugins/obs-filters/data/locale/en-US.ini
plugins/obs-filters/data/locale/zh-CN.ini
```

参考实现：

```text
plugins/obs-filters/face-swap-filter.c
plugins/obs-filters/face-swap-ort.c
plugins/obs-filters/face-swap-preprocess.c
plugins/obs-filters/mask-filter.c
plugins/obs-filters/chroma-key-filter.c
```

## 9. OBS 属性设计

已实现的滤镜属性：

- `Model`：模型文件选择（扫描模型目录下的 `.onnx`）
- `Model Type`：自动 / MODNet / RobustVideoMatting
- `Inference FPS`：5、10、15、20、30
- `Mask Threshold`：默认 0.5
- `Edge Softness`：阈值软过渡带宽度
- `Feather`：边缘羽化半径
- `Expand / Shrink`：边缘扩张或收缩
- `Temporal Smoothing`：时域平滑强度
- `Show Mask`：调试模式，直接显示遮罩

`Model Type` 默认「自动」，按文件名推断（`rvm*` / `modnet*`）。推断失败时会记录警告并要求显式选择，而不是猜一个张量契约。即使推断错误也不会产生错误结果：`matting_ort_create_session` 会把规格与模型真实的输入输出名逐一核对，不匹配即失败。

模型目录建议：

```text
%APPDATA%/obs-studio/background-models/
```

模型加载失败时，属性页应显示明确错误，视频输出回退为原始源，不得输出黑屏。

## 10. 性能

### 10.1 线程数的影响

最初 `SetIntraOpNumThreads(options, 1)` 是从 face swap 抄来的。对人脸检测（模型小、调用频繁）合理，对全身抠像则严重浪费——实测在 10 核 CPU 上损失了约 3.5 倍性能。

`matting-threads-probe.c` 实测（256px）：

| 线程 | MODNet | RVM |
|---:|---:|---:|
| 1 | 83.6 ms | 60.0 ms |
| 2 | 46.3 ms (1.8x) | 33.6 ms (1.8x) |
| 4 | 25.6 ms (3.3x) | 21.2 ms (2.8x) |
| 6 | 23.0 ms (3.6x) | **16.6 ms (3.6x)** |
| 8 | 23.0 ms (3.6x) | 18.8 ms (3.2x) ← 回退 |

4 线程即获得大部分收益，6 线程最优，8 线程因调度开销和争用反而变慢。

因此 `matting_ort_thread_count()` 取 **物理核心数的一半、上限 6**。上限与线程数同样重要：OBS 还要同时渲染、编码、推流，占满所有核心会赢得基准测试但丢掉预览帧。

### 10.2 实测推理耗时

`matting-bench.c`，ONNX Runtime 1.23.2 CPU、256px、2 预热 + 10 计时帧，i5-13400（10 物理核）：

| 模型 | 修复前（单线程） | 修复后 | 提速 |
|---|---:|---:|---:|
| MODNet | 84.2 ms | **24.5 ms** | 3.4x |
| RobustVideoMatting | 61.7 ms | **19.7 ms** | 3.1x |

**RVM 比 MODNet 快约 20%**，与「循环模型更重」的直觉相反——其 MobileNetV3 骨干比 MODNet 轻，循环状态的开销小于骨干网络的差距。RVM 在速度和时序稳定性上都占优，唯一代价是 GPL-3.0。

### 10.3 延迟与默认推理帧率

推理帧率直接决定遮罩的最大陈旧程度：10 FPS 意味着遮罩在合成时可能已有 100ms 历史，这正是人走动时感到的延迟。

单线程时 MODNet 需要 84ms，无法支撑 15 FPS（66.7ms 预算），所以当时只能默认 10 FPS。多线程后两个模型都在 25ms 以内，默认值回到 **15 FPS**，遮罩陈旧度上限降至约 67ms。

快速移动场景可上调至 20 或 30 FPS；性能不足的机器降到 5 或 10。

### 10.4 遮罩陈旧时的渐隐

早期实现在遮罩超时后直接 `obs_source_skip_video_filter`，整幅原始画面（含真实背景）瞬间全部露出——这是用户反馈的「背景会显现」。硬切换比画面本身的瑕疵更显眼。

现在改为按陈旧程度渐隐：

```text
0 ~ 400ms     强度 1.0，完全应用
400 ~ 1000ms  线性衰减至 0
超过 1000ms   回退原始画面
```

`background_alpha.effect` 的 `mask_strength` 已支持该参数，渲染时传入实际强度而非固定 1.0。渐隐区间起点（400ms）高于所有可选推理帧率的间隔，因此 worker 正常工作时不会出现持续半透明。

该逻辑由 `background-fade-test.c` 覆盖：单调性、中点、边界、以及各推理帧率下正常工作时必须为全强度。

### 10.5 其余指标

| 指标 | 状态 |
|---|---|
| 输出帧率 | 不受推理影响，回读与合成均限频 |
| OBS 渲染线程阻塞 | 不等待模型（设计保证） |
| 无模型/推理失败 | 原始画面回退（已验证） |
| 遮罩陈旧 | 渐隐回退（已验证） |
| 端到端额外延迟 | 上限 ≈ 推理间隔 + 一帧渲染，未精确实测 |
| 内存增长 | **未实测** |
| GPU 回读开销 | **未实测** |

## 11. GPU 推理路线

### 11.1 P0

只要求 ONNX Runtime CPU Execution Provider，保证任何支持 OBS 的 Windows x64 机器都能运行。

### 11.2 P1

增加 DirectML，要求：

- 检测 provider 是否实际加载成功
- 记录实际使用的 provider
- provider 初始化失败时回退 CPU
- 不把 DirectML DLL 错误传播为 OBS 崩溃

### 11.3 不作为必选依赖

CUDA、TensorRT 不作为默认发行包的强依赖。它们会增加：

- CUDA/cuDNN/TensorRT 版本耦合
- 驱动和 DLL 分发问题
- 模型算子兼容问题
- 安装和故障诊断成本

## 12. 分阶段实施

### 阶段 A：模型获取（已完成）

模型放置目录（滤镜只扫描此处）：

```text
%APPDATA%\obs-studio\background-models\
```

MODNet（**已完成**）：

1. `git clone https://github.com/ZHKKKe/MODNet`
2. 下载预训练权重，用仓库 `onnx/` 目录的脚本导出
3. 用 `docs/inspect-onnx.py` 确认输入输出规格

RVM（**已完成**）：

1. 从 [Releases v1.0.0](https://github.com/PeterL1n/RobustVideoMatting/releases/tag/v1.0.0) 下载 `rvm_mobilenetv3_fp32.onnx`（官方直接提供 ONNX，无需自行导出）
2. 用 `docs/inspect-onnx.py` 确认规格

文件名保持包含 `modnet` / `rvm` 即可被「自动」识别；重命名后需在滤镜属性中显式选择 `Model Type`。

交付物：模型文件、许可证记录、SHA-256。

已记录：

| 模型 | SHA-256 | 大小 |
|---|---|---|
| `modnet.onnx` | `07C308CF0FC7E6E8B2065A12ED7FC07E1DE8FEBB7DC7839D7B7F15DD66584DF9` | 24.7 MB |
| `rvm_mobilenetv3_fp32.onnx` | `88D4531297118F595BF2FD60F6F566AEC2E559393802D1F436C380F0CBBD2828` | 14.3 MB |

RVM 实测规格，与代码假设完全一致：

```text
INPUTS  (6): src, r1i, r2i, r3i, r4i  float32 [batch, C, height, width]
             downsample_ratio         float32 [1]
OUTPUTS (6): fgr  [batch, 3, H, W]    pha  [batch, 1, H, W]
             r1o [.,16,.,.]  r2o [.,20,.,.]  r3o [.,40,.,.]  r4o [.,64,.,.]
```

注意 `fgr` 与 `pha` 空间尺寸相同，因此 RVM 必须按名取 `pha`；若沿用 MODNet 的按形状识别会选中前景图。该风险由 `matting-e2e-test.c` 的 cross-family 用例固化。

### 3.4 ONNX Runtime 依赖

`ONNXRUNTIME_ROOT` 未配置时，所有推理代码会被 `FACE_SWAP_HAS_ORT` 编译掉——插件仍能构建和加载，但滤镜静默变成直通，只有一行日志说明原因。这是一个很容易忽略的失败模式（本项目开发过程中即发生过）。

因此 `cmake/onnxruntime.cmake` 现在会自动探测约定位置：

```text
.deps/onnxruntime-<version>/
```

获取方式：

```text
https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/1.23.2
解压到 .deps/onnxruntime-1.23.2/
```

需包含：

```text
build/native/include/onnxruntime_c_api.h
runtimes/win-x64/native/onnxruntime.dll
```

构建时 `onnxruntime.dll` 会自动复制到 `obs64.exe` 同级目录——加载器搜索可执行文件目录，而非插件目录。

### 阶段 B：抠像滤镜（代码完成，待实机验证）

1. ~~新增 `background_segmentation_filter`~~
2. ~~同步滤镜 + GPU 回读，支持任意视频源~~
3. ~~mask 后处理和 GPU mask 上传~~
4. ~~双模型支持（MODNet 逐帧 / RVM 循环）~~
5. ~~日志、调试 mask 和失败回退~~
6. ~~用真实模型验证推理链路（`matting-e2e-test.c`）~~
7. 在 OBS 中用真实人像验证抠像质量 ← **需人工操作**

### 阶段 C：稳定性与质量

1. 增加时域平滑
2. 增加分辨率变化处理
3. 增加边缘羽化和形态学参数
4. 优化模型加载、线程退出和纹理更新
5. 增加长时间直播测试

### 阶段 D：一体化背景和 GPU 推理

1. 增加滤镜内图片背景
2. 增加滤镜内视频背景
3. 接入 DirectML

## 13. 测试要求

### 功能测试

- 图片背景静态显示
- 视频背景循环播放
- 背景源切换
- 场景切换和转场
- 开始/停止推流
- 开始/停止录制
- 摄像头断开和重连
- 摄像头分辨率切换
- 禁用和重新启用滤镜

### 质量测试

- 正常室内光线
- 逆光
- 低照度
- 复杂背景
- 头发、眼镜和耳机
- 快速挥手
- 人物进出画面
- 多人画面

### 稳定性测试

- 连续运行 8 小时以上
- 反复打开和关闭设置窗口
- 反复切换模型
- GPU 资源创建和销毁
- 推理线程异常退出
- ONNX Runtime DLL 缺失
- 模型文件损坏
- 低内存环境

## 14. 风险与应对

| 风险 | 影响 | 应对 |
|---|---|---|
| 权重许可证不明确 | 无法发行 | 只使用有明确再分发条款的权重，保留核验记录 |
| CPU 推理过慢 | 延迟和 mask 落后 | 降低推理频率、缩小输入尺寸、复用上一帧 mask |
| mask 闪烁 | 直播观感差 | EMA、置信度门限、RVM 评估 |
| 头发边缘粗糙 | 抠像质量差 | 羽化、边缘细化、评估 RVM |
| GPU 纹理同步错误 | 黑屏或崩溃 | 所有 GPU 资源在 graphics context 中创建/销毁 |
| 视频源尺寸变化 | 纹理错位 | 监听尺寸，清理旧 mask 并重新分配 |
| 内置视频 source 递归渲染 | 崩溃 | P0 使用场景背景源，P1 单独设计生命周期 |
| provider 加载失败 | 无法推理 | 自动回退 CPU，并在日志中记录原因 |

## 15. 结论

采用「MODNet / RVM 双模型 + ONNX Runtime CPU + OBS 场景背景源 + GPU Shader 合成」作为实现路线。

该方案满足：

- 本地离线处理，无云服务依赖
- 两个模型均为开源，与项目 GPL-3.0 发行方式相容
- 可挂载到摄像头、视频、图片、窗口捕获等任意视频源
- 支持图片和视频背景，解码由 OBS 负责
- 模型不随包分发，无权重再分发义务
- 允许后续接入 DirectML 和一体化背景滤镜

模型选择建议：

- **有绿幕**：优先用现有色度键滤镜，边缘质量更好且零推理开销
- **无绿幕**：优先 RVM——实测比 MODNet 快 27%，且具备时序稳定性，唯一代价是 GPL-3.0
- **需要宽松许可证**：MODNet（Apache-2.0）

第一阶段不把图片/视频解码器塞进抠像滤镜，也不把 GPU 推理作为硬依赖。

## 16. 验证状态

### 已用真实模型验证（`matting-e2e-test.c`）

两个模型均已通过 ONNX Runtime 实际加载并推理：

- 会话创建、输入/输出名解析
- 单帧推理产出值域正确的 matte
- RVM 4 组循环状态：跨帧保持、复用、重置后可恢复
- 张量长度不符时被拒绝，不会越界读取
- 用 MODNet 规格打开 RVM 被正确拒绝（防止误选 `fgr`）
- 文件名自动识别对两个官方命名均命中

### 已实测

- CPU 推理耗时（见第 10 节）

### 仍未验证

- **真实人像的抠像质量**——基准测试用的是合成色块，不能反映实际效果
- 端到端延迟
- `gs_stage_texture` 回读在不同 GPU 驱动上的同步开销
- RVM 循环状态在长时间直播下的表现，500ms 重置阈值是否合适
- 长时间运行的内存稳定性
- 在 OBS 中的实际合成效果（需人工操作）
