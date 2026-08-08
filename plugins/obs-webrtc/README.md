# ppobs 节点通道改造说明

基于 OBS Studio 32.1.2，扩展 obs-webrtc 插件以支持与 ppcenter 双活控制平面的通用节点通道通信。

## 变更总览

| 文件 | 类型 | 说明 |
|------|------|------|
| `plugins/obs-webrtc/ppcenter-node-proto.h` | 新增 | protobuf wire-format 编解码声明 |
| `plugins/obs-webrtc/ppcenter-node-proto.cpp` | 新增 | WorkerIndication/NodeMsgReq/NodeMsgRsp 手动编解码 |
| `plugins/obs-webrtc/ppcenter-node-channel.h` | 新增 | NodeChannelClient 类声明 |
| `plugins/obs-webrtc/ppcenter-node-channel.cpp` | 新增 | WebSocket + HTTP 双通道实现 |
| `plugins/obs-webrtc/whip-output.h` | 修改 | 增加 include / forward decl / 成员 |
| `plugins/obs-webrtc/whip-output.cpp` | 修改 | Setup 配置推导 + Start/Stop 集成 |
| `plugins/obs-webrtc/CMakeLists.txt` | 修改 | 新文件加入编译 |

## 功能描述

### 1. Protobuf 无线格式编解码（`ppcenter-node-proto`）

不依赖外部 protobuf 库，纯 C++17 实现 varint/zigzag 编码：

- **`WorkerIndication`**：字段 1-7（msgType, workerType, workerId, version, region, capacity, pipelines），匹配 `pb3/mmx.proto` 的字段编号，输出为 ppcenter WebSocket binary frame payload
- **`NodeMsgReq`**：解析 ppcenter 下发的指令（msgType, workerType, workerId, streamPath, signature, msgId, publishUrl）
- **`NodeMsgRsp`**：构建 ppcenter 回复（workerType, workerId, code, reason, msgId），作为 binary frame 返回

参考 mmxcontrol 的 Go 实现（`internal/mmxcontrol/client.go`），字段编号和类型完全一致。

### 2. 节点通道客户端（T-OBS-01）

`NodeChannelClient` 通过 WebSocket 连接到 ppcenter 的 `/ws/mmx` 端点：

- **RFC 6455 握手**：带有 `Authorization: Bearer <token>` 头
- **二进制帧通信**：客户端帧按规范 mask；心跳负载为 protobuf 编码的 `WorkerIndication`
- **服务器 Ping/Pong**：收到 opcode 0x9 回复 opcode 0xA
- **指令处理**：收到二进制帧后解析为 `NodeMsgReq`，通过回调分发，生成 `NodeMsgRsp` 后发送回 ppcenter
- **断开重连**：exponential backoff 自动重连
- **传输层抽象**：支持 raw socket (ws://) 和 TLS (wss:// 通过 libcurl CONNECT_ONLY)

### 3. HTTP 兜底通道（T-OBS-02）

WebSocket 连续断开达到阈值（默认 5 次）后自动切换到 HTTP 轮询：

- **心跳**：`POST /internal/mmx/v1/heartbeat`，body 为 protobuf 编码的 `WorkerIndication`
- **命令轮询**：`GET /internal/mmx/v1/commands/pending`
- **命令应答**：`POST /internal/mmx/v1/commands/response`
- **恢复**：HTTP 模式下定期尝试重连 WS，成功后切回主通道
- **迁移信令**：收到 `RECONNECT_HINT` 后主动断开 WS 并触发重连

### 4. WHIPOutput 生命周期集成

- **`Setup()`**：从 `ppcenter_url` 自动推导 `ws://host/ws/mmx` 地址，从设置项读取 token、role、region
- **`StartThead()`**：WHIP 连接建立后构造并启动 `NodeChannelClient`
- **`StopThread()`**：停止 NodeChannelClient
- **配置字段映射**：
  - `ppcenter_url` → WebSocket 目标地址
  - `ppcenter_secret` → Bearer 令牌
  - `ppcenter_node_id` → workerId
  - `ppcenter_region` → 区域标识

## 编译

使用 Visual Studio 2022 + CMake，在 OBS 构建树中：

```
cmake --build build_x64 --target obs-webrtc --config RelWithDebInfo
```

生成 `obs-webrtc.dll`（611KB，RelWithDebInfo）。

## 依赖

- WinSock2（Windows 套接字 API）
- libcurl（TLS 传输，HTTP fallback）
- OBS studio 32.1.2 SDK + obs-deps-2025-08-23-x64