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

**默认由插件自己拉起。** 把口型驱动选成「真人（MuseTalk worker）」，插件就会创建管道、
启动 worker、等它连上；worker 退出或崩溃后按 1s、2s、4s……（上限 30s）的间隔重新拉起，
连续稳定运行 10 秒以上才把间隔重置。worker 在一个 Windows job object 里，OBS 即使崩溃，
worker 也会被一并收掉，不会留一个占着显存的孤儿进程。

属性里有五项（只在 MuseTalk worker 模式下显示）：

| 属性 | 留空时 |
|---|---|
| 自动启动 worker | 默认勾选；取消则回到手动模式（见下） |
| 口型幅度 | 默认 200%，运行时生效（经 `MOUTH_GAIN` 消息发给 worker，不用重启） |
| MuseTalk 目录 | 默认 `D:\github\MuseTalk`，环境变量 `MUSETALK_DIR` 可改默认值 |
| Python 解释器 | 用 `<MuseTalk 目录>\venv\Scripts\python.exe`；没有 venv 才退到 PATH 里的 `python` |
| worker 脚本 | 用编译时记下的源码树里的 `ppobs_avatar_worker.py` |

**口型幅度**：MuseTalk v1.5 在不少脸上嘴只张一点点，模型里没有对应的旋钮，把音频放大也没用（Whisper
特征做了归一化）。worker 启动时先算一遍静音下 UNet 的输出作为"嘴的静止形状"，之后每一帧输出
`静止 + 幅度 × (输出 - 静止)`，在隐空间里放大偏移。实测（`greg02.jpg`）100%、150% 几乎看不出张嘴，
200% 清楚，300% 开口更大但嘴角出现发白的伪影，所以默认 200%、范围 50%–300%。

**生成帧率**：本机 RTX 4060 上 MuseTalk 约 40 ms 一帧（GPU 空闲时），所以 30 fps 比实时还慢，
25 fps 刚好没有余量，OBS 自己的编码器还要分走一部分 GPU。默认 20 fps。worker 追不上时，
插件只保留最近 1 秒的音频、更早的丢掉，并在日志里报
`the worker is not keeping up with real time` —— 嘴会滞后、掉帧，但 OBS 不会被拖住。

MuseTalk 必须用它自己 venv 里的解释器 —— torch、CUDA 都装在那里，系统 Python 没有。

worker 的 stdout/stderr 写到 OBS 日志旁边的 `logs/avatar-worker.log`（便携模式下是
`config/obs-studio/logs/`）。解释器找不到、import 失败这类问题发生在握手之前，只有这个文件
能看到。每次启动前会写一行 `---- launching worker ----` 和完整命令行；OBS 本身的日志里也会
记 `started the worker (pid …)`、`worker exited with code …` 和下一次重拉的间隔。

配置的路径不存在时，插件在日志里报错并退回手动模式，而不是反复重拉一个注定起不来的命令。

### 手动启动

取消「自动启动 worker」后，插件只创建管道并等待，管道名打在 OBS 日志里（也显示在属性页）：

```
[obs-avatar sidecar: '头像直播'] waiting for a worker on \\.\pipe\ppobs-avatar-1234-000001ab...
```

把那个名字抄给 worker：

```bash
python ppobs_avatar_worker.py --pipe "\\.\pipe\ppobs-avatar-1234-000001ab"
```

好处是 worker 的输出直接在终端里看得见，排查协议问题比翻日志快得多。

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
