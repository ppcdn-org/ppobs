# ppobs 头像口型 worker

模式 A（MuseTalk）的推理进程。不随 OBS 编译，也不进主安装包 —— 见
`docs/avatar-lipsync-design.md` 第 6 节：3.4GB 的 UNet 和 CUDA 运行时不能塞进 OBS 进程，
推理崩溃或显存耗尽不能把直播一起带走。

## 协议

`plugins/obs-avatar/avatar-sidecar-protocol.h` 是唯一权威。

**插件是管道的服务端，worker 是客户端。** 这是刻意的：从 Python 连一个命名管道就是
`open(r"\\.\pipe\name", "r+b")`，而*创建*一个则需要 pywin32。顺带还让访问控制归 OS 管，
而不是任何本机进程都能连的 localhost 端口。

字节序一律小端。每条消息是 8 字节头（uint32 负载长度、uint16 类型、uint16 保留必须为 0）
加负载。`--selftest` 里固定的那几串字节与 `avatar-protocol-test.c` 里的是同一组 —— 两种
语言对不上会在测试里炸，而不是在直播画面上变成一团花屏。

## 跑起来

插件在源启用时创建管道，并把管道名打进 OBS 日志：

```
[obs-avatar sidecar: '头像直播'] waiting for a worker on \\.\pipe\ppobs-avatar-1234-000001ab...
```

把那个名字抄给 worker：

```bash
python ppobs_avatar_worker.py --pipe "\\.\pipe\ppobs-avatar-1234-000001ab"
```

当前阶段**由你手动启动**。插件侧的 `worker_command` 设置已经留好，但拉起进程和退避重拉
还没实现（属于阶段 D 的生命周期管理）—— 手动启动的好处是 worker 的输出直接在终端里看得见，
排查协议问题比翻日志快得多。

检查线格式，不需要管道也不需要 OBS：

```bash
python ppobs_avatar_worker.py --selftest
```

## 两个引擎

| `--engine` | 说明 |
|---|---|
| `auto`（默认） | 有可用的 MuseTalk 就用，否则回落到 stub 并发一条 LOG 说明原因 |
| `stub` | 强制用占位引擎 |
| `musetalk` | 强制用真模型；不可用则报 ERROR 退出，而不是悄悄降级 |

**stub 引擎**不需要任何第三方依赖（纯标准库），画一张占位脸，嘴的开合跟着音频电平走。
它的作用不是好看，而是让整条链路在 3.4GB 权重到位之前就能验证：取音 → 重采样 →
管道 → 帧 → 上屏。盯着那张嘴对自己说话，还能直接看出端到端延迟 —— 那正是阶段 D 要补偿的
那个数。

**MuseTalk 引擎**还没实现。它会在构造时加载准备好的头像包（coords / latents / mask）
以及 whisper-tiny、UNet、VAE decoder，然后每个音频窗跑一次
whisper → UNet → VAE decode → 融合。

## 权重放哪

插件启动时会把资源目录打进日志：

```
[obs-avatar] asset and model directory: ../../config/background-models
```

便携模式下即 `build_x64/rundir/RelWithDebInfo/config/background-models`。MuseTalk 权重放
`background-models/musetalk/`，准备好的头像包放 `background-models/avatars/<id>/`。

## 许可证

并入发行包前必须逐条核验 MuseTalk 依赖权重的商用授权，见
`docs/avatar-lipsync-design.md` §4.3。MuseTalk 自己的代码是 MIT、官方声明权重可商用，但
**SD v1.4 衍生的 UNet** 和**人脸检测／解析权重**这两项还没核验完，后者风险最高（部分
mmpose/dwpose 系权重为非商用）。这是阻断项，不是待办。
