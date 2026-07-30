# WebKit 源码树补丁（非 wpe-drm/ 部分）

`wpe-drm/` 根目录的文件是 WPEPlatform DRM 后端（`Source/WebKit/WPEPlatform/wpe/drm/`）和视频直出 quirk 的镜像，构建服务器 WebKit 树里对应文件就是它们的副本。

除此之外，还在服务器 WebKit 树里对其他子系统打了几处源码级补丁——这些改动只存在于服务器（`~/wpe-lite2/stripped/WebKit/`，每处都留了 `.bak-claude-*` 备份保存补丁前的原始版本），此前没有同步回本仓库。本目录按 `Source/` 下的原始相对路径镜像这几个**补丁后**的文件，作为唯一的版本历史备份；每次改动前先在服务器上更新这里，再 scp 回服务器对应路径。

## 补丁清单

| 文件 | 做了什么 | 为什么 |
| --- | --- | --- |
| `Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/AcceleratedSurface.{cpp,h}` | 保留 dma-heap 探测与 SHM 回退；新增 `WEBKIT_SKIA_CPU_COMPOSITOR`，CPU 模式让 composited/non-composited Skia 表面都直接写入共享像素，不创建 GL texture/render fence，也不做 `readPixels()`；DRM release fence 直接用 `poll(sync_file)` 等待 | 去掉 CPU profile 最终合成和页面直绘中的 softpipe GL、整帧回读与 EGL wait，同时保留 framebuffer 复用同步 |
| `Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/ThreadedCompositor.{cpp,h}` | CPU compositor 不创建 GL context，不执行 GrContext flush、EGL swap 或 GL fence；tile 上限固定 4096，并输出 2 秒窗口性能统计 | 避免 `ThreadedCompositor` 被 softpipe 单线程占满，同时保留 GPU profile 原路径 |
| `Source/WebCore/platform/graphics/skia/SkiaPaintingEngine.cpp` | CPU compositor 强制使用 unaccelerated raster tile，跳过 GPU atlas 准备 | 防止 CPU tile worker 的结果重新进入 GPU/softpipe 上传链路 |
| `Source/WebCore/platform/graphics/skia/SkiaBackingStore.{cpp,h}` | CPU tile 保存在 raster `SkSurface`，局部更新直接 `writePixels()`，合成时使用 raster `SkImage` | 将 tile 链路从 CPU-to-GPU 改成 CPU-to-CPU |
| `Source/WebCore/platform/graphics/skia/SkiaCompositingLayer.cpp` | mask 和 intermediate surface 在 CPU compositor 下使用 raster surface | 保证滤镜、遮罩和复杂图层在无 GL compositor context 时仍可合成 |
| `Source/WebCore/platform/graphics/skia/GraphicsContextSkia.cpp` | CPU tile 录制时把 clip 等纹理图像同步转成 raster，并禁止记录 GPU fence | 避免 raster worker 进入依赖 EGL wait 扩展的 `SkiaReplayCanvas` 路径 |
| `Source/WebCore/platform/graphics/skia/PatternSkia.cpp` | CPU compositor 创建 pattern shader 前将纹理 tile 转成 raster image | 防止复杂背景 pattern 把 GPU image/fence 带入 CPU tile 录制结果 |
| `Source/WebCore/platform/graphics/egl/GLFence.cpp` | CPU compositor 禁用内部 GL fence，使 Skia 使用同步 submit；外部导入的 Linux sync fd 使用 `poll()` 等待 | 兼容错误宣称 EGL fence 扩展但不导出 wait API 的软件 EGL 栈，且不影响 GPU profile |
| `Source/WebCore/page/scrolling/coordinated/ScrollerCoordinated.cpp` | CPU compositor 将异步滚动条直接绘制成 raster NativeImage | 避免滚动条单独创建 softpipe `BitmapTexture` 和 GL fence |
| `Source/WebCore/platform/graphics/texmap/coordinated/CoordinatedPlatformLayerBufferNativeImage.cpp` | raster NativeImage 在 Skia compositor 中直接返回底层 `SkImage` | 防止 raster 内容再次上传成 GL texture |
| `Source/WebCore/platform/graphics/texmap/coordinated/CoordinatedPlatformLayerBufferRGB.cpp` | CPU compositor 对意外残留的纯 GL platform buffer 记录一次并安全跳过 | 防止无 GL context 时断言崩溃，并暴露仍需迁移的生产者 |
| `Source/WebCore/platform/gstreamer/GStreamerQuirks.cpp` | 注册 `rockchip` hole-punch quirk（`WEBKIT_GST_HOLE_PUNCH_QUIRK=rockchip` 可选） | 视频 KMS overlay 直出功能的注册点 |
| `Source/WebCore/platform/SourcesGStreamer.txt` | 加入 `GStreamerHolePunchQuirkRockchip.cpp` 编译条目 | 同上，构建系统需要知道这个新文件 |
| `Source/WebCore/platform/graphics/gstreamer/MediaPlayerPrivateGStreamer.cpp` | `configureElement()` 里检测本管线是否挂了 hole-punch sink，是则给 `mppvideodec` 设 RGA 预旋转 `rotation` 属性 | 视频直出功能需要解码器按面板方向硬件预旋转，且不能影响软件回退管线 |
| `0002-registry-scanner-classify-mpp-as-hardware.patch` | 无论普通 quirk 列表是否为空，都调用平台硬件分类器 | Rockchip 只注册 hole-punch quirk；旧条件会发现 `mppvideodec` 却错误报告 `isUsingHardware=false`，导致云游戏平台拒绝 VPU 能力 |
| `Source/WebCore/platform/graphics/texmap/coordinated/CoordinatedBackingStoreProxy.{cpp,h}` | tile 预取倍数默认从 2× 改为 1× | 960x266 小屏上 2× 恒定预取是内存压力监控被禁用时代遗留的保守默认值，浪费光栅/驻留内存 |
| `Source/WebCore/platform/network/soup/SoupNetworkSession.cpp` | `max-conns` 256→96，`max-conns-per-host` 6→8 | 单任务设备降低总连接数省内存，同时提高单站点并行度改善移动站首屏加载 |
| `Source/WebCore/Modules/mediastream/gstreamer/GStreamerMediaEndpoint.cpp` | 对 Xbox 和 `mihoyo.com` 云游戏页面强制使用 `max-bundle`；在调用 `set-remote-description` 前先强制本端 ICE 为 controlling，并在远端 Offer、本地 Answer 应用后重复校验 | GStreamer 1.22 不实现默认 `balanced` bundle policy；部分云游戏节点不会主动发送 `USE-CANDIDATE`。conncheck 开始后 libnice 会拒绝 role 切换，因此只在 description 回调后修改仍会 `checking -> failed` 并报首帧超时 `-5001` |
| `0035-cloud-sdp-transport-diagnostics.patch` | 只记录 BUNDLE mids 和 application m-line 的 proto、port、mid、setup、sctp-port | 在不记录 SDP、candidate 或 fingerprint 的前提下确认 DataChannel transport 协商是否完整 |
| `0036-cloud-ice-controller-toggle.patch` | 增加 `WEBKIT_CLOUD_FORCE_ICE_CONTROLLER=0|1` 诊断开关，默认保持 `1` | 对云原神做可回滚 ICE role A/B；实测关闭后 ICE 仍 completed，但 SCTP 仍无 INIT_ACK，已排除该 role quirk 是当前 DataChannel 卡点 |
| `Source/WebCore/Modules/mediastream/gstreamer/GStreamerDataChannelHandler.cpp` | 在 `WEBKIT_CLOUD_DATACHANNEL_DIAGNOSTICS=1` 时记录 DataChannel 创建、状态转换、关闭与收包字节数，不记录负载 | 确认云游戏控制通道是否真正打开并承载服务器控制消息，同时避免把会话数据写入设备日志 |
| `0028-cloud-websocket-outbound-diagnostics.patch` | 在 `WEBKIT_CLOUD_WEBSOCKET_DIAGNOSTICS=1` 时记录 WebSocket 的发送类型/长度和本地关闭请求，不记录内容 | 区分网页未完成云端启动协议、本地主动关闭与服务端异常断开，避免错误归因到 RTP 或 DRM |
| `0029-cloud-websocket-endpoint-diagnostics.patch` | 将无载荷 WebSocket 帧计数与关闭事件关联到 host/port，并关闭诊断开关时不输出入站消息 | 区分云端信令、控制和测速 socket，避免把其中一条连接的关闭误判为媒体会话失败 |
| `0030-cloud-websocket-request-metadata.patch` | 在握手失败时只记录标准 WebSocket/fetch/client-hint 头是否存在 | 在不记录值或认证信息的前提下判断 5443 是否因浏览器请求元数据缺失而被拒绝 |
| `0037-cloud-websocket-envelope-metadata.patch` | 仅记录云游戏二进制 WebSocket 消息的 8 字节 envelope 类型和长度，不记录载荷 | 确认 Proxy、Handshake、Signaling、KeepAlive 控制阶段是否完整到达 |
| `Source/WebKit/NetworkProcess/soup/NetworkSessionSoup.cpp` | 仅在 `WPE_CLOUDGAME_ALLOW_EXPIRED_TLS=1` 时放行 `*.mhystatic.com:5443` 的 WSS 证书过期错误；未知 CA、域名不匹配及其他 TLS 错误仍拒绝 | 云原神信令服务器证书已过期，原行为会导致 WebSocket 失败并返回 `-5001`；例外严格限制在信令域名、端口、协议和错误类型 |
| `0017-webrtc-h264-parser-allocation-probe.patch` | 在 H.264 parser 输出端探测 allocation query，避免错误的下游内存协商阻塞 early decode | 将卡点从 parser allocation 明确推进到 MPP/MediaStream 播放器 |
| `0018-webrtc-copy-decoded-video-to-system-memory.patch` | 提供显式 malloc-backed decoded sample 深拷贝调试路径 | 用于隔离 Rockchip DMA-BUF 生命周期；启用后无法走视频 overlay，因此生产默认关闭 |
| `0019-webrtc-early-mpp-h264-access-units.patch` | 要求 early MPP 输入为 H.264 byte-stream、AU 对齐 | 修复 MPP 只收少量 frame 后冻结的问题，使真实云游戏解码可持续增长 |
| `0020-webrtc-skip-player-audio-diagnostic.patch` | 可显式跳过 MediaStream player 的 audio source | 证明无 caps/sample 的音频 appsrc 会阻塞 playbin，诊断模式下视频可达约 60fps 并进入 NV12 DRM plane |
| `0021-webrtc-defer-player-audio-until-video-preroll.patch` | 视频播放器先暴露 video source，视频 preroll 稳定后再动态挂载 audio；10 秒无视频则保持 video-only | 保留音频能力，同时避免初始空音频 pad 阻塞云游戏首帧；可用 `WEBKIT_GST_WEBRTC_DEFER_PLAYER_AUDIO=0` 回滚 |
| `0022-webrtc-map-balanced-to-max-bundle.patch` | 将 GStreamer 1.22 不支持的 `balanced` bundle policy 映射为 `max-bundle` | 防止 Answer 丢失 `a=group:BUNDLE` 后，复用同一 ICE/DTLS transport 的视频 RTP 在音频 session 中找不到 payload clock-rate |
| `0023-webrtc-allow-player-audio-before-video.patch` | incoming WebRTC video player 允许音频样本在首个视频样本前进入 live appsrc | 消除“先丢音频、playbin 又等待音频 preroll”的死锁，并避免 0021 动态添加音频触发视频支路重配置冻结；可用三个环境变量分别回滚 |
| `0038-webrtc-do-not-suspend-mediastream-offscreen.patch` | MediaStream 播放器不参与普通静音离屏视频的 suspend 策略 | `<hole>`/DRM 直出时 WebKit 可能认为视频不在 viewport；远端实时媒体不能因此停在 PAUSED |
| `0039-webrtc-do-not-gate-audio-only-player-on-video.patch` | 纯音频 MediaStream consumer 不再等待同一父 stream 的首个视频 sample | 云游戏把音频和视频接到不同元素时，音频元素永远收不到视频 sample，旧逻辑会永久丢音频 |
| `0040-webrtc-retimestamp-audio-only-mediastream.patch` | incoming audio-only consumer 也启用 MediaStream 本地时间戳重建 | 避免远端 RTP 时间线直接进入 live appsrc 后出现巨大的等待时间 |
| `0041-webrtc-use-direct-audio-sink.patch` | MediaStream 默认绕过 WebAudio provider wrapper，直接使用平台 audio sink | 缺少 WebAudio 子插件时 wrapper 会形成只有 tee、没有实际 sink 的半成品 bin |
| `0042-webrtc-allow-forced-direct-audio-sink.patch` | `WEBKIT_GST_MEDIASTREAM_DIRECT_AUDIO_SINK=1` 可在 URI/source 尚未挂载时强制直连音频 sink | `createAudioSink()` 的调用时机早于可靠的 `isMediaStreamPlayer()` 判断 |
| `0043-webrtc-retimestamp-audio-from-zero.patch` | incoming audio buffer 从本地首包起按零基准重建 PTS/DTS | 防止 `do-timestamp` 在异步状态切换期间生成绝对单调时钟 PTS，使 sink 等待远未来时间 |
| `0044-webrtc-decode-audio-before-mediastream-player.patch` | incoming audio 可在 track processor 中提前解码 | 将编码 Opus 与 MediaStream player preroll 解耦；该动态 `decodebin3` 方案后续由 0045 静态链替代 |
| `0045-webrtc-use-static-opus-decoder-chain.patch` | Opus 使用 `rtpopusdepay -> opusdec -> audioconvert -> audioresample` 静态链 | 实机 `decodebin3` 在 READY 到 PAUSED 阶段阻塞；固定协商后的 Opus 不需要动态解码器选择 |
| `0046-webrtc-fix-static-opus-bin-ownership.patch` | 修正静态 Opus bin 内 floating reference 的所有权转移 | 0045 首版父 bin 存活但子 element 被提前释放，导致空处理链 |
| `0047-webrtc-disable-audio-preroll-async.patch` | incoming audio 的中间 `fakesink` 设置 `async=false`，视频仍保持异步状态切换 | 中间 sink 的 preroll 会吞掉父 bin 的 PLAYING 请求，只放行一个音频 sample；关闭音频异步 preroll 后可持续输出到 ALSA |
| `0048-webrtc-defer-static-audio-ready.patch` | 静态 Opus processor 在第一帧解码 PCM 到达后才通知 `trackReady()`；`WEBKIT_GST_WEBRTC_DEFER_STATIC_AUDIO_READY=0` 可回滚原时序 | `configure()` 发生在 bin 加入 WebRTC pipeline、外部 pad link 和 PAUSED 之前；同步 `trackReady()->connectIncomingTrack()->setBin()` 会与这些状态切换交叉，迟到音轨可将 WebProcess 主线程永久卡死 |
| `0049-webrtc-block-first-static-audio-sample.patch` | 用 `BLOCK_DOWNSTREAM` probe 暂停第一帧 PCM，主线程完成 `setBin()` 后移除 probe并放行首帧 | 0048 虽推迟 ready，但首帧继续向 fakesink 推送时，主线程仍会在给同一 sink 安装 handoff/probe 时等待 pad/state 锁；明确的阻塞握手消除并发修改 |
| `0054-webrtc-keep-flush-events-pipeline-local.patch` | teardown 时只在当前 WebRTC endpoint pipeline 内发送 flush，不再把 flush 传播到共享 MediaStream player | 避免第二次连接/关闭时跨 pipeline flush 造成主线程锁反转和 WebProcess 卡死 |
| `0055-rockchip-overlay-acquire-on-first-frame.patch` | hole-punch overlay 所有权延迟到首个有效 NV12 DMA-BUF 帧，并允许 750ms 无帧的旧 owner 被替换 | 防止无有效视频的旧 sink 永久占用唯一 DRM 视频 overlay，导致新会话只有黑屏 |
| `0056-webrtc-direct-remote-audio-sink.patch` | 解码后的远端 Opus 在 endpoint pipeline 内经 `tee + leaky queue` 直连平台音频 sink；MediaStream player 跳过该音轨 | 消除音频 appsrc/playbin preroll 对共享 BUNDLE transport 的反压；实机 Janus AV 与云原神均能持续视频、音频和 DataChannel |
| `0057-webrtc-overlay-rotation-and-aspect-fit.patch` | WebRTC early decode 创建 `mppvideodec` 时按 DRM 输出方向设置 RGA 旋转；DRM 视频 plane 增加 `contain/cover/stretch` 等比适配 | 修复云游戏 NV12 帧未预旋转且被非等比铺满 CRTC，导致画面方向错误和纵向拉伸 |
| `0058-fullscreen-video-touch-mapping.patch` | 将视频 overlay 的 DOM 矩形和 `contain` 后可见矩形发布到同进程 WPEView；launcher 在网页全屏游戏模式下反算触摸坐标 | 修复等比留黑后视觉画面与 DOM 输入范围不一致，保证云游戏触摸位置与画面一致 |
| `Source/WTF/Scripts/Preferences/UnifiedWebPreferences.yaml` | `PropagateDamagingInformation` 在 `PLATFORM(WPE)` 下默认改为 `true` | 让 CPU 合成路径只重绘脏区，而不是每帧全屏重绘 |
| `Source/WebKit/WPEPlatform/wpe/drm/CMakeLists.txt` | 为 `WPEPlatformDRM` 显式链接 `Freetype::Freetype` | 原生 chrome 需要用包内 NotoSansSC 绘制中文 Profile 名、历史和收藏标题；字体不可用时仍回退 5x7 ASCII 字库 |

## 构建配置（非源码文件，记在这里以防遗漏）

以下是服务器 `build-drm-aarch64-deb12` 的 cmake cache 配置，不对应任何源码文件改动，但同样是"内核定制"的一部分，只存在于服务器 `CMakeCache.txt` 里：

- `-mcpu=cortex-a53 -fomit-frame-pointer -flto=thin`，链接强制 `-fuse-ld=lld`
- 裁剪：`ENABLE_SAMPLING_PROFILER` / `ENABLE_REMOTE_INSPECTOR` / `ENABLE_WEBDRIVER` / `ENABLE_JAVASCRIPT_SHELL` / `ENABLE_PDFJS` / `ENABLE_MATHML` / `ENABLE_GPU_PROCESS` 全部 `OFF`
- PGO：`-DUSE_PGO_PROFILE=ON -DPGO_PROFILE_PATH=<merged.profdata>`（采样数据不入库，太大且是派生产物）
- CPU profile 默认 `WEBKIT_SKIA_CPU_COMPOSITOR=1`，完整路径为 Skia raster tile → raster SHM/dma-heap framebuffer → WPE DRM；设为 `0` 可即时回滚旧 softpipe GL compositor。GPU profile固定为 `0`。

详见 `~/.claude/projects/.../memory/wpe-build-server.md`（本机 Claude 记忆文件，不在仓库里）。
### `0034-drm-seat-reserve-raw-touch-device.patch`

Prevents WPE DRM's libinput seat and `wpe-drm-minimal` raw-touch handler from
opening the same evdev touchscreen with independent readers. The raw-touch path
owns `WPE_TOUCH_DEVICE`; libinput continues to handle all other seat devices.
Set `WPE_RAW_TOUCH_EXCLUSIVE=0` only for platform-input diagnostics.
