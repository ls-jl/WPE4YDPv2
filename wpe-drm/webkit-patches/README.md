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
| `Source/WebCore/platform/graphics/texmap/coordinated/CoordinatedBackingStoreProxy.{cpp,h}` | tile 预取倍数默认从 2× 改为 1× | 960x266 小屏上 2× 恒定预取是内存压力监控被禁用时代遗留的保守默认值，浪费光栅/驻留内存 |
| `Source/WebCore/platform/network/soup/SoupNetworkSession.cpp` | `max-conns` 256→96，`max-conns-per-host` 6→8 | 单任务设备降低总连接数省内存，同时提高单站点并行度改善移动站首屏加载 |
| `Source/WebCore/Modules/mediastream/gstreamer/GStreamerMediaEndpoint.cpp` | 对 Xbox 和 `mihoyo.com` 云游戏页面强制使用 `max-bundle`；远端 Offer 应用后强制本端 ICE 为 controlling | GStreamer 1.22 不实现默认 `balanced` bundle policy；部分云游戏节点会响应 connectivity check，但不会主动发送 `USE-CANDIDATE`，若本端保持 controlled 会出现候选全部 valid、nominated 为 0 并最终报首帧超时 `-5001` |
| `Source/WebKit/NetworkProcess/soup/NetworkSessionSoup.cpp` | 仅在 `WPE_CLOUDGAME_ALLOW_EXPIRED_TLS=1` 时放行 `*.mhystatic.com:5443` 的 WSS 证书过期错误；未知 CA、域名不匹配及其他 TLS 错误仍拒绝 | 云原神信令服务器证书已过期，原行为会导致 WebSocket 失败并返回 `-5001`；例外严格限制在信令域名、端口、协议和错误类型 |
| `Source/WTF/Scripts/Preferences/UnifiedWebPreferences.yaml` | `PropagateDamagingInformation` 在 `PLATFORM(WPE)` 下默认改为 `true` | 让 CPU 合成路径只重绘脏区，而不是每帧全屏重绘 |
| `Source/WebKit/WPEPlatform/wpe/drm/CMakeLists.txt` | 为 `WPEPlatformDRM` 显式链接 `Freetype::Freetype` | 原生 chrome 需要用包内 NotoSansSC 绘制中文 Profile 名、历史和收藏标题；字体不可用时仍回退 5x7 ASCII 字库 |

## 构建配置（非源码文件，记在这里以防遗漏）

以下是服务器 `build-drm-aarch64-deb12` 的 cmake cache 配置，不对应任何源码文件改动，但同样是"内核定制"的一部分，只存在于服务器 `CMakeCache.txt` 里：

- `-mcpu=cortex-a53 -fomit-frame-pointer -flto=thin`，链接强制 `-fuse-ld=lld`
- 裁剪：`ENABLE_SAMPLING_PROFILER` / `ENABLE_REMOTE_INSPECTOR` / `ENABLE_WEBDRIVER` / `ENABLE_JAVASCRIPT_SHELL` / `ENABLE_PDFJS` / `ENABLE_MATHML` / `ENABLE_GPU_PROCESS` 全部 `OFF`
- PGO：`-DUSE_PGO_PROFILE=ON -DPGO_PROFILE_PATH=<merged.profdata>`（采样数据不入库，太大且是派生产物）
- CPU profile 默认 `WEBKIT_SKIA_CPU_COMPOSITOR=1`，完整路径为 Skia raster tile → raster SHM/dma-heap framebuffer → WPE DRM；设为 `0` 可即时回滚旧 softpipe GL compositor。GPU profile固定为 `0`。

详见 `~/.claude/projects/.../memory/wpe-build-server.md`（本机 Claude 记忆文件，不在仓库里）。
