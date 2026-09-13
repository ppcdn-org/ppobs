## ppobs v32.1.2-rc002

本版本包含 WHIP/P2P 推流增强、HEVC/H264 多轨同播、SRT 同播、直播设置优化、画质处理能力及稳定性修复。

### WHIP / P2P 推流

- 新增 WHIP HEVC + H264 多轨推流
  - H264 与 HEVC 使用独立 WHIP 会话
  - 两路会话分别携带音频
  - 支持 H264/HEVC 多层 Simulcast
  - 自动匹配对应厂商的 H264/HEVC 编码器
  - HEVC 编码器不可用时提供明确提示
- 新增 WHIP P2P NAT 探测
  - 推流前探测发布端 NAT 类型
  - 持久化设备 ID
  - NAT 探测失败时自动降级为 Edge-only，不阻断推流
- 新增 PPCenter STUN 服务下发与配置
- 增强 P2P WebSocket 信令安全
  - 远程信令强制使用 `wss://`
  - `ws://` 仅允许 localhost/loopback
  - 修复 IPv6 loopback 地址处理
- 修复 WHIP 连接建立、断线重连和停止流程
  - 更快恢复断线
  - 增加 reconnect watchdog
  - 限制重连节奏，避免重复重连线程
  - 修复连接关闭时的数据发送竞态
  - 修复输出停止后 degrade WebSocket 残留重连
  - 修复 WHIP 输出失败后编码器仍处于 active 状态的问题
  - 修复 WHIP 输出卡在 0 bitrate 的问题
- 支持 `{ppcenter_appid}` 展开
  - WHIP endpoint 支持 App ID 占位符
  - 自定义 SRT URL 的 `streamid` 同样支持 App ID 占位符

### Simulcast / SRT

- 新增单条 SRT 连接承载多路 Simulcast
  - 每个视频编码器输出独立 H264 elementary stream
  - 支持按 `track_idx` 路由视频包
  - 每层独立等待关键帧后开始发送
  - 保留高到低的 Simulcast 层级顺序
  - 为每层创建独立 FFmpeg `AVStream` 和编码状态
- SRT Simulcast 限制为最多 4 层
- WHIP Simulcast 同样限制为最多 4 层
- SRT 多路视频要求所有视频编码器均为 H264
- 修复硬件编码器首个 H264 Access Unit 兼容性
  - 支持合法 Annex-B 前导零
  - 支持四字节长度 AVCC 转 Annex-B
- 初始化失败时正确清理 FFmpeg/MPEG-TS 状态
- 自定义服务不再显示不适用的独立“多轨视频设置”
- 保留自定义服务与 WHIP 的统一“同播配置”入口

### 推流限制

WHIP 推流增加硬性限制，适用于所有启动方式和重连路径：

- 横屏最大分辨率：`3840x2160`
- 竖屏最大分辨率：`2160x3840`
- 最大 FPS：`60`
- 最大视频码率：`10000 Kbps`
- H264、HEVC 及所有 Simulcast 层分别校验
- 超限时停止输出并记录明确错误
- 设置界面提前提示分辨率和 FPS 超限

### 直播设置

- 自定义服务与 WHIP 统一配置项目：
  - 终点
  - 同播
  - 认证中心
  - 区间直播配置
- Custom 和 WHIP endpoint 分开记忆
  - 在两种服务之间切换时不会串用终点地址
- 自定义服务也显示认证中心配置
- 自定义服务认证中心字段会保存到对应服务设置
- 自定义服务保留用户选择的音频编码器
  - 不再因为协议推荐值自动切换或隐藏 Opus 等编码器
- 同播编码器数量统一限制为 4 层

### 画质与视频处理

- 新增 Clarity 清晰度滤镜
- 新增 Temporal Denoise 时域降噪滤镜
- 新增基础 Beauty 美颜滤镜
- 新增人脸美化/风格化处理能力
- 新增 ROI 相关能力
  - 手动 ROI 框选
  - ROI 与 WHIP 服务设置解耦
  - ROI 设置移动到视频设置页面
- 新增直播质量评分
  - 计算视频质量分数和 PSNR
  - 支持质量下降提示
  - 支持质量评分预览覆盖层
- 调整质量处理默认值和 UI 文案
- 增强 QSV/硬件编码器码率上限处理

### 网络与时钟

- 新增 Windows 网络适配器类型识别
  - Ethernet
  - Wi-Fi
  - Cellular
  - Other/Unknown
- RTMP 和 WHIP 日志增加：
  - 网卡描述
  - 网卡类型
  - 上下行速率
- 新增启动时活动网络适配器日志
- 改进 NTP 校正时钟
  - 将校正后的时钟锚定到系统墙上时间
  - 提高推流时间戳稳定性

--------------------
ppobs v32.1.2-rc002
This version includes enhanced WHIP/P2P streaming, HEVC/H264 multi-track simultaneous broadcast, SRT simultaneous broadcast, optimized live streaming settings, improved image quality processing capabilities, and stability fixes.

# WHIP / P2P Streaming
Added WHIP HEVC + H264 multi-track streaming
H264 and HEVC use separate WHIP sessions
Each session carries audio separately
Supports multi-layer Simulcast for H264/HEVC
Automatically matches the corresponding vendor's H264/HEVC encoder
Provides explicit prompts when the HEVC encoder is unavailable
Added WHIP P2P NAT detection
Detects the publisher's NAT type before streaming
Persistentizes device ID
Automatically downgrades to Edge-only when NAT detection fails, without blocking streaming
Added PPCenter STUN service deployment and configuration
Enhanced P2P WebSocket signaling security
Forces remote signaling to use wss://
ws:// only allows localhost/loopback
Fixed IPv6 loopback address handling
Fixed WHIP connection establishment, reconnection, and termination processes
Faster disconnection recovery
Added reconnect watchdog
Limit reconnection frequency to avoid duplicate reconnection threads
Fixed data transmission race condition when connection is closed
Fixed residual reconnection after degrade WebSocket stops
Fixed issue where the encoder remained active after WHIP output failure
Fixed issue where WHIP output was stuck at 0 bitrate
Support for {ppcenter_appid} expansion
WHIP endpoint supports App ID placeholders
Custom SRT URL's streamid also supports App ID placeholders

# Simulcast / SRT
Added single SRT connection to carry multiple Simulcast streams
Each video encoder outputs an independent H264 elementary stream
Support for routing video packets by track_idx
Each layer independently waits for a keyframe before starting transmission
Preserves the high-to-low Simulcast hierarchy order
Creates independent FFmpeg AVStream and encoding state for each layer
SRT Simulcast is limited to a maximum of 4 layers.
WHIP Simulcast is also limited to a maximum of 4 layers.
SRT multi-channel video requires all video encoders to be H.264.
Fixed compatibility issues with the first H.264 Access Unit of the hardware encoder.
Supports valid Annex-B leading zeros.
Supports 4-byte AVCC to Annex-B conversion.
Correctly cleans up FFmpeg/MPEG-TS state when initialization fails.
Custom services no longer display inapplicable independent "Multitrack Video Settings".
Retains the unified "Simultaneous Playback Configuration" entry for custom services and WHIP.

# Push Streaming Limitations
WHIP push streaming has added hard limits, applicable to all startup methods and reconnection paths:
Maximum landscape resolution: 3840x2160
Maximum portrait resolution: 2160x3840
Maximum FPS: 60
Maximum video bitrate: 10000 Kbps
H264, HEVC, and all Simulcast layers are validated separately.
Output stops and a clear error is logged when limits are exceeded.
Settings interface provides advance warning of resolution and FPS exceeding limits.

# Live Streaming Settings
Unified configuration items for custom services and WHIP:
Endpoint
Simultaneous Broadcast
Authentication Center
Interval Live Streaming Configuration
Custom and WHIP endpoints are remembered separately.
Endpoint addresses will not be used interchangeably when switching between the two services.
Authentication center configuration is also displayed for custom services.
Custom service authentication center fields are saved to the corresponding service settings.
Custom services retain the user-selected audio encoder.
No longer automatically switching or hiding encoders such as Opus based on protocol recommendations.
Simultaneous broadcast encoders are uniformly limited to 4 layers.

# Image Quality and Video Processing
Added Clarity filter
Added Temporal Denoise filter
Added basic Beauty filter
Added face beautification/stylization capabilities
Added ROI-Related Capabilities
Manual ROI selection
Decoupling ROI from WHIP service settings
Moving ROI settings to the video settings page
Adding live streaming quality score
Calculating video quality score and PSNR
Supporting quality degradation alerts
Supporting quality score preview overlays
Adjusting quality processing defaults and UI text
Enhancing QSV/hardware encoder bitrate cap handling

# Network and Clock
Adding Windows network adapter type recognition
Ethernet
Wi-Fi
Cellular
Other/Unknown
Adding the following to RTMP and WHIP logs:
Network interface card description
Network interface card type
Uplink and downlink speeds
Adding active network adapter logs at startup
Improved NTP clock calibration
Anchoring the calibrated clock to the system wall time
Improved streaming timestamp stability
