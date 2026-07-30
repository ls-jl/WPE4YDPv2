# 云游戏 WebRTC 排障记录

本文记录 Direct WPE 浏览器适配云原神时已经验证的结论、排障顺序和清理要求。目标是优先用证据定位层级，避免围绕无害日志反复修改 WebKit、GStreamer 或 DRM。

## 防止陷入 Bug 循环

每轮调试必须遵守以下规则：

1. **先写假设再动代码**：明确待验证层级、预期日志和失败判据。没有可观测判据时不改实现。
2. **一次只改一个变量**：WebKit、WPE、GStreamer、DRM、页面状态和网络环境不能在同一轮同时变化。
3. **记录部署身份**：保存源码 commit、AMR SHA256、关键 ELF SHA256、安装槽和进程 maps。只看到文件已覆盖不能证明进程已加载新版本。
4. **验证完整链路**：单条成功日志不能代替终态。ICE 要看到 nominated 和 completed；视频要同时看到解码器、时间推进、NV12 framebuffer 和可见画面。
5. **已排除层级不回退**：除非出现与旧结论矛盾的新证据，否则不重新修改证书、candidate、DRM 或解码器。
6. **临时改动必须成对恢复**：修改前备份，测试后恢复并校验；数据库、`run.sh`、hosts mount、日志级别和临时测试页都在范围内。
7. **失败也要落记录**：记录命令、输入、输出、耗时和结论。不能只写“没用”或“还是卡”。

建议每轮使用下面的记录模板：

```text
问题：
假设：
唯一改动：
部署身份：
观测命令：
成功判据：
实际结果：
结论：
回滚/清理：
下一步：
```

## 已验证基线

2026-07-24 在 `YoudaoDictionaryPen-794`（Linux `5.10.160`、`960x266`、旋转 `270`）验证：

- 包内 WebRTC loopback 可完成 Offer/Answer、ICE `connected/completed`、DTLS、SCTP 和 DataChannel ping/pong。
- 云游戏域名下 Answer 端 ICE role 修复已通过受控映射测试：本地 Answer 后重新设为 controlling，可产生 `USE-CANDIDATE`、`nominated=1` 并完成 DataChannel。
- 禁止摄像头/麦克风不会影响接收远端媒体和 DataChannel。
- H.264/H.265 的 `mppvideodec` 已被 WebKit 识别为硬件解码器。
- 包内 H.264 烟测已验证 `mppvideodec -> NV12 DMA-BUF -> DRM video overlay`，DRM plane 出现非零 NV12 framebuffer。
- Skia raster -> SHM -> 旋转 dumb buffer -> DRM 的网页路径可工作。
- DRM 视频 overlay plane 可用，但只有实际视频帧到达后才会出现非零 framebuffer。
- `960x266` 设备上对云游戏页面使用 `0.5` page zoom 后，奖励弹窗和底部按钮可完整显示；普通网页保持 `1.0`。

## 正确排障顺序

1. **页面和信令**
   - 查主页面是否加载完成、WebSocket 是否建立、是否有业务错误码。
   - 未出现 `setRemoteDescription` 前，不要排查视频解码或 DRM plane。
2. **WebRTC 协商**
   - 依次确认 Offer/Answer、远端 candidate、ICE、DTLS、SCTP/DataChannel。
   - ICE 未 connected 时，视频解码器和视频 plane 为空属于预期。
3. **媒体管线**
   - 确认实际选中 H.264、创建 `mppvideodec`、输出 NV12 DMA-BUF。
4. **DRM 出屏**
   - 最后检查视频 overlay socket、plane framebuffer、尺寸和 zpos。
5. **性能**
   - 对比 WebKit compose、WPE copy 和 DRM commit；不要只看肉眼帧率。

## 容易误判的日志

### H.264 探针主动 `close()`

云原神 SDK 会创建临时 `RTCPeerConnection`，生成包含 `H264/90000` 的 Offer，然后主动关闭。日志中的以下序列是能力探测成功，不是会话崩溃：

```text
Applying cloud-game quirk, forcing max-bundle policy
Created SDP offer ... H264/90000
close:<webkit-webrtc-pipeline-0> Closing
```

只有后续真实会话没有进入 ICE/DTLS 时才继续排查。

### ICE `generation` warning

服务端 candidate 可能带：

```text
generation 0 ufrag <value> network-id 1
```

WebKit 2.53.3 + GStreamer 1.22 会打印 `Unsupported 'generation' ICE candidate field`。设备回归已证明保留这些字段仍可达到 ICE completed 和 DataChannel PASS，因此该 warning 本身不是连接失败原因，不要盲目删除 candidate 字段。

### Candidate valid 但没有 nominated

`libnice` 显示 connectivity check 成功，不代表 ICE 已连接。必须同时确认候选对被提名：

```text
valid pairs > 0
nominated pairs > 0
ICE connected/completed
```

云游戏服务端可能发送 Offer，却不主动发送 `USE-CANDIDATE`。GStreamer 1.22 把 Answer 端设为 controlled 后，双方会一直等待 nomination。更隐蔽的是：远端 Offer 应用后强制 controlling 仍不够，设置本地 Answer 时 `webrtcbin` 会再次恢复 controlled。修复必须在远端描述和本地描述完成后都执行，日志应依次出现：

```text
Applying cloud-game ICE quirk after remote description
Applying cloud-game ICE quirk after local description
```

随后确认 STUN 请求带 `USE-CANDIDATE`、`nominated > 0`。不要通过删 candidate 扩展、放宽 TLS 或改 DRM 来绕过这个状态机问题。

真实云原神会话进一步证明，**回调后改 role 仍然可能太晚**。GStreamer
在 `set-remote-description` 内创建 ICE stream 并启动 conncheck；libnice
会拒绝已开始检查后的 role 切换：

```text
Property set, role switch requested but conncheck already started
```

因此云游戏 Answer 端必须在调用 `set-remote-description` 之前先把 ICE
agent 设为 controlling。随后 remote description 和 local answer 回调后的
重复设置只作为校验。定向日志的正确顺序应为：

```text
Applying cloud-game ICE quirk before remote description
we are in ice controlling mode: true
Applying cloud-game ICE quirk after remote description
Setting local description
we are in ice controlling mode: true
```

若本地 Answer 阶段仍打印 `controlling mode: false`，该轮修复未生效，继续
等待 6 秒只会得到 `checking -> failed`。

### WebSocket 证书过期

`*.mhystatic.com:5443` 当前可能返回过期证书。内核例外只允许：

- `WPE_CLOUDGAME_ALLOW_EXPIRED_TLS=1`
- `wss://`
- 主机后缀严格为 `.mhystatic.com`
- 端口 `5443`
- 唯一错误为 `G_TLS_CERTIFICATE_EXPIRED`

日志出现 `Allowing expired cloud-game WSS certificate` 仅表示 TLS 证书回调已放行，不等于 WebSocket 或业务鉴权成功。仍需继续观察连接建立、服务端关闭和业务错误码。

### `mppvideodec` 被误判为软件解码

Rockchip 只启用了 hole-punch quirk 时，旧 registry scanner 不会调用硬件分类器，导致：

```text
factory=<mppvideodec> isUsingHardware=false
```

修复位于：

```text
wpe-drm/webkit-patches/0002-registry-scanner-classify-mpp-as-hardware.patch
```

正确日志应包含：

```text
Setting <mppvideodec> as hardware accelerated Rockchip MPP decoder
isUsingHardware=true
```

### MediaStream 视频被“不可见视频”策略暂停

全屏 `<hole>` 和 DRM overlay 下，WebKit 可能判断云游戏的 muted
`MediaStream` video 不在页面 viewport 内，并把播放器挂起。典型日志为：

```text
Muted video player not visible in viewport
Play (state change saved due to suspend)
```

这种情况下 ICE、DTLS、DataChannel 和 H.264 RTP 都可以正常，但 MPP 管线
不会真正进入播放。诊断时设置：

```sh
WEBKIT_GST_ALLOW_PLAYBACK_OF_INVISIBLE_VIDEOS=1
```

2026-07-25 实机验证该开关能让 `mppvideodec` 创建并启动。它只是解除
MediaPlayer suspend，不等于已经解码成功；仍需检查 `frames-received`、
`frames-decoded` 和 NV12 plane。

### 关键帧早于硬件解码消费者就绪

云游戏首帧失败的一组已验证时序为：

```text
23.372  incoming track 请求关键帧
23.440  收到 SPS/PPS/IDR
23.481  创建 mppvideodec
23.490  mppvideodec started
23.513  mppvideodec drain/reset
```

这说明服务器响应 PLI 正常，但 IDR 在 MediaStream 播放管线和 MPP decoder
就绪前已经通过。结果是：

```text
frames-received=0
frames-decoded=0
key-frames-decoded=0
video overlay fb=0
```

不要据此再次修改 ICE、DTLS、H.264 SDP 或 MPP 分类；下一步应在真实消费者
就绪后，从 `GStreamerIncomingTrackProcessor` 持有的 webrtcbin source pad
直接有限重试 PLI。

2026-07-25 已否定一条看似合理但无效的路径：从
`WebKitMediaStreamSrc` 的 appsrc source pad 发送 ForceKeyUnit，再经
`RealtimeIncomingSourceGStreamer::handleUpstreamEvent()` 代理。定时器按
250/750/1500ms 正常执行，事件也到达代理函数，但全部返回 `rejected`，服务端
没有发送第二个 IDR：

```text
Requesting a WebRTC key-frame for consumer caps: rejected
Requesting a WebRTC key-frame for late consumer +250ms: rejected
Requesting a WebRTC key-frame for late consumer +750ms: rejected
Requesting a WebRTC key-frame for late consumer +1500ms: rejected
```

因此不要重复走 appsrc 代理 PLI。直接发送初始 PLI 的
`GStreamerIncomingTrackProcessor::m_pad` 已被真实服务端证明有效。
下一轮候选补丁暂存于：

```text
wpe-drm/webkit-patches/0003-webrtc-direct-keyframe-retry.patch
```

2026-07-28 已先恢复 PVE 上的 appsrc 失败实验，再将该补丁应用到
`GStreamerIncomingTrackProcessor.cpp`。ARM64 `-j72` 编译通过，候选库
SHA256 为：

```text
0885a178c504adddf6f947de60df54a6296f1cc79e25b4d3c2dd03c4f3785d08
```

候选已临时部署到设备，原库以
`libWPEWebKit-2.0.so.1.10.2.pre-direct-keyframe-20260728` 保留。

2026-07-28 的真实云原神串流已证明 direct-pad 请求有效：

```text
01:25.842  initial Requesting a key-frame
01:25.907  SPS
01:25.912  PPS
01:25.914  IDR
01:25.941  mppvideodec started
01:26.158  Late consumer key-frame request +250ms: sent
01:26.292  SPS/PPS
01:26.297  IDR
01:26.658  Late consumer key-frame request +750ms: sent
01:26.707  SPS/PPS
01:26.713  IDR
01:27.408  Late consumer key-frame request +1500ms: sent
```

ICE 从 `checking` 到 `connected/completed`，PeerConnection 进入 `connected`，
DTLS ready，SCTP association established。不要再把当前问题归因于 ICE、
DataChannel、服务器不响应 PLI 或缺少关键帧。

但是 `mediastream-media-player-1` 仍停在 `PAUSED`、pending `PLAYING`，
`frames-received/frames-decoded` 始终为 0，plane 89 的 `fb=0`。MPP 在
`start` 后执行一次 `drain/reset`，没有错误消息。当前阻塞已经缩小到：

```text
webrtcbin encoded sample
  -> RealtimeIncomingSourceGStreamer handoff
  -> WebKitMediaStream appsrc
  -> decodebin3
  -> mppvideodec
  -> WebKit appsink/video overlay
```

设备已改用聚焦日志类别
`webkitwebrtcincoming/webkitmediastreamsrc/appsrc/decodebin3/mppdec`，等待
下一次用户点击“开始游戏”后确认 sample 在哪一层停止。该调整只改变日志，
不改变媒体行为。

## 帧率判断

日志中的三段耗时分别代表不同层：

```text
ThreadedCompositor ... compose_avg_ms=...
Frame window ... wpe_receive_fps=...
WPEViewDRM ... copy_avg_ms=... commit_avg_ms=...
```

若 compose 为 `90–230ms`，而 copy 约 `5ms`、commit 约 `1–4ms`，瓶颈在复杂网页的 Skia CPU 合成，不在 DRM。实际云游戏视频应走 MPP + NV12 overlay，不应按网页合成 FPS 判断视频 FPS。

## 可重复验证

### WebRTC loopback

包内测试页：

```text
assets/wpe-runtime/tests/webrtc-loopback.html
```

追加 `?candidateExtensions=1` 可测试 Chrome 风格 candidate 扩展。预期终态：

```text
WEBRTC_TEST PASS ICE + DataChannel
PASS WebRTC ICE DataChannel
```

测试时先备份 `browser.sqlite3`，停止所有 WPE 子进程后再修改活动标签。完成后恢复数据库，并删除 `-wal`、`-shm` 临时文件，避免测试 URL 留在用户状态中。

要验证只对 `mihoyo.com` 生效的 ICE controlling quirk，可在设备上临时把一个位于 `/tmp` 的 hosts 文件 bind mount 到只读的 `/etc/hosts`，把 `ys.mihoyo.com` 指向局域网测试服务器。严禁直接覆盖系统 hosts。预期日志必须同时包含：

```text
Applying cloud-game ICE quirk after remote description
Property set, changing role to "controlled"
Applying cloud-game ICE quirk after local description
set cand_use=1
1 nominated
PASS ICE + DataChannel
```

测试后先停止所有 WPE 进程，再恢复数据库和 `run.sh`，最后 `umount /etc/hosts`。必须检查 mount 列表，确认临时映射已经移除。

### MPP 与实际加载库

不要硬编码 MiniApp 的 `a/b` 安装槽，应从进程 maps 获取实际路径：

```sh
P=$(pidof WPEWebProcess | awk '{print $1}')
grep libWPEWebKit /proc/$P/maps
grep -E 'mppvideodec|isUsingHardware|Hardware decoding supported' \
  "$APP_DATA/browser/wpe-drm.log"
```

### DRM 视频 plane

```sh
cat /sys/kernel/debug/dri/0/state
```

在远端视频尚未开始时，视频 plane `fb=0` 正常。只有解码后已提交 NV12 framebuffer，才应看到 plane 绑定 CRTC、非零 fb 和正确 `crtc-pos`。

包内 MPP 烟测页为：

```text
assets/wpe-runtime/tests/mpp-video.html
assets/wpe-runtime/tests/mpp-smoke.mp4
```

测试成功需要同时满足：页面日志持续增加 `currentTime`、出现 `mppvideodec`、video overlay socket 已连接、DRM 视频 plane 为非零 `NV12` framebuffer。仅有 `playing` 或仅有 MPP 日志都不足以证明视频已经出屏。

### 不依赖截图 API 检查 WPE 画面

`miniapp_cli capture` 可能只捕获 MiniApp plane。需要检查 WPE framebuffer 时，使用只读工具：

```text
tools/drm_fb_dump.c
```

先从 `/sys/kernel/debug/dri/0/state` 取得 WPE plane 的 framebuffer ID，再导出 PPM。工具只执行 `GETFB2`、dumb map 和读取，不提交 framebuffer，也不改变 plane。旋转设备拉回图片后必须按实际 panel rotation 旋转再判断，避免把坐标问题误判成页面裁剪。

2026-07-25 的实机 framebuffer 证明云原神已加载到“每日登录奖励”弹窗，但 `960x266` 超矮 viewport 将弹窗下方操作区裁掉。这是当前页面布局阻塞，不是 ICE、MPP 或 DRM 无输出。修复应优先验证云游戏域名专用 page zoom；不要为此再次修改 WebRTC candidate 或视频 overlay。

### 云游戏输入模式不能按域名立即切换

云原神域名包含普通 HTML 奖励、排队和启动按钮。`auto` 一进入域名就切到
game profile 时，WPE 只发送 touch down/move/up，不生成 pointer click；
实机同一坐标 A/B 证明：

- `game`：触摸进入 WebKit，但“我知道了”无响应。
- `browser`：生成 pointer tap 后弹窗立即关闭。

正确策略是页面加载阶段保持 browser profile，在真正的 `<video>` 开始播放
后再切换 game profile。视频停止后延迟恢复 browser，避免 WebRTC 缓冲瞬间
反复切换。不要用云游戏域名、创建 `RTCPeerConnection` 或生成 Offer 作为
“已进入游戏”的判据；页面启动时就会执行 WebRTC 能力探针。

### 自动触摸测试

`tools/uinput_touch_injector.c` 是仅用于设备诊断的标准 `/dev/uinput` 工具，
不会打入 AMR。它创建虚拟多点触摸设备并从 FIFO 接收：

```text
tap RAW_X RAW_Y
swipe X1 Y1 X2 Y2 STEPS DURATION_MS
```

WPE 测试时可临时把 `WPE_TOUCH_DEVICE` 指向该 event 节点。当前 5.10.160
内核还实测支持通过 `tools/evdev_touch_injector.c` 写入现有
`/dev/input/event4`，事件会广播给已打开该节点的 WPE。后者更适合无人值守
点击，因为它不替换真实触摸设备，用户物理触摸仍然有效；但工具只允许放在
设备 `/tmp`，不得打包进 runtime，也不得把生产 `WPE_TOUCH_DEVICE` 改成临时
event 节点。

uinput 会抑制与上次相同的 ABS 值。若 WPE 重启后第一次点击仍是同一坐标，
只发送 tracking ID 而没有 X/Y，WPE 会因坐标未初始化而丢弃。诊断工具在
每次 down 前先写相邻坐标再写目标坐标，保证重复点击也产生完整事件。

uinput 只能通过一次性启动环境传入，禁止把虚拟 event 节点写进包内
`run.sh`。测试结束后必须同时确认：

```sh
grep -n 'WPE_TOUCH_DEVICE' "$RUNTIME/run.sh"
ps | grep uinput-touch-injector
grep 'WPE launch:' "$WORKDIR/wpe-drm.log" | tail -n 1
```

正式启动日志中的 `touch_device` 必须来自系统配置或
`/dev/input/by-path/hyn_ts`，不能是 `/dev/input/event10` 之类的临时节点。
如果物理触摸突然完全失效，先检查这一项，再排查坐标矩阵或 WebKit 输入。

## 设备清理规范

每次临时测试结束必须：

1. 终止全部 `wpe-drm-minimal`、`WPEWebProcess`、`WPENetworkProcess`。不要假设一次 `pidof name1 name2...` 能枚举所有多标签 WebProcess；使用 `killall -TERM` 后再用 `ps` 确认零残留。
2. 恢复浏览数据库和默认 `run.sh`。
3. 删除临时 HTML、registry、日志级别备份和测试载荷。
4. 重启浏览器后检查活动 URL、进程 maps 和 DRM state。
5. 确认没有测试 HTTP 服务、残留 socket 或测试 Profile。

热替换 `libWPEWebKit` 前必须先完成第 1 步。旧 WebProcess 会继续映射旧 inode，使同一份日志同时出现旧行号和新行号，造成“补丁已部署但行为未改变”的假象。

停止进程和覆盖大库必须分成两个独立 ADB shell 命令。MiniApp 在 WPE 退出时
可能销毁当前页面并中断发出 kill 的 shell；如果同一命令后面紧跟 `cp`，可能
留下截断的 `.so`。恢复后必须比较当前文件和备份文件的 SHA256，不能只检查
命令退出码。

设备 `/tmp` 通常是空间紧张的 tmpfs。大 WebKit 库应先推到同一
`/userdisk` 目录的临时文件，校验 SHA256 后再停止进程并 `rename`；不要把
150MB 级候选库推到 `/tmp`。

## 大文件传输

PVE 到本机的大型 WebKit 库可先 gzip，再分成 8 份并行 SCP。合并后必须同时校验：

- 每个分片 SHA256；
- 合并 gzip 的 SHA256；
- 解压后二进制的 SHA256。

未通过最终哈希前，不得覆盖 `assets/wpe-runtime/lib/libWPEWebKit-2.0.so*`。

多线程只用于相互独立的大文件分片。ADB 控制命令保持串行；同一 ADB daemon
并发执行多个 `push/shell` 容易触发设备选择和 smartsocket 竞争。设备侧大文件
先传到 `/userdisk` 临时路径，完整 SHA256 通过后再单独停止进程并原子替换。

分片必须是连续字节块，例如 `split -n 8` 或 `split -b 6m`。不要使用 `split -n r/8`：`r/` 是按行轮询分配，不能用 `cat part.*` 顺序还原二进制 gzip。

并行执行器返回 `session_id` 不等于 SCP 已完成。若外层任务结束时没有继续
等待每个 session，会留下大小不一的截断分片。应使用会等待全部子进程退出
的 `xargs -P 8`，或逐个保存并轮询 session；最终仍以三层 SHA256 为准。

## 当前待验证项

- 云原神排队结束后验证 0021 动态挂载 audio source，且 video
  `frames-received/frames-decoded` 继续增长。
- 验证远端音频实际从扬声器输出；若 audio source 仍无 sample，保留
  video-only 而不能重新阻塞视频。
- 验证 game profile 下连续拖动、多点触摸和短按 pointer fallback 同时成立。
- 连续运行 20 分钟，并完成 10 次启动、Home、重新进入的稳定性回归。

## 2026-07-25 回滚矩阵

| 对象 | 候选 SHA256 | 回滚对象/位置 | 结果 |
| --- | --- | --- | --- |
| 设备 `libWPEWebKit-2.0.so.1.10.2` appsrc PLI 候选 | `8445c0b4...2513` | 同目录 `.pre-late-keyframe-20260725`，`cd889c25...0ea7` | 已回滚并校验 |
| 设备高详细度 `run.sh` | `5594c46c...d0533` | `.pre-keyframe-category-20260725`，`8210701a...55bb` | 已回滚并校验 |
| PVE `GStreamerMediaStreamSource.cpp` | appsrc PLI 实验 | `.pre-late-keyframe-20260725` | 下次构建前必须恢复 |

本地失败候选仍保存在忽略目录
`.cache/keyframe-patch/candidate/`，只用于复核哈希，不能同步进正式 runtime。

## 2026-07-28 回滚矩阵

| 对象 | 当前/候选 SHA256 | 回滚对象/位置 | 状态 |
| --- | --- | --- | --- |
| 设备 direct-pad PLI WebKit | `0885a178...5d08` | 同目录 `.pre-direct-keyframe-20260728`，`cd889c25...0ea7` | 已部署，等待登录后验证 |
| PVE `GStreamerIncomingTrackProcessor.cpp` | `f2a8008b...` | `.pre-direct-keyframe-20260728`，`47f72df4...` | ARM64 编译通过 |
| 设备物理触摸 `run.sh` | `61ef7282...6be48` | 调试前 `.pre-uinput-test`，`929a72ff...b7e8` | 已恢复 `/dev/input/by-path/hyn_ts` |
| 错误 event10 调试脚本 | `8210701a...55bb` | `.pre-physical-touch-restore-20260728` | 仅保留取证，禁止恢复到正式运行 |
| 设备 MediaStream 聚焦日志 `run.sh` | `445f4bf2...2a60f` | `.pre-bridge-debug-20260728`，`61ef7282...6be48` | 仅日志变更，待复测 |

设备上的 uinput 注入器必须在验证结束后终止。`event10` 只可通过一次性环境
传给单次诊断进程，不能写回包内 `run.sh`。

### 2026-07-28 WebRTC 视频回环与 encoded appsrc 背压

为隔离云原神服务器，新增
`assets/wpe-runtime/tests/webrtc-video-loopback.html`，使用
`canvas.captureStream(30)` 建立本机 H.264 PeerConnection 回环。测试成功建立
ICE，但设备 runtime 没有 H.264 编码器，发送 pipeline 明确报：

```text
No encoder found for codec avc1.42C000
Unable to link any packetizer
streaming stopped, reason not-linked
```

因此 `packetsReceived=0`，这个结果不能用于判断接收解码桥。测试数据库先经
checkpoint 和 `.backup` 保存，结束后整库恢复；恢复后的主数据库与备份 SHA256
均为 `fa8d2711...f391`，活动标签仍是云原神，`next_tab_id` 仍为 `11`。

源码审计发现 `GStreamerMediaStreamSource::InternalSource::pushSample()` 在
appsrc `enough-data` 后使用 `doCapsHaveType(caps, "video")`，这会把
`video/x-h264` 的 IDR 与 delta frame 一起丢弃。该行为能解释 direct-pad PLI
已促使服务器重发 SPS/PPS/IDR，但下游 MPP 仍没有输入帧的现象。

`0004-webrtc-preserve-encoded-keyframes.patch` 将背压策略改为：

- raw `video/x-raw` 继续丢弃；
- 编码 delta frame 继续丢弃；
- 编码 key frame 即使队列已满也必须送入 appsrc；
- 记录非 `GST_FLOW_OK` 的 appsrc push 返回值。

PVE 源文件修改前 SHA256 为 `eaf09460...369d`，备份名为
`GStreamerMediaStreamSource.cpp.pre-encoded-keyframe-backpressure-20260728`；
修改后 SHA256 为 `1b8088fc...c35`，ARM64 `ninja -j72 WebKit` 编译通过。

设备同时保留一个独立 early-decode A/B：

```text
WEBKIT_GST_WEBRTC_FORCE_EARLY_VIDEO_DECODING=1
```

它只用于判断绕过 encoded MediaStream 二次解码桥后是否能出首帧。设备
`run.sh` 修改前 SHA256 为 `445f4bf2...2a60f`，回滚文件为
`run.sh.pre-early-decode-20260728`；候选 SHA256 为
`a9e6c2d3...1523`。该 A/B 和 `0004` 必须分开验证，不能同时据此归因。

### 2026-07-28 Janus 接收测试的弱判据与触摸恢复

`assets/wpe-runtime/tests/webrtc-janus-recv.html` 使用 Janus 公共 Streaming
服务建立了 receive-only PeerConnection。第一版测试观察到：

- ICE 达到 `connected/completed`；
- 收到远端 video track；
- `videoWidth/videoHeight=1280x720`；
- `currentTime` 持续增加。

但同时 `HTMLVideoElement.readyState=0`，DRM 视频 plane 89 仍为 `fb=0`，
MPP decoder 只完成 caps 查询后即 finalize。仅凭尺寸和 `currentTime`
不能证明视频已经解码或出屏，旧版页面给出的 `PASS` 是弱证据，禁止将其作为
early-decode 成功结论。

测试页现已增加三层验证：

1. `RTCPeerConnection.getStats()` 的 `packetsReceived/framesReceived/framesDecoded`；
2. `requestVideoFrameCallback()` 回调计数；
3. 将远端视频缩放绘制到 `32x18` canvas 后检查非黑像素能量。

只有连接、远端 track、解码证据和连续非黑 canvas 帧同时成立才标记 PASS。
增强版 SHA256 为：

```text
195c572bfd0c7b5f7b4f09abf7ba277d733dba65c496f7f42e8b545b0e274666
```

Janus 临时标签使用数据库备份
`browser.sqlite3.pre-janus-recv-early-20260728`。测试结束后主数据库已恢复，
两者 SHA256 均为：

```text
ff18eed85cc38ce78d90a490c3bccbba2ea358697929c3f069bbc5cf74dd56f4
```

恢复后活动标签为云原神，逻辑标签 ID 仍为 `5/7/8/9/10`。测试状态另存为
`browser.sqlite3.janus-touch-recovery-20260728`，用于复盘，不可作为正常
浏览数据库恢复。

此次用户看到“浏览器没有触摸”的一个直接原因是 Janus 临时标签和临时数据库
未及时恢复。2026-07-28 后续又定位到第二个独立原因：云游戏 `<video>` 开始
播放后，`auto` profile 切到 game，旧代码只发送 touch down/move/up，不再
生成 pointer tap，导致登录、教程和网页浮层等普通 DOM 按钮无法点击。
恢复后必须同时验证：

```text
/dev/input/by-path/hyn_ts -> /dev/input/event4
WPE_TOUCH_DEVICE=/dev/input/by-path/hyn_ts
WPE 进程 fd -> /dev/input/event4
初始活动 URI=https://ys.mihoyo.com/cloud/
```

`View state` 中的 `touch=<n>` 只在 `send_touch_event()` 路径递增。
browser profile 默认使用 pointer tap 和 native scroll，因此即使物理触摸完全
正常，该字段也可以一直为 `0`。browser profile 的有效证据应为：

```text
Touch down: ...
Pointer tap: ...
Native scroll: ...
Scroll fallback stop: ...
```

2026-07-28 恢复后，物理 `event4` 与 WPE 日志在同一时段交叉验证成功：
点击产生 `Pointer tap`，连续滑动使 `Native scroll` 计数从 1 增长到 64。
只读 `evtest` 验证结束后已终止，不得在后台长期占用诊断资源。

最终兼容策略为：

- 拖动和多点移动继续走 WPE touch event，保证游戏摇杆和连续操作；
- 仅当移动距离小于 tap 阈值时补发 pointer tap，恢复 DOM 按钮；
- `WPE_GAME_POINTER_TAP_FALLBACK=0` 可即时回滚；
- 实机强制 game 模式点击云原神右上角“更多”后，日志同时出现 touch
  down/up 和 pointer tap，菜单实际展开。

对应 launcher SHA256 为：

```text
wpe-drm-minimal: 2dded2fb3625ce9b4875c575b4c52facbc9b8fc1a37a757bb4d92ef0ca951bb7
```

另外，`miniapp_cli start` 的正确页面参数是：

```sh
miniapp_cli start 8001779591038449 frame
```

不能写成 `--frame`；后者会尝试加载不存在的 `--frame.js`，从而让恢复后的
数据库看似“浏览器没有启动”。

### 2026-07-28 early-decode 的 19/6 冻结与 incoming fakesink 时钟

真实云原神会话证明 RTP、H.264 和 MPP 都已经工作，但 early-decode 路径稳定
冻结在：

```text
frames-received=19
frames-decoded=6
frames-dropped=0
decoder-implementation=GStreamer mppvideodec
```

冻结后 libnice 仍持续收到 RTP，DRM 视频 plane 89 仍为 `fb=0`。MPP 输出帧的
PTS 约为 `1:08:59`，而当时 pipeline 运行时间仅约 6 分钟。源码审计确认
`GStreamerIncomingTrackProcessor::configure()` 创建的中间 fakesink 固定使用
`sync=TRUE`；它会等待远未来的 PTS，最终使上游队列阻塞。这说明卡点不在
RTP、MPP 解码耗时或 DRM commit。

`0005-webrtc-disable-intermediate-sync.patch` 增加：

```text
WEBKIT_GST_WEBRTC_DISABLE_INCOMING_SYNC=1
```

只在显式启用时将 incoming-track 中间 fakesink 改为 `sync=FALSE`。最终
`<video>` 播放 pipeline 仍保留正常时钟同步，未设置环境变量时行为与上游一致。
构建和部署校验值为：

```text
PVE patched source: 59e91add3ee4a088a308ab7a4202f9d4b930eb8b7b1e77d07ac9491f57df8681
ARM64 WebKit ELF:  f9c0f43bcb43faab9c63e5f200e9400c6a41383c427d2890601048ce33c098b1
pre-0005 WebKit:   0885a178c504adddf6f947de60df54a6296f1cc79e25b4d3c2dd03c4f3785d08
pre-0005 run.sh:    a9e6c2d35d1d69eb099c88980de029b192a02f874b5cef94a8bb7c6c2e751523
```

设备回滚文件为：

```text
lib/libWPEWebKit-2.0.so.1.10.2.pre-0005-20260728
run.sh.pre-incoming-sync-20260728
```

补丁已确认映射到 WebProcess，两个 early-decode/incoming-sync 环境变量也已
进入进程；仍须在一次成功建立视频 track 的云原神会话中验证解码计数持续增长。

### 2026-07-28 framebuffer 驱动的无人值守页面操作

为了不再要求人工点击，当前诊断流程直接导出 WPE plane 的 dumb framebuffer，
将 `266x960` ARGB framebuffer 旋转为可读的 `960x266` 图像，再根据页面坐标
反算临时 uinput 原始坐标。当前设备 rotation 270 的映射是：

```text
panel_x = (1 - raw_y / 960) * 960
panel_y = ((raw_x - 107) / (373 - 107)) * 266
```

例如页面“进入游戏”按钮中心 `panel=(835,247)` 对应
`raw=(354,125)`。实测日志同时给出：

```text
Touch down: raw=354,125 mapped=835.0,247.0
Pointer tap: x=835.0 y=203.0 screen_y=247.0
```

点击后画面进入“游戏启动中，请耐心等待”，证明 framebuffer 定位、rotation
映射和临时触摸注入链路均有效。随后本次服务端控制 WebSocket 返回：

```text
The server did not accept the WebSocket handshake
```

此时虽有 ICE/UDP 报文，但没有创建 incoming video track、没有 MPP decoder，
因此这次黑屏不能用于判断 `0005` 成败。必须区分：

- 控制 WebSocket 未建立：服务端会话/信令阶段失败；
- `frames-received=19, frames-decoded=6`：视频 track 已建立后的时钟背压；
- plane 89 `fb=0`：尚未产生可提交的 NV12 视频帧。

临时工具仅放在设备 `/tmp`，测试期间才把 `run.sh` 的触摸设备改为
`/dev/input/event10`。当前修改前备份为
`run.sh.pre-uinput-automation-20260728`，SHA256 为
`a089523599d96eeecd940e9dfcc12335a0632c98612c385d6497f195427dcc78`。
无人值守测试结束后必须恢复该文件、终止 uinput PID、删除 FIFO，并重新启动
确认 `touch_device=/dev/input/by-path/hyn_ts`。

### 2026-07-28 MediaStream 空音频 pad 阻塞与 0021

真实云原神会话最终证明 early H.264 + MPP 解码链已经持续工作。启用
`0020-webrtc-skip-player-audio-diagnostic.patch` 后：

```text
mediastream-media-player-1: NULL -> READY -> PAUSED -> PLAYING
frames-received/frames-decoded 持续增长，约 60fps
WPEViewDRM video overlay: client connected
plane 89: NV12 1920x1080, fb != 0
```

根因不是 DRM、MPP 或 RTP，而是同一 MediaStream player 初始同时暴露 video
和 audio appsrc。audio 尚无 sample/caps 时，playbin 等待该 pad，导致最终
video sink 也无法 PLAYING。永久禁音可证明根因，但不满足产品要求。

`0021-webrtc-defer-player-audio-until-video-preroll.patch` 改为：

1. 初始只暴露 video source；
2. video caps 连续稳定 5 次、约 250ms 后动态添加 audio source；
3. 10 秒内没有 video preroll 时清理 deferred audio，保持 video-only；
4. dispose 时取消 timer，避免 element 销毁后回调；
5. `WEBKIT_GST_WEBRTC_DISABLE_PLAYER_AUDIO=1` 仍保留为 0020 回滚，
   正式默认使用 `DISABLE=0`、`DEFER=1`。

构建和设备身份：

```text
0021 WebKit: 49cca77ea9f4ab62aa181d9e03e3f71ab675dc9571fdc0eb7742c28c01719afe
0020 backup: 77f165bd28e3fa06a4745a91c6a0f5519823c2ea68c059f8b40835fc7d79565a
device backup: libWPEWebKit-2.0.so.1.10.2.pre-0021-20260728
PVE source backup: GStreamerMediaStreamSource.cpp.pre-0021-20260728
formal AMR: 8873128f6964f9673612caaaba6d37b6704a744c9b37fe2368815fc3547f1871
```

正式安装后运行槽为 `a`，进程 maps 已确认映射该槽的大库。升级前后数据文件
哈希完全一致：

```text
browser.sqlite3: 6af998cf8cd93775ae65b034a53de1c4377c78dea11bb0247cbabb2b1e0ce133
cookies.sqlite:  facc4476044ea487958540e43fc7992e1c3da56b7f317745d708e6bb5fd94274
```

因此该次 AMR 安装未重置 Profile、Cookie 或云原神登录状态。上传到
`/userdisk` 的安装包和 `/tmp` 中的 0021 大库在安装、校验完成后已删除；
设备只保留正式安装槽和 0020 回滚文件。

本地 PeerConnection 回环不能作为 0021 成败判据：包内 runtime 没有 H.264
或 VP8 编码器，发送端分别报 `No encoder found`。直接
canvas/WebAudio MediaStream 测试在 `DEFER=1` 和 0020 控制组都会触发既有
WebProcess crash，说明它是本地发送/采集路径问题。Janus 公共接收测试能触发
deferred 分支和 10 秒安全超时，但该轮服务未产出可解码首帧。0021 的动态
audio attach 后持续播放仍须在云原神排队结束后验收，不得把上述隔离测试写成
已完成的远端音频验证。

### 2026-07-28 云原神自动启动、ICE/DTLS 已连接但仍报 `-5001`

本轮使用 persistent Profile 的现有 CookieJar 自动恢复云原神会话，没有修改
Cookie、Profile 或登录状态。MiniApp 的 hole 不会出现在 `miniapp_cli capture`
中，必须从 WPE DRM plane 导出 framebuffer：

```sh
/tmp/drm_fb_dump /dev/dri/card0 <wpe-fb-id> /tmp/wpe-fb.ppm
```

对于该设备的 `rotation=270` 和 `chrome inset=44`，页面按钮坐标必须经过两个
变换：raw touch 映射到 panel，再由 WPE 把网页事件的 y 坐标减去 chrome inset。
“进入游戏”实际成功点击是 `raw=(367,125)`，对应：

```text
screen=(835,260) -> page pointer=(835,216)
```

`raw=(354,125)` 只落在按钮上缘之外。自动化临时使用 `/dev/uinput` 创建一个
触摸设备并由它中继物理 `hyn_ts`；该进程必须通过 `setsid` 脱离 adb shell，
否则 adb 命令结束后 event 节点会消失，WPE 将记录 `Raw touch open failed`。
测试结束后已恢复 `/dev/input/by-path/hyn_ts`，并停止临时 uinput 进程。

为诊断 `-5001`，一次性启用了 `webrtcbin/libnice/dtls` GStreamer 日志和 DOT
graph，之后已恢复默认 `GST_DEBUG`。真实云端会话产生的 graph 明确显示：

```text
webrtcbin: connection-state=connected
webrtcbin: ice-connection-state=completed
dtlssrtpdec: connection-state=connected
mediastream player: 1920x1080 NV12, 60/1
```

因此此次 `-5001` 不是 Cookie、触摸、过期 WSS 证书、ICE 候选解析或 UDP 内核
丢包造成的。`generation` ICE 字段在 GStreamer 1.22 的旧解析器中只会输出
warning，源码确认它不会使 candidate 解析失败。内核 UDP 计数在连接期间有正常
收发且 `RcvbufErrors` 没有增长。

剩余问题位于已连接 MediaStream 到首帧呈现的最后一段：DOT graph 的最终 sink
仍是普通 `WebKitAppSinkWithWorkarounds`，不是预期的 Rockchip hole-punch sink；
随后网页仍显示“连接中断，错误码 -5001”。下一轮应先给 appsink sample 回调、
MediaStream first-frame 通知和 hole-punch quirk 选择增加计数日志，确认是没有
sample、sample 未触发 WebKit 首帧通知，还是 quirk 未被选中。不要再把这个
阶段误诊为 ICE 或 MPP 初始化失败。

### 2026-07-28 自动化重试与 0025 WebSocket 生命周期诊断

为避免依赖人工点击，测试通过临时 uinput 设备自动点击云原神的“进入游戏”。本轮
确认持久 Profile 仍保留登录 Cookie，首页可显示剩余免费时长。一次较晚点击得到
控制 WSS `403 Forbidden`；重新启动并提前点击后，TLS 过期例外正常放行，但网页
仍显示 `-5001`，且 video overlay plane 89 保持 `fb=0`。

新增 `0025-cloud-websocket-lifecycle-diagnostics.patch` 只添加以下不含敏感值的
日志：握手连接的 host/port/protocol/Cookie 是否存在，以及已连接后的 GIO 错误。
它不修改 Cookie、请求头、TLS、ICE 或媒体行为。该诊断库 SHA256 为：

```text
c9ebf72c6c2eefb5efa1fd161482f74325d99e14d357f045d718871ae30d7669
```

自动化页面操作仍有一个独立限制：在 `960x266` 视口中，“进入游戏”按钮会被底部
裁切；点击事件可到达 WPE，但在本次会话中无法稳定触发页面状态变化。一次上滑已
被 native scroll 记录，却没有新的网页内容帧，framebuffer 只剩工具栏。测试结束
必须恢复 `/dev/input/by-path/hyn_ts`，不得把临时 `/dev/input/event10` uinput 路径
留给日常浏览。后续应优先加入页面级 target/scroll 观测，再用 0025 捕获真正开始
会话时的握手结果。

### 2026-07-28 无人值守启动、RTP 证据与协议边界

当前 `wpe-drm-minimal` 的 `WPE_CLOUD_AUTOSTART=1` 通过页面注入脚本识别
`进入游戏`，再由 native `send_pointer_tap()` 完成点击。这样不依赖 MiniApp
触摸注入，也不需要人工点击。持久 Profile 的 `cookies.sqlite` 已保留，自动会话
可以直接进入云原神启动序列。

本轮新增的诊断均不记录 WebSocket、DataChannel、DTLS 或 RTP 载荷：

- `0027-cloud-datachannel-state-diagnostics.patch` 只记录 DataChannel 创建、状态和字节数；
- `0028-cloud-websocket-outbound-diagnostics.patch` 只记录 WebSocket 发包类型、长度和本地关闭请求；
- `tools/udp_pcap_summary.c` 只读取 Ethernet/IP/UDP 头，用于统计端点包数和长度，采集完成后立即删除 pcap。

实测结果排除了两个常见误判：

1. 云原神此次没有创建 WebRTC DataChannel，因此 DataChannel 不是 `-5001` 的当前门槛。
2. 主 WSS 完整执行了信令：浏览器发送 `103`、`51`、`2170` 等二进制消息，接收
   `319`、约 `5.4KB` SDP/控制消息，并持续对 `:5443` 控制 socket 发送/接收 `15`、`22`
   字节心跳。`1005` 关闭来自测速 `wssping` socket 的本地主动关闭，不能作为主信令失败。

对自动会话做 35 秒 `wlan0` UDP 头部采样后，云端 `:53701` 地址的双向总量每端仅约
`12KB`，单包主要是几十到两百字节；没有连续的大 RTP 视频包。与此同时 MediaStream
video source、Rockchip appsink 和 DRM overlay plane 都已创建，但 plane 89 仍为 `fb=0`。

因此当前故障边界是：**信令、ICE、DTLS 与控制 UDP 已可用，但服务端没有开始下发媒体**。
不要继续修改 DRM、MPP、旋转、Cookie 或 RTP decoder 来解决这个 `-5001`；下一步应比较
正常 Chrome 与 WPE 的 WebRTC Offer/Answer capability、浏览器特征与云端启动请求的结构，
并继续只记录长度、状态和协商字段，不记录认证或游戏控制内容。

### 2026-07-28 原生触摸回归与 SDP 结构检查

`0031` 诊断库（SHA256
`3dd726add865864911f2b9aab797d599affc465994639a5d745b0b576987abdf`）只增加了
`WEBKIT_CLOUD_SDP_DIAGNOSTICS=1`。它只记录 m-line 数量、media 类型、codec 数量、
direction、BUNDLE 和 `rtcp-mux`，不记录 SDP、candidate、fingerprint、cookie 或 URL 参数。

自动会话的协商结构是完整的：远端为 audio/video `sendonly`、本地为 `recvonly`，共有
audio、video、application 三个 m-line，BUNDLE 与 audio/video 的 `rtcp-mux` 都已启用。
这进一步排除了方向、BUNDLE 或 `rtcp-mux` 缺失导致的 `-5001`。

GStreamer 的实际 transceiver 选择也确认了远端视频不是“没有共同 codec”：云端提供的
H.264 集合中，本地已选中 H.264 payload `98` 及其 RTX payload `99`。后续仍无 RTP
动态 pad 或 overlay framebuffer，说明问题发生在云端决定是否开始媒体发送的阶段，而不是
H.264/Mpp 解码器未被协商。

同次启动发现运行时曾把 `WPE_SEND_TOUCH_EVENTS=0` 与
`WPE_TOUCH_SCROLL_FALLBACK=1` 固定为默认值。该组合只留下鼠标式轻点和私有滚动回退，
会让网页触摸控件、登录页和云游戏触摸路径失效。已改为默认：

```text
WPE_SEND_TOUCH_EVENTS=1
WPE_TOUCH_SCROLL_FALLBACK=0
```

设备上的 `run.sh.pre-native-touch-20260728` 是该脚本回滚点；切换前的 WebKit 库仍保留为
`libWPEWebKit-2.0.so.1.10.2.pre-0031-sdp-20260728`。重启后日志已确认：

```text
Raw touch: device=/dev/input/by-path/hyn_ts ... send_touch=1 ... scroll_fallback=0
```

这只恢复原生 WPE touch event 分发，不修改 Cookie、Profile、DRM plane 或自动点击逻辑。

### 2026-07-28 自动点击已送达页面，`-5001` 仍无媒体

为消除“仍需要人工点一次进入游戏”的不确定性，`wpe-drm-minimal` 的
cloud launch user script 增加了结构化点击回执。它只记录 host、frame、HTML
tag 和 `isTrusted`，不记录页面文字、URL 参数或事件载荷。交叉编译启动器的
SHA256 为：

```text
34c6b13ea15266578ba8aa33ae5bab1e5d1b795f1f255cd0210efc98104e9b1b
```

设备启动器替换前保留：

```text
assets/wpe-runtime/wpe-drm-minimal.pre-cloud-click-20260728
```

本机自动会话得到：

```text
Cloud launch native tap: css=1631.3,432.0 zoom=0.50 screen=815.7,260.0
Cloud page click: host=ys.mihoyo.com top=1 tag=DIV trusted=1
```

因此 native pointer tap 已被页面作为可信用户点击接收，不是 WebKit user
script 的 `element.click()`，不需要人工重复点击。

随后连续自动重试覆盖 telecom、unicom、mobile 控制入口；每轮均可完成 WSS、
Offer/Answer、ICE `completed`，并创建网页侧的 incoming audio/video track，
但从未出现 WebRTC `Connecting pad`、RTP buffer、`mppvideodec` 或 overlay
framebuffer。各轮约 20 秒后都以 `trackEnded` 结束，DRM video plane 保持：

```text
plane 89: fb=0
```

结论：当前 `-5001` 不由登录 Cookie、自动点击、触摸映射、ICE、DTLS、H.264
协商或本地 MPP/DRM 输出触发。下一步只能比较云端启动协议/浏览器特征与一个
能正常出媒体的 Chromium 会话，或增加不含控制载荷的页面 API 调用/状态日志；
不要再修改 PLI、解码器、toolbar 或要求用户手动点击。

### 2026-07-28 DataChannel 门槛、ICE role A/B 与并发传输

后续检查当前云游戏 SDK 后，前文“没有创建 DataChannel，因此 DataChannel
不是当前门槛”的判断已被新证据推翻。当前 SDK 注册
`RTCPeerConnection.ondatachannel`，只有收到服务端创建的通道并触发
`RtcDataChannelOpen` 后才把状态从 `StartingGame` 推进到 `Connected`。
客户端没有主动调用 `createDataChannel()`。

设备上的页面能力探针仅上报 API 是否存在，不读取认证、Cookie 或信令内容：

```text
Cloud WebRTC capabilities: host=ys.mihoyo.com top=1
  pc=1 dc=1 create_data_channel=1 sctp=1
```

因此前端没有因 WebRTC API 缺失而禁用通道。Offer/Answer 的 application
m-line 也一致：

```text
remote: UDP/DTLS/SCTP port=9 mid=2 setup=actpass sctp-port=5000
local:  UDP/DTLS/SCTP port=9 mid=2 setup=active  sctp-port=5000
BUNDLE: 0 1 2
```

DTLS client 握手成功，GStreamer 将 100-byte SCTP INIT 加密成 129-byte
DTLS application record，但没有收到 INIT_ACK。将
`WEBKIT_CLOUD_FORCE_ICE_CONTROLLER=0` 做负面 A/B 后，ICE 仍能
`connected -> completed`，SCTP 结果不变；测试结束必须恢复默认值 `1`。
所以不要再围绕 ICE controlling role 反复修改。

`wpe-drm-minimal` 能力探针构建：

```text
SHA256 b0a687447f3e79fcbe125b790bc8ec233eca551e09120b631c7d0a058189b0cd
rollback wpe-drm-minimal.pre-capabilities-20260728
```

PVE 上超过 100 MB 的库使用“远端 gzip + 4 MB 分片 + 8 路断点续传 +
本地校验”。实测并行 SCP 会在 255 KB 整数倍附近反复重置，macOS 自带
openrsync 又不支持 `--append-verify`，因此应使用 `--partial --append`，最后
用完整 SHA-256 兜底：

```sh
gzip -1 -c libWPEWebKit-2.0.so.1.10.2 > /tmp/libWPEWebKit.gz
split -b 4M -d -a 3 /tmp/libWPEWebKit.gz /tmp/libWPEWebKit.part.

# macOS 本地；末尾编号按实际分片数调整。重复执行即可续传。
jot -w '%03d' 13 0 | xargs -P8 -I{} \
  rsync --partial --append \
    -e 'ssh -o BatchMode=yes -o ConnectTimeout=8' \
    pve@192.168.100.118:/tmp/libWPEWebKit.part.{} \
    /tmp/wpe-parts/part.{}
cat /tmp/wpe-parts/part.* > /tmp/libWPEWebKit.gz
gzip -t /tmp/libWPEWebKit.gz
gzip -dc /tmp/libWPEWebKit.gz > /tmp/libWPEWebKit-2.0.so.1.10.2
shasum -a 256 /tmp/libWPEWebKit-2.0.so.1.10.2
```

必须把最终 SHA-256 与 PVE 原文件比较；分片缺失、重复或顺序错误时不得部署。
本次 149 MB WebKit 库压缩为约 49 MB，并发拉取和哈希核对均通过。

### 2026-07-28 SCTP INIT 已离开设备

`tools/udp_flow_meta.c` 是受限 UDP 元数据采样器，只记录时间、方向、IP、
端口和 UDP 长度，不保存包体。源码 SHA256：

```text
5a2071fd49e02f8f5df7761b53e83849d15ff65cbfb4c2e1e058aa5fddd02be5
```

ARM64 诊断二进制 SHA256：

```text
10aa4bedadcdc1152384b9f99955bcf188d9a45022e284dc1732bb8a30de7263
```

采样必须由设备端 `timeout 35` 限时，结果写到 `/tmp`，结束后删除。实机在
选中的云游戏 UDP endpoint 上看到：

- DTLS 握手完成；
- 每次 usrsctp 打印 `Sending INIT` 时，都有长度 `137` 的出站 UDP；
- 该长度对应 `129-byte DTLS application record + 8-byte UDP header`；
- 服务端仍返回 ICE/STUN 小包，但没有 INIT_ACK 对应的 DTLS application data。

所以 INIT 没有卡在 `sctpenc -> dtlsenc -> ICE socket` 的本地发送链路。禁止
为继续诊断而抓原始 pcap、DTLS 载荷、DataChannel 载荷或认证数据；元数据已经
足够证明这一层。

### 2026-07-28 SCTP stream 数 A/B 已证伪

回移植 GStreamer 上游提交 `fa6a598f9140`，把 incoming/outgoing stream 数
统一为 1024。候选 `libgstsctp.so` SHA256：

```text
3466f0a0914b27f185aeb0c3ad8ef0c4049cda2f9fde73693bf4b1209d0b39b0
```

该候选仍发送相同的 100-byte INIT，服务端没有 INIT_ACK，DataChannel 和视频
都未建立。测试后设备恢复正式插件：

```text
21cce2576dce8bb7a4cfffc26a632cbb40ddc1cfeee74e562257cad65468264a
```

因此不要再次把“stream 数不对”当作当前阻塞点。

### 2026-07-28 `max-message-size` 与 DTLS role A/B 已证伪

JS 结构化 SDP 探针确认：

```text
remote application: sctp-port=5000 max-message-size=262144
local  application: sctp-port=5000 max-message-size=none
```

WebKit 候选只对 Mihoyo Answer 回写远端的 `max-message-size`。修正后的候选
SHA256 为：

```text
8fa567838020e99d5cf00365d9c80c90ae13a6b89b96a9ae9483eee98b12c77b
```

设备日志明确出现：

```text
Cloud SDP compatibility: added max-message-size=262144 to local Answer
```

但连续两个会话仍停在 SCTP `COOKIE-WAIT`，只有 INIT 重传，没有 INIT_ACK。
因此该 SDP 属性缺失不是当前根因。

第二个 A/B 只改 Mihoyo Answer 的 `a=setup:active` 为
`a=setup:passive`。launcher 候选 SHA256：

```text
6a120abc8e5e89aecdab6fa2e07bf7ee8c5296da56b6b68f5e7850c18d91fcec
```

GStreamer 已进入 DTLS server role，但服务端从不发送 ClientHello；会话约
30 秒后重试。说明该服务要求当前 Answer 保持 `active`，不能用反转 DTLS role
绕过 INIT 无响应。

两项试验结束后，设备和 PVE 均恢复：

```text
libWPEWebKit-2.0.so.1.10.2
  d0eef26831cfe98650651ab748bb3f9e8156a9a079330a416feec57d1f6ac920
wpe-drm-minimal
  e9f523aa7d83770b9c97a399886a4cc8b662215cec0d3433514dad54c3d45273
run.sh
  b6ee39540de3188b9755c7691ddff62a3fdb1dfc4186a8c85fc546363777d815
```

源码中的两个 A/B 分支也已删除，不保留默认关闭的死代码。

### 2026-07-28 SCTP INIT 头字段有效

在 `sctpassociation.c` 的包发送回调中做了一次环境变量控制的临时诊断，只解析
SCTP common header 和 INIT 固定头，不记录 verification tag、TSN、参数内容、
DTLS 数据或任何业务载荷。实机三次重传结果一致：

```text
bytes=100
src_port=5000
dst_port=5000
configured_local=5000
configured_remote=5000
chunk_length=86
outgoing_streams=1024
incoming_streams=2048
checksum_zero=0
```

这证明当前 INIT 至少满足以下条件：

- SCTP 源端口、目的端口和 SDP `sctp-port` 一致，均为 5000；
- INIT chunk 长度在 100-byte SCTP 包范围内有效；
- CRC32c 字段不是 AF_CONN loopback 模式下的零校验和；
- 1024/2048 流数量确实进入线上包；此前对称 1024/1024 A/B 已证伪。

ICE 在 INIT 前已经 `connected`，随后进入 `completed`；每次 INIT 也已被先前的
UDP 元数据探针确认离开设备。服务端仍没有返回 INIT_ACK，因此当前阻塞点不是
端口写反、零校验和、INIT 未发出或 stream 数未生效。下一轮应比较 INIT
parameter 类型集合、DTLS application-data 路由和服务端所接受的 usrsctp/
浏览器版本行为，不应重复修改 SDP role、`max-message-size` 或 stream 数。

临时插件 SHA256：

```text
7b64fd1908446da3ac36e2cd4c2bc221ae019770d019944466c99f5c6ab6e93e
```

试验后设备恢复并核验：

```text
libgstsctp.so
  21cce2576dce8bb7a4cfffc26a632cbb40ddc1cfeee74e562257cad65468264a
run.sh
  b6ee39540de3188b9755c7691ddff62a3fdb1dfc4186a8c85fc546363777d815
```

PVE 临时源码已恢复；正式 staging 插件仍为
`21cce2576dce8bb7a4cfffc26a632cbb40ddc1cfeee74e562257cad65468264a`。

进一步只记录 INIT parameter 的类型和长度，三次重传完全一致：

```text
0xc000:4,0x8008:9,0x8002:36,0x8004:6,0x8003:6
```

对应 PR-SCTP、stream reconfiguration、AUTH random、HMAC algorithm 和
AUTH chunk list 等标准 WebRTC SCTP 扩展。没有长度越界或未知私有参数。
该轮临时插件 SHA256 为
`482d565205ebe5def7762388bd9d027863f742add4068f73fbbf1bf9a3b7099d`；
设备与 PVE 在记录后再次恢复正式版本。

### 2026-07-28 Chrome dcsctp stream 数 A/B 已证伪

当前 Chromium dcsctp 默认声明 `65535/65535` 个 incoming/outgoing streams。
为排除云游戏平台按 Chrome INIT 特征做脆弱兼容，在创建 usrsctp socket 后、
connect 前通过 `SCTP_INITMSG` 临时设置同样的流数量。实机日志确认线上包已变为：

```text
outgoing_streams=65535 incoming_streams=65535
parameters=[0xc000:4,0x8008:9,0x8002:36,0x8004:6,0x8003:6]
```

ICE 正常进入 `completed`，INIT 仍按 100 bytes 发出并重传，远端依旧不返回
INIT_ACK，DataChannel 和视频都没有建立。候选插件 SHA256：

```text
70d25f2cfe06c7b2a75f388db685ed8421b2a35cbbe15e199dffecda177ed2d8
```

因此 `1024/2048` 与 Chrome `65535/65535` 的差异不是当前根因。测试后设备
插件、`run.sh`、PVE 源码和正式 staging 插件均已恢复并核对正式哈希。

### 2026-07-28 SCTP INIT CRC32c 已验证

不能只凭 checksum 字段非零判断包有效。临时诊断用独立的 Castagnoli
`0x82f63b78` 实现，在计算时将 SCTP common header 的 checksum 四字节置零，
再同时按大端和小端解释线上字段。两次独立 association 的所有重传均满足：

```text
calculated=0xdca37508 wire_le=0xdca37508 match_le=1
calculated=0x7678b915 wire_le=0x7678b915 match_le=1
```

SCTP CRC32c 在线上按相应字节序保存，独立重算完全匹配。候选插件 SHA256：

```text
b23d28bec03e916f2d1fbec1667fde7772704ccf03817bb74f00ba1711567564
```

所以服务端没有 INIT_ACK 不是因为 CRC 错误而静默丢包。测试结束后设备和 PVE
再次恢复正式版本；不要重复尝试关闭 checksum 或改写 checksum 字节序。

### 2026-07-28 usrsctp master 升级 A/B 已证伪

为排除 GStreamer 1.22 vendored usrsctp 的旧建链缺陷，在 PVE 先完整备份
vendored 源码，再用上游 `fd070e05a7474f38c7fecdf4d4b6005d2547ee00`
的 `usrsctplib` 只重建 `libgstsctp.so`。未修改 WebKit、webrtcbin、DTLS、
libnice 或设备 runtime 的其它文件。候选 SHA256：

```text
a3893854ac1d5fa4de4a9aebf25da853acd95eae18fbea229ab50d782e777d5d
```

实机两次 association 均能让 ICE 进入 `completed`，但没有 INIT_ACK、
DataChannel、远端音视频或 DRM video overlay，约 30 秒后服务端重建
PeerConnection。说明当前阻塞不由 vendored usrsctp 到上游 master 之间的
协议、锁或输入处理修复解决。

测试后设备恢复正式 `libgstsctp.so`，PVE 恢复原 vendored 目录并重新构建；
正式 staging 插件仍为
`21cce2576dce8bb7a4cfffc26a632cbb40ddc1cfeee74e562257cad65468264a`。

### 2026-07-28 SCTP AUTH/ASCONF A/B 已证伪

Chromium dcsctp 不使用 usrsctp 默认开启的 SCTP-AUTH。临时候选在
`usrsctp_init()` 后关闭 `sctp_auth_enable` 和 `sctp_auto_asconf`，线上 INIT
从 100 bytes 缩为 44 bytes，参数只剩：

```text
0xc000:4,0x8008:8
```

也就是 PR-SCTP 与 stream reconfiguration；Random、HMAC 和 AUTH chunk list
均已消失。候选插件 SHA256：

```text
11280f7690046e1adb6a2ce28a0a8222d75c6fe73075e482d2e27bc0a02097c8
```

三次 PeerConnection 的 ICE 均能进入 `completed`，但仍只有 INIT 重传，
没有 INIT_ACK、DataChannel 或远端音视频。服务端仍约每 30 秒重建会话。
所以 SCTP-AUTH/ASCONF 扩展也不是当前阻塞点。测试结束后设备、PVE 源码和
正式 staging 插件均恢复正式哈希。

### 2026-07-28 JS RTC 终态已确认，不是连接状态未推进

`wpe-drm-minimal` 的只读页面探针持续记录
`connectionState/iceConnectionState/signalingState/iceGatheringState` 和 DTLS
状态，不读取 SDP、candidate、认证信息或业务载荷。云游戏真实会话稳定出现：

```text
connection=connected
ice=connected -> completed
signaling=stable
gathering=complete
dtls=connected
```

约 20–30 秒后服务端关闭 PeerConnection 并重试。期间没有
`RTCDataChannel`、INIT_ACK、RTP 或视频帧。因此“底层已连接但 JS 仍停在
connecting，SDK 在循环等待”不是根因。当前阻塞仍是服务端没有响应客户端发出
的 SCTP INIT，不能再用篡改 JS connection state 的方式绕过。

### 2026-07-28 Chrome INIT 组合 A/B 已证伪

一次性候选同时对齐 Chromium dcSCTP 的主要 INIT 行为：

- incoming/outgoing streams 均为 `65535`；
- receive window 为 `5 MiB`；
- send buffer 为 `2,000,000`；
- 关闭 usrsctp AUTH 和 auto-ASCONF，只保留 PR-SCTP 与 stream reset。

候选源码与插件 SHA256：

```text
source 6a14d926d0ee12bb34d401b9e1c7043b048cecf68f14900e6e924caaab2ebba6
plugin 56b1ae054bd45474a407b0ad17beef275983eda5743ffca78e9724b4e6bf3262
```

设备仍只有 INIT 重传，没有 INIT_ACK、DataChannel、RTP 或视频。测试后设备恢复
正式 `libgstsctp.so`：

```text
21cce2576dce8bb7a4cfffc26a632cbb40ddc1cfeee74e562257cad65468264a
```

PVE `sctpassociation.c` 和 build tree 也恢复正式基线。不要再把上述参数拆开或
重新组合测试。

### 2026-07-28 WPE 与 Chromium DataChannel 互通

使用一次性本地信令 relay，让当前设备 WPE 作为 Answerer、真实 Headless
Chromium 作为 Offerer。双方均达到 ICE `connected/completed` 和
connection `connected`，DataChannel 在两端进入 open，并完成 `ping/pong`
双向交换。

该试验证明当前 GStreamer DTLS、usrsctp、WebKit DataChannel glue 与 Chromium
具备通用互操作性。云原神失败是服务特定的协议/兼容问题，不是“当前 WPE 完全
不能和 Chromium WebRTC 通信”。测试 relay、HTTP server 和 Chrome 进程均已
停止，设备未保留测试页面或信令状态。

### 2026-07-28 ECDSA P-256 DTLS 证书 A/B 已证伪

为排除云端按浏览器证书类型做兼容判断，仅将 GStreamer 1.22 默认生成的
RSA-2048 DTLS 证书替换为 Chromium 常用的 ECDSA P-256。候选使用 OpenSSL
EVP keygen API，未修改 ICE、SCTP、SDP、WebKit 或媒体管线。运行日志确认：

```text
generated DTLS certificate key_type=ECDSA curve=P-256
```

候选源码与插件 SHA256：

```text
source 74fcb26ae2766a915fce9770c54d9dbb4f52e0720f05fba9c3f14818afbae706
plugin 4768af2ca502296b0a72c11826916cd8b5eed2bf3b5bf3e86bfdcf804d852722
```

ECDSA 下 ICE、DTLS 仍正常连接，但没有 DataChannel、INIT_ACK、RTP 或视频；
服务端依旧关闭并重试。安全日志检查中证书 PEM、SRTP key 等敏感文本计数为
零。试验后设备恢复原 DTLS 插件：

```text
a1043c9de2dd32eb73e219560463472e7fb2ff1fd8f09640eeb0619b7f5f8749
```

PVE 源码和 build tree 恢复：

```text
gstdtlscertificate.c 4afe759dbd06b577f774118181645c325a7bb3e3821b4441fb9aaf74d93b60cf
libgstdtls.so         9ff4500e1a2dc090cf4a1bc9898620fd053f8eecd8025b2841887db2c3cf336b
```

证书类型不是当前根因，不要重复尝试 RSA/ECDSA 切换。

### 2026-07-28 较早 WebKit 库回归 A/B

设备安装槽仍保留了当前正式库之前的 WebKit 二进制，因此直接在同一设备、
Profile、CookieJar、launcher、GStreamer 和网络环境下做库级 A/B：

```text
current d0eef26831cfe98650651ab748bb3f9e8156a9a079330a416feec57d1f6ac920
older   d84d20313818eb8a74fc45f70acb59932ae840b8961b641e166594ed6a77e510
```

较早库连续三轮同样完成 Offer/Answer、ICE `completed` 和 DTLS `connected`，
但没有 INIT_ACK、DataChannel、RTP 或 video overlay framebuffer。它还输出了
若干非关键跨域请求失败日志，但真实 PeerConnection 仍成功建链。

因此当前“无 SCTP 响应/无媒体”不是最近 RTC-state、SDP transport 或
DataChannel 诊断补丁引入的回归。试验后活动库已恢复为 `d0eef268...`，DTLS、
SCTP、launcher 和 `run.sh` 也逐一核验为正式哈希。

### 2026-07-28 公开 SDK 状态机与 Safari UA A/B

公开首页引用的 `cg-sdk.c39c60f7.js` SHA256：

```text
b2139a961b06634e8cf1ce6bd103d1d2822a2dc785a65f9f87eec235fb2ac15a
```

静态检查确认：

- SDK 在 WSS 建立后先发送 `StartGameReq`；
-收到成功响应后创建 PeerConnection 并发送固定的 client hello；
- SDK 不主动 `createDataChannel()`，而是等待 `ondatachannel`；
- 只有 `RtcDataChannelOpen` 才让状态机从 `StartingGame` 进入 `Connected`；
- 网页的 `-5001` 是等待首帧超时后主动停止游戏的前端错误。

当前浏览器用 Chrome 120 UA 驱动 WebKit。为排除 adapter/SDK 因伪装 Chrome
进入错误分支，只增加一次性 UA override，改为 Safari 17.6 风格 UA。实机确认
UA 生效，页面和 SDK 仍能正常完成 Offer/Answer、ICE `completed`、DTLS
`connected`，但依旧没有 INIT_ACK、DataChannel 或 RTP，约 30 秒后重试。

候选 launcher 与 `run.sh` SHA256：

```text
launcher efc7b7ee9f4f773b76e357e791e5da09d33d8d49dd5084ddb8b937c809d65cb5
run.sh   a857fab0cfda9116f5b5860305b242eb0e90884312fc73a43e3d4274362a2deb
```

因此 UA 浏览器类型不是当前根因。测试开关已从本地源码删除；设备 launcher、
`run.sh` 和 PVE launcher 源码均恢复正式基线，不保留 UA override 死代码。

公开 SDK 的 adapter.js 先检查 `navigator.webkitGetUserMedia`，再检查 UA。一次性
只读探针确认当前 WPE：

```text
adapter=safari legacy_gum=0 legacy_pc=0
```

也就是说，即使工具栏使用 Chrome 120 UA，SDK 的 WebRTC adapter 实际已经走
Safari 分支。Safari UA A/B 没有改变 adapter 分支，只改变了 UA 字符串。探针
launcher SHA256 为
`823901260ad79762c0c948c8d542b03180cf5597064002b30f5871e84ed75ae4`；
记录后已删除探针代码并恢复设备/PVE launcher 基线。

### 2026-07-28 云游戏 WebSocket 控制序列完整

为避免猜测网页是否完成启动协议，在 WebKit 网络进程中临时只解析二进制
WebSocket 消息的 8 字节 envelope 头。日志只包含方向、类型和长度，不记录
SDP、token、Cookie、protobuf 字段或业务载荷。类型定义来自公开 SDK：

```text
1 Signaling
2 Proxy
3 Handshake
4 KeepAlive
```

实机会话按以下顺序稳定出现：

```text
out Proxy     103 bytes
in  Proxy     319 bytes
out Handshake  51 bytes
in  Handshake  47 bytes
in  Signaling 约 5.4 KiB
out Signaling 2170 bytes
out Signaling 135/146/151 bytes
```

这与公开 SDK 的 `StartGameReq -> StartGameRsp -> client hello -> remote Offer ->
local Answer/candidates` 状态机一致。第二轮继续只解析 Proxy 包固定头
`0x4567 + cmdId + lengths + 0x89ab`，确认：

```text
20001 outbound StartGameReq
20002 inbound  StartGameRsp
1900  bidirectional ReliableMessageQueueData
```

`1900` 后续包含双向分片、ACK 和约 59 KiB 的发送消息，说明云端控制代理和
WebSocket fallback 数据队列都在工作。失败仍严格发生在 DTLS 后的 SCTP：
本机发送 INIT，远端不回 INIT_ACK，也不发送 RTP。不要再把 `-5001` 归因于
StartGame、Handshake、Offer/Answer 或 WebSocket 控制序列缺失。

两版临时 WebKit 候选 SHA256：

```text
envelope-only 9fa5ff3fbc0aa892ddcd1adaac89a4b640bbd994c4611b81daa5f9cc6fa907fd
proxy-cmd     51a9bab2d5352c10e865dd4118c84c49ee37810afd36e49384fad591142d6e2d
```

大库均通过 4 MiB 分片、8 路 `rsync --partial --append`、gzip 校验和完整 ELF
SHA256 后才部署。记录完成后设备和 PVE 已恢复正式 WebKit：

```text
d0eef26831cfe98650651ab748bb3f9e8156a9a079330a416feec57d1f6ac920
```

### 2026-07-28 音视频加 DataChannel 的 BUNDLE 互通

此前 WPE 与 Chromium 的互通测试只有 DataChannel，不能排除云游戏三条 m-line
触发 GStreamer BUNDLE 映射问题。新增一次性本地页面，让真实 Headless
Chromium Offerer 同时提供合成音频、合成视频和 server-created DataChannel，
WPE 继续作为 Answerer。

验证结果：

- WPE 收到 audio/video 两条 track；
- 双方 ICE、DTLS 和 connection 均进入 connected；
- server-created DataChannel 在双方进入 open；
- 双向 `ping/pong` 完成。

因此 audio/video/application 三条 m-line 和 `BUNDLE 0 1 2` 在当前
WebKit/GStreamer 上可互通，云端无 INIT_ACK 不是通用 BUNDLE transport 映射
缺陷。临时 relay、HTTP server、Chromium 和 WPE 均已停止，设备 `run.sh`、
WebKit、launcher 恢复正式哈希。

### 2026-07-28 DTLS cipher/profile 对照已证伪

临时 DTLS 插件只在握手成功后打印 OpenSSL 返回的版本、cipher、client/server
role 和既有 SRTP profile 名，不记录证书、fingerprint、密钥或包体。云原神与
本地 Chromium 三路 BUNDLE 互通测试均协商出完全相同的组合：

```text
version=DTLSv1.2
cipher=ECDHE-ECDSA-CHACHA20-POLY1305
role=client
srtp=SRTP_AES128_CM_SHA1_80
```

本地 Chromium 对照在该组合下 audio/video track、SCTP、DataChannel 和
`ping/pong` 均成功。因此不能通过强制 AES-GCM 或调整 SRTP profile 修复云端
无 INIT_ACK；当前 ChaCha20 cipher 不是根因。临时插件 SHA256：

```text
41fb04ba2f55ffc803a4303c67a83acc9d3006bddffef122031c9f3e36d93839
```

测试结束后设备恢复正式 `libgstdtls.so`：

```text
a1043c9de2dd32eb73e219560463472e7fb2ff1fd8f09640eeb0619b7f5f8749
```

PVE DTLS source/build tree 恢复并核验：

```text
source c737c330d4f7b93ba3f97efbdf33848a0023ad1a057d64b4df6c982737d3ec5e
plugin 9ff4500e1a2dc090cf4a1bc9898620fd053f8eecd8025b2841887db2c3cf336b
```

### 2026-07-28 云端 Offer 与 WPE Answer 的 payload 参数不保真

在 launcher 的 document-end 用户脚本中临时增加严格受限的 SDP 摘要，只记录
payload number、`rtpmap`、`fmtp`、`rtcp-fb`、extmap 数量和媒体方向，不记录
candidate、fingerprint、地址、token 或 SDP 原文。正式云会话稳定出现：

```text
remote audio PT63  red/48000/2  fmtp=111/111
local  audio PT63  RED/48000/2  fmtp=111/111=1

remote video PT98 H264/90000 fmtp=...profile-level-id=42001f
local  video PT98 H264/90000 fmtp=...level-asymmetry-allowed=1;
                                  level-asymmetry-allowed=1;
                                  profile-level-id=42e01f
```

也就是说，GStreamer Answer 保留了远端 payload number，却改写了同一 payload
的 codec 参数；音频 RED fmtp 也被 caps 序列化成不同语义。诊断 launcher：

```text
328a169d88c69728e5069336195550561b466e93a53549af2cebaf9f8a1b9891
```

随后做了一次最小负面 A/B：在 JavaScript `createAnswer()` 返回后，仅把已选
payload 的 `rtpmap/fmtp` 恢复为 Offer 原值。修改后的摘要符合预期，但连续三轮
均从正式基线的 `ICE completed` 退化成约 6 秒 `ICE failed`，未进入 DTLS/SCTP。
原因是 SDP 文本与 webrtcbin 已选 transceiver codec 状态不一致。以后不要再在
`createAnswer()` 之后直接改 codec 参数；若要修复，必须在生成 Answer 前修正
GStreamer codec preference/caps 交集。

失败候选与设备日志：

```text
candidate a9d1fa9c5c7a8b3d80796bc9736610b78d6ae403d978fbd77fb20d2190dc6d8e
log       3da22b072fe1f6ee9962c8fc04d813c6107e2dea5065dd1eb09da904169b14c0
```

设备已恢复正式 launcher：

```text
4bb9bd9fc474bd901d9b303046e376e2ccad8c8ee401f415545b89fa036716c9
```

### 2026-07-28 云游戏远端音频最终修复

云游戏已经能够完成 ICE、DTLS、SCTP、DataChannel 和 MPP 视频解码，但远端
Opus 音频长期停在 MediaStream player 的 PAUSED 状态。按单变量原则依次得到：

1. 独立 GStreamer 管线
   `rtpopusdepay -> opusdec -> audioconvert -> audioresample -> alsasink`
   可持续播放，排除 Opus 插件、decoder 属性和 RK817 ALSA。
2. `decodebin3` 在 incoming track 的 READY 到 PAUSED 状态切换中阻塞，因此
   0045 改为固定的静态 Opus 解码链。
3. 0045 首版错误混用 floating reference 与 `GRefPtr`，父 bin 存活但子 element
   被释放；0046 修正 GStreamer element 所有权。
4. 0046 解码器和 ALSA 均能启动，但只有一个 sample。状态日志证明 incoming
   track 中间 `fakesink` 正在等待异步 preroll，父 bin 的 PLAYING 请求没有继续。
5. 0047 仅对 audio 中间 sink 设置 `async=false`。之后音频每约 20ms 进入
   `incoming-audiosrc0`，ALSA 每约 10ms 写入 480 frames，音频和视频 player
   都稳定处于 `PLAYING / SUCCESS`。

最终实机同时确认：

```text
ICE connected -> completed
DataChannel open
mppvideodec active
NV12 DRM video plane active
/dev/snd/pcmC0D0p opened
audio/video MediaStream players PLAYING
```

正式 WebKit 0047 与 WebRTC 插件 SHA256：

```text
libWPEWebKit  0c07b7d63d9566a0be2482978a48d9c81dae5523577a7d6bd0cc8936fd54b9b7
libgstwebrtc  9ba9d82899375b7dd4d34b792c3358e135d26b5932406ba18cea57b7a90d59eb
```

设备保留 `.0044-dynamic-opus-blocked-20260728`、
`.0045-empty-static-bin-20260728` 和
`.0046-audio-track-paused-20260728` 三个相邻候选备份。出现回归时应按哈希
回退，不要重新猜测 DTLS、SDP 或 ALSA。

### 大文件多线程传输经验

PVE 到 macOS 的 149 MiB WebKit 调试库使用 8 路传输：

1. PVE 端 `split -n 8` 生成等量分片。
2. macOS 用 `xargs -P8 rsync --partial --append` 并行拉取。
3. 按文件名顺序合并，先校验 gzip/zstd（如有），最后校验完整 ELF SHA256。

不要对同一 USB ADB server 同时启动 8 个 `adb push`。实测多个客户端会竞争
启动 adb daemon，全部传输反而失败。PVE/SSH 网络链路可以多线程；Mac 到设备
保持单个 `adb push`，并在替换前后校验 SHA256。

### 触摸日志采样注意

旧日志只输出前 8 个 touch event 和每 80 个 event 一次，因此短 swipe 看不到
`TOUCH_UP` 不代表输入未释放。正式代码改为 down/up 始终输出、move 低频采样。
判断触摸卡死时必须同时检查：

- raw evdev 是否收到 `ABS_MT_TRACKING_ID=-1`；
- WPE 是否发送 `WPE_EVENT_TOUCH_UP`；
- `game_input_active` 是否已由视频播放状态切为 true；
- DataChannel 是否仍为 open。

### 2026-07-28 晚间 云原神卡“游戏启动中”与迟到音轨主线程死锁

问题：同一台 794 设备、同一套 0047 正式二进制（当天早些时候已验证全链路可用），
云原神进入后永久停在“游戏启动中，请耐心等待～”，无 `-5001`、不响应任何操作。

部署身份（进程 maps 与本地仓库逐一核对，全部一致，排除部署问题）：

```text
libWPEWebKit  0c07b7d63d9566a0be2482978a48d9c81dae5523577a7d6bd0cc8936fd54b9b7
libgstwebrtc  9ba9d82899375b7dd4d34b792c3358e135d26b5932406ba18cea57b7a90d59eb
libgstsctp    21cce2576dce8bb7a4cfffc26a632cbb40ddc1cfeee74e562257cad65468264a
libgstdtls    a1043c9de2dd32eb73e219560463472e7fb2ff1fd8f09640eeb0619b7f5f8749
wpe-drm-minimal d27fddcaa2f450c04c7e39959d44904802fef8b73b9a9e3f7ba170ebf68c4c9b
run.sh        21efa688dcf0e7e3e1f18895106b2d7bc3987d6f273f1f10a278b2b694997b13
```

两轮观测（第二轮仅开启 `WPE_CLOUD_DIAGNOSTICS=1`，纯日志变更）：

1. 信令、Offer/Answer、ICE `connected/completed`、DTLS `connected` 全部正常。
2. 服务端创建 DataChannel（stream 1，label `webrtc_data_channel`），SCTP 层
   open/ack 完成，0027 诊断确认 `DataChannel created/state=2`。
   **之后双向零消息**：无 `DataChannel received`，webrtcdatachannel:6 也无发送记录。
3. 视频只收到 480x240 等待动画流：mppvideodec 正常解码约 75/90 帧（约 2 秒）后
   服务端永久停发；plane 89 始终 `fb=0`。
4. 视频停发约 5 秒后音频 RTP 到达，`GStreamerIncomingTrackProcessor` 走
   0045 静态 Opus 链（`Using static rtpopusdepay -> opusdec WebRTC audio chain`）。
5. **该日志行之后 WebProcess 主线程立即死锁（2/2 复现）**：endpoint 1 秒统计、
   ThreadedCompositor、JS 探针全部停止；进程 CPU≈0 但 SCTP/rtpbin 流线程仍活；
   chrome reload 点击已到达 UIProcess（`Chrome tap: reload`）但 WebProcess 不执行；
   页面永久冻结在“游戏启动中”。60 秒 UDP 元数据采样证实连接侧也已无任何云端报文。
6. 服务端约在 2 分钟后以 `1006` 异常关闭控制 WSS；因主线程已死锁，前端无法做出
   任何反应（不显示 -5001、不退回大厅）。

结论：

- 全连接链（ICE/DTLS/SCTP/DataChannel open）在当前正式库上仍然健康，与当天
  早些时候的成功会话一致。服务器只发约 2 秒等待动画后不再推进游戏实例、
  DataChannel 零流量，属于服务端/边缘节点/容量侧问题，不要为此修改本地 ICE、
  SDP、SCTP、解码器或 DRM。
- **我方真实缺陷**：在“服务器挂起型”会话中，迟到音轨的静态 Opus 链
  （0044-0047 路径）会死锁 WebProcess 主线程，把可恢复的卡顿放大成永久卡死。
  已定位的竞态窗口：0045 在 `GStreamerIncomingTrackProcessor::configure()` 内
  同步调用 `trackReady()`，早于 `connectPad()` 的 `gst_bin_add`/`gst_pad_link`/
  `gst_element_set_state(PAUSED)` 完成；主线程随即进入
  `connectIncomingTrack() -> setBin()`，与流线程的 bin 添加/状态同步形成交叉。
  修复方向（PVE 侧，未实施）：恢复上游时序保证——bin 加入 pipeline 并 link
  完成后再触发 `trackReady()`/`connectIncomingTrack`（例如由首个 buffer 探针或
  async 回调触发），并核查 `setBin()` 与该路径上的对象锁。
- 未验证项：服务端为何停发等待动画后不推进。下一步应比较正常 Chrome 会话在
  DataChannel open 后由哪一方先发消息、服务端推进游戏的时间特征；或换时段/
  网络入口复测（本次为晚高峰 telecom `cg-mhycd1` 入口）。

gdb 限制：设备 1GB 内存，attach 该 WebProcess 时 gdb 自身 RSS 超 260MB 被
OOM killer 终止（dmesg 已确认）； stripped 大库无法在本机离线符号化设备 core。
后续定位死锁帧建议在 PVE 用带符号构建复现，或在设备上先行 A/B
`WEBKIT_GST_WEBRTC_FORCE_EARLY_AUDIO_DECODING=0`（回退 parsebin 路径）以确认
死锁是否仅在静态链路径。

回滚/清理：`run.sh` 已恢复正式哈希 `21efa688...`（备份
`run.sh.pre-audio-freeze-20260728` 保留在设备同目录）；三个 WPE 进程已终止并
确认零残留；设备 `/tmp` 的 framebuffer/core 临时文件已删除，诊断工具保留。

### 2026-07-29 动画降至 1-2fps 后 MiniApp 崩溃

这次故障不是 DRM commit 或 GPU fault。内核明确记录：

```text
miniapp invoked oom-killer
Out of memory: Killed process 643 (miniapp)
```

OOM 前 1GB 设备的主要常驻占用为：

- MiniApp PSS 约 342MB，其中 `/dev/dri/card1` 映射约 185MB；
- `WPEWebProcess` PSS 约 247MB；
- `WPENetworkProcess` PSS 约 160MB；
- 512MB swap 已全部用完。

旧 `run.sh` 给 WPE 及其子进程设置 `oom_score_adj=-600`，并默认把
`swappiness` 提到 100。这会让已满 swap 下的交互延迟继续恶化，同时迫使内核
避开 WPE、选择杀死 MiniApp 生命周期宿主。`miniapp_cli trimImageCache` 当前
实现为 TODO，不能释放上述 card1 映射。

修复后的默认策略：

```text
WPE_OOM_SCORE_ADJ=200
WPE_DROP_CACHES=0
WPE_SWAPPINESS=(不修改系统值)
WPE_WEB_PROCESS_MEMORY_LIMIT_MB=340
conservative/strict/kill=0.45/0.65/0.90
WPE_WEBKIT_CACHE_MODEL=document-browser  # RAM <= 1.5GB
```

首次在 swap 仍为 512/512MB 的苛刻状态下复测，内核改为杀死
`WPEWebProcess`，MiniApp PID 保持存活，证明宿主保护已生效：

```text
oom-kill: task=WPEWebProcess oom_score_adj=200
Out of memory: Killed process 3097 (WPEWebProcess)
```

同一轮性能日志显示 Skia compose 从正常的约 2-6ms 上升到 150-190ms，
画面同步降至 1-4fps；DRM 全帧旋转 copy 约 5ms，commit 约 6-11ms。因此此类
“先逐渐卡顿、随后崩溃”应先检查 `free -m`、各进程 `smaps_rollup` 和 dmesg，
不要先修改 DRM 刷帧或 WebRTC。

当前设备没有可用 Mali runtime/probe，网页合成是
`skia_raster_shm -> rotated dumb framebuffer -> atomic DRM`；云游戏视频仍走
Rockchip MPP 解码和 NV12 overlay。这两个结论必须分开，MPP 视频工作不代表
网页动画获得 GPU 加速。

0047 WebKit 使用 `llvm-strip --strip-unneeded` 后包内文件从约 149MB 降到
99MB，逻辑未变，SHA256 为：

```text
20e022057bda63611dde6c57b4742168c88fd4a2e859fb5379793eb74fc66c9a
```

AMR 相应从约 291MB 降到 268MB。strip 只降低安装包和文件映射成本，不应被
当作运行时匿名内存 OOM 的主要修复。设备重启后的干净 swap 性能复测因目标
ADB 尚未重新上线而待完成。

最终待安装的自适应 cache launcher SHA256：

```text
c80bd3c110d6a942550b4ef82766a1407269ba0377573115bc5f10f36d7cfac4
```

### 2026-07-29 WebRTC 音频反压、overlay 抢占与云原神内存阈值

#### A/B 定位结论

本地 Janus 接收测试把同一 WebRTC 会话分成两组：

- video-only：约 30fps 持续运行，MPP 解码、NV12 DRM plane、UDP 接收队列均正常；
- audio+video：普通 MediaStream audio 支路只接收约 18 个包后停止，UDP receive
  queue 增长到约 `0x680c0`，累计丢包约 27923，随后视频也停止。

这证明卡点不在 ICE、MPP 或 DRM，而是 audio appsrc/playbin preroll 对共享 BUNDLE
transport 产生反压。不能通过继续修改视频队列、DRM page flip 或 ICE role 修复。

#### 0055：只让实际视频帧取得 overlay

旧实现创建 hole-punch sink 时立即占用唯一 overlay。一个没有输出 NV12 DMA-BUF
的旧播放器也会挡住后续真实视频。`0055` 改成：

- 只在收到首个单平面 NV12 DMA-BUF 时取得 owner；
- 旧 owner 超过 750ms 没有帧时允许新 sink 接管；
- sink 释放时只释放属于自己的 owner。

MPP 普通视频 smoke test 验证 plane 89 为 NV12，FB 130–133 持续轮换。补丁：
`wpe-drm/webkit-patches/0055-rockchip-overlay-acquire-on-first-frame.patch`。

设备回滚文件：

```text
libWPEWebKit-2.0.so.1.10.2.pre-0055
sha256=a625b036cb1a626f37c9bcffac4b2915e0265dd8dcaae523a3dffa154b45b93b
```

#### 0056：远端音频留在 endpoint pipeline

最终方案不再把远端 Opus 暴露给 MediaStream player：

```text
decoded Opus -> tee
  -> bounded leaky queue -> bookkeeping fakesink
  -> bounded leaky queue -> platform ALSA sink
```

两个分支均关闭会阻塞状态切换的异步 preroll；动态加入的 audio track 同样从
MediaStream player 过滤。环境开关：

```text
WEBKIT_GST_WEBRTC_DIRECT_REMOTE_AUDIO=1
```

失败迭代不要重试：

- 0056a 重复链接 processor 到 queue，导致 track 未 ready；
- 0056b 修复链接后，BUNDLE 在约 12 个视频包处冻结；
- 0056c 虽过滤动态 player audio，但 `RealtimeIncomingSource` 仍在约 18 个音频包
  处造成反压；
- 0056d 完全不暴露该音轨，只保留 endpoint 内直连 sink，才消除共享 transport
  反压。

最终库：

```text
libWPEWebKit-2.0.so.1.10.2
sha256=7d9daf567cb4a7c49b382b770e07b39ba8628a3284423f1836b9a88ede9afb56
```

对应补丁：
`wpe-drm/webkit-patches/0056-webrtc-direct-remote-audio-sink.patch`。

受控 Janus AV 运行超过 2 分 20 秒：

```text
frames-received=4149
frames-decoded=2076
video=30fps, MPP
ALSA status=RUNNING
UDP rx_queue=0, drops=0
```

直接音频 sink 的当前限制：网页自身的 MediaStream volume/mute 不再控制该 sink。
系统音量仍有效；后续若要支持网页 mute，应给直连 sink 增加独立控制 IPC，而不是
重新接回会产生反压的 MediaStream player。

#### 云原神正式页内存阈值

云原神进入 H.264 480x240、MPP 约 60fps 后，旧 340MB WebProcess 上限会主动
触发 WebKit memory-limit：

```text
Unable to shrink memory footprint of process (329 MB) below the kill threshold (306 MB)
reason=memory-limit(1)
```

当时系统仍有约 440MB `MemAvailable`，不是 kernel OOM。将
`WPE_WEB_PROCESS_MEMORY_LIMIT_MB` 调为 448 后，真实云原神连续运行超过
3 分钟：

```text
frames-received=10031
frames-decoded=9981
video=60fps, H.264 480x240, MPP
WebProcess RSS=383-400MB, peak about 410MB
MemAvailable minimum about 202MB
ALSA=RUNNING
DRM NV12 framebuffer IDs continuously rotating
```

默认配置因此固定为：

```text
WPE_WEB_PROCESS_MEMORY_LIMIT_MB=448
WEBKIT_GST_WEBRTC_DIRECT_REMOTE_AUDIO=1
```

游戏页面触摸 profile 自动从 `browser` 切到 `game`。中心 tap 和 500ms swipe
实测 native touch 计数增长、scroll 计数保持 0，说明连续触摸没有再被转换成
浏览器滚动。

0056d 之前的设备回滚库按迭代保留：

```text
libWPEWebKit-2.0.so.1.10.2.pre-0056-direct-audio
libWPEWebKit-2.0.so.1.10.2.pre-0056b-ready-fix
libWPEWebKit-2.0.so.1.10.2.pre-0056c-dynamic-filter
libWPEWebKit-2.0.so.1.10.2.pre-0056d-direct-only
```

正式安装会轮换 MiniApp 的 `a/b` 槽，因此不要依赖槽内临时 `.pre-*` 文件长期
存在。PVE 构建机的稳定回滚源保留在：

```text
/tmp/libWPEWebKit-2.0.so.1.10.2.0055-overlay-first-frame
/tmp/libWPEWebKit-2.0.so.1.10.2.0056-direct-audio
/tmp/libWPEWebKit-2.0.so.1.10.2.0056b-direct-audio-ready
/tmp/libWPEWebKit-2.0.so.1.10.2.0056c-dynamic-audio-filter
/tmp/libWPEWebKit-2.0.so.1.10.2.0056d-direct-only
```

正式安装前必须先停止 `run.sh/wpe-drm-minimal/WPEWebProcess/WPENetworkProcess`，
记录 `browser.sqlite3` 与 `cookies.sqlite` 哈希；安装后再次核对，不能用清数据
或新建 Profile 代替回归测试。

#### 正式 AMR 验收

正式 AMR 安装后生效槽中的 WebKit 库 SHA 与 `0056d` 一致。恢复原 Profile 和
Cookie 后，第一次云端启动返回网页自身的“网络错误，请稍后重试”；关闭弹窗并
再次点击“进入游戏”后正常建立 WebRTC，不应把一次服务端入口失败归因到本地
runtime。

```text
formal AMR sha256=0db04367eedd7fd9292df6024ee5ac54583e2d5be729f60f21422f6399645d2b
WebKit sha256=7d9daf567cb4a7c49b382b770e07b39ba8628a3284423f1836b9a88ede9afb56
```

正式媒体会话连续采样 150 秒：

```text
frames-decoded: 670 -> 7836
frames-per-second: 60
decoder: GStreamer mppvideodec
WebProcess RSS: about 330-349MB
MemAvailable: about 229-246MB
ALSA: RUNNING, hw_ptr 629768 -> 6651288
video plane: NV12, framebuffer IDs continue changing
memory-limit termination: none
```

生命周期回归同时发现 `src/app.json` 曾将 `frame` 直接指向 `frame.vue`。Vue
组件里的 `onHide/onUnload` 不是 Falcon Page 生命周期，Home 后不会自动调用。
正式修复增加 `src/pages/frame/frame.js`，继承 `BasePage` 并挂载
`FrameComponent`，让 Page 层转发 `onShow/onHide/onUnload`。用
`miniapp_cli start 8080222437664451 index` 产生真实前后台切换后，日志出现
`wpe frame onHide`，三个 WPE 进程在 8 秒内全部退出。`hal-key 1` 在该固件上
只返回按键配置检查，不能作为 Home 注入或生命周期验收手段。
