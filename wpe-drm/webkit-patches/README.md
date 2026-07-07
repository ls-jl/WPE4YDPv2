# WebKit 源码树补丁（非 wpe-drm/ 部分）

`wpe-drm/` 根目录的文件是 WPEPlatform DRM 后端（`Source/WebKit/WPEPlatform/wpe/drm/`）和视频直出 quirk 的镜像，构建服务器 WebKit 树里对应文件就是它们的副本。

除此之外，还在服务器 WebKit 树里对其他子系统打了几处源码级补丁——这些改动只存在于服务器（`~/wpe-lite2/stripped/WebKit/`，每处都留了 `.bak-claude-*` 备份保存补丁前的原始版本），此前没有同步回本仓库。本目录按 `Source/` 下的原始相对路径镜像这几个**补丁后**的文件，作为唯一的版本历史备份；每次改动前先在服务器上更新这里，再 scp 回服务器对应路径。

## 补丁清单

| 文件 | 做了什么 | 为什么 |
| --- | --- | --- |
| `Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/AcceleratedSurface.cpp` | `SwapChain` 初始化时先探测 `WPE_DRM_DMA_HEAP` 指向的 heap 设备能否打开，打不开就降级 `SharedMemory` 而不是返回空渲染目标 | 2026-04 起的固件内核移除了 `/dev/dma_heap/system`，`WPE_DRM_BUFFER_PATH=dma_heap` 强制路径原本无回退，导致 WebProcess 一帧都产不出、plane 空置黑屏且无任何报错（用户反馈黑屏问题的根因） |
| `Source/WebCore/platform/gstreamer/GStreamerQuirks.cpp` | 注册 `rockchip` hole-punch quirk（`WEBKIT_GST_HOLE_PUNCH_QUIRK=rockchip` 可选） | 视频 KMS overlay 直出功能的注册点 |
| `Source/WebCore/platform/SourcesGStreamer.txt` | 加入 `GStreamerHolePunchQuirkRockchip.cpp` 编译条目 | 同上，构建系统需要知道这个新文件 |
| `Source/WebCore/platform/graphics/gstreamer/MediaPlayerPrivateGStreamer.cpp` | `configureElement()` 里检测本管线是否挂了 hole-punch sink，是则给 `mppvideodec` 设 RGA 预旋转 `rotation` 属性 | 视频直出功能需要解码器按面板方向硬件预旋转，且不能影响软件回退管线 |
| `Source/WebCore/platform/graphics/texmap/coordinated/CoordinatedBackingStoreProxy.{cpp,h}` | tile 预取倍数默认从 2× 改为 1× | 960x266 小屏上 2× 恒定预取是内存压力监控被禁用时代遗留的保守默认值，浪费光栅/驻留内存 |
| `Source/WebCore/platform/network/soup/SoupNetworkSession.cpp` | `max-conns` 256→96，`max-conns-per-host` 6→8 | 单任务设备降低总连接数省内存，同时提高单站点并行度改善移动站首屏加载 |
| `Source/WTF/Scripts/Preferences/UnifiedWebPreferences.yaml` | `PropagateDamagingInformation` 在 `PLATFORM(WPE)` 下默认改为 `true` | 让 CPU 合成路径只重绘脏区，而不是每帧全屏重绘 |

## 构建配置（非源码文件，记在这里以防遗漏）

以下是服务器 `build-drm-aarch64-deb12` 的 cmake cache 配置，不对应任何源码文件改动，但同样是"内核定制"的一部分，只存在于服务器 `CMakeCache.txt` 里：

- `-mcpu=cortex-a53 -fomit-frame-pointer -flto=thin`，链接强制 `-fuse-ld=lld`
- 裁剪：`ENABLE_SAMPLING_PROFILER` / `ENABLE_REMOTE_INSPECTOR` / `ENABLE_WEBDRIVER` / `ENABLE_JAVASCRIPT_SHELL` / `ENABLE_PDFJS` / `ENABLE_MATHML` / `ENABLE_GPU_PROCESS` 全部 `OFF`
- PGO：`-DUSE_PGO_PROFILE=ON -DPGO_PROFILE_PATH=<merged.profdata>`（采样数据不入库，太大且是派生产物）

详见 `~/.claude/projects/.../memory/wpe-build-server.md`（本机 Claude 记忆文件，不在仓库里）。
