# WPE4YDPv2

[English](README.en.md) | 中文

词典笔（Rockchip RK3562，1GB RAM / 4×A53 / 多种屏幕方向 / VPU+RGA，部分固件可启用 Mali G52）上的 Direct WPE 浏览器。整体链路：

```text
MiniApp 启动壳 -> frame 页全屏 <hole> -> JSAPI fork/exec 包内 WPE runtime -> WPE 直接 DRM 出屏
                                                     └-> 视频走独立 KMS overlay plane 直出（hole-punch）
```

MiniApp 只负责启动页、生命周期、系统键盘桥和全屏 `<hole>` 承载页；网页渲染、原生工具栏、触摸滚动、DRM 提交和视频直出都由包内 WPE runtime（定制 WebKit + `wpe-drm-minimal`）负责。

## 核心特性

- **包内自包含 runtime**：WebKit、Mesa 软渲染、GStreamer、OpenSSL、libnice、libsrtp、MPP 硬解、ALSA、字体和 CA 证书全部随包，安装后不依赖外部 runtime；本地构建可额外带 Mali G52 runtime/KO。
- **GPU 自动探测与可靠回退**：`WPE_GPU_MODE=auto` 在 `/dev/mali0` 可用或 ARM64 `5.10.160` KO 成功加载后，继续验证 Mali EGL/GLES、linear DMA-BUF、CPU map 和 DRM framebuffer；任何一步失败都回到 Skia CPU + dma-heap/SHM。
- **视频 KMS overlay 直出（hole-punch）**：`mppvideodec` VPU 硬解 + RGA 硬件预旋转输出 NV12 dmabuf，经 unix socket 送 UI 进程放到空闲 Esmart overlay plane 直接扫描输出。
- **WebRTC 云游戏链路**：WebKit 使用 GStreamer PeerConnection backend，包内提供 ICE/DTLS/SRTP/SCTP/RTP/Opus；允许远端音视频和 DataChannel，默认拒绝麦克风、摄像头和屏幕采集。云原神域名自动切换到原生多点触摸 `game` 输入模式。
- **内核按机型深度定制**：`-mcpu=cortex-a53` + ThinLTO + **PGO**（抖音/bilibili/百度等真机负载采样 profile-use）；裁剪 SAMPLING_PROFILER / REMOTE_INSPECTOR / WEBDRIVER / JAVASCRIPT_SHELL / PDFJS / MATHML / GPU_PROCESS 等。
- **低内存自保**：内存上限按物理内存 68% 动态设定（1GB 机型 673MB，kill 阈值 0.92）；同一 URL 120 秒内被杀 3 次自动回主页的崩溃熔断；MSE 每 SourceBuffer 缓冲上限。
- **单任务内存倾斜**：浏览器 oom_score_adj=-600、启动时 drop_caches、运行期 swappiness=100（退出恢复），后台进程冷页压进 512MB swap。
- **移动 UA + 站点档案**：默认 Android Chrome UA；支持按站点切换 desktop/mobile 档案。
- **多用户 Profile 与 Cookie 持久化**：最多 8 个持久 Profile 和访客模式；Cookie、LocalStorage、IndexedDB、Service Worker、缓存、标签、历史、收藏和站点档案完全隔离。浏览器元数据使用 SQLite WAL，Cookie 使用 WebKit 独立 SQLite CookieJar。
- 原生工具栏 `inset` 布局（显隐不 resize WebView）、Chrome 风格三点分层菜单、地址栏直接键盘编辑、横向滚动、系统键盘桥、双显示模式（原生/横屏旋转）。

## 目录结构

- `src/`：MiniApp 前端壳（`pages/index` 启动页、`pages/frame` 全屏 hole 承载页、`utils/` 显示解析/生命周期/键盘桥）。
- `jsapi/`：MiniApp native JSAPI（模块名 `browser`）源码。
- `libs/arm64-orange/libjsapi_browser.so`：JSAPI so（唯一入库源；`libs/libjsapi_browser_12345.so` 由构建脚本生成，不入库）。
- `assets/wpe-runtime/`：包内 WPE runtime。
  - `run.sh`：启动脚本（由 `scripts/sync_generated.sh` 从 `wpe-drm/run.sh` 单源同步），所有运行期调优 env 的唯一维护处。
  - `wpe-drm-minimal`：Direct DRM 浏览器壳（chrome 工具栏/Profile/多标签/历史/收藏/Cookie/键盘桥/崩溃熔断）。
  - `lib/libWPEWebKit-2.0.so.1.10.2`：定制 WebKit（Git LFS）。`lib/gstreamer-1.0/` 含设备拷入的解码插件。
  - `tests/webrtc-loopback.html`：无需本机编码器的 ICE + DataChannel + 本地采集拒绝自测页。
  - `gpu/`：本地可选 Mali 用户态库和 `5.10.160` KO；专有二进制不提交 Git，由 `scripts/stage_gpu_runtime.sh` 按 SHA256 组装。
- `wpe-drm/`：**服务器 WebKit 树对应文件的本地镜像**（改动须双向同步）：
  - `wpe-drm-minimal.c`、`run.sh`：应用层。
  - `WPEViewDRM.cpp`（含 VideoOverlay 模块）、`WPEDisplayDRM.cpp/Private.h`、`WPEDRM.h/cpp`：WPEPlatform DRM 后端，对应 `Source/WebKit/WPEPlatform/wpe/drm/`。
  - `GStreamerHolePunchQuirkRockchip.{h,cpp}`：视频直出 WebProcess 端 quirk，对应 `Source/WebCore/platform/gstreamer/`。
  - `webkit-patches/`：**服务器 WebKit 树里其他子系统的源码级补丁**（按 `Source/` 原始路径镜像），清单和原因见 `webkit-patches/README.md`——包括 dma-heap 黑名单等。
- `tools/`：文档（`DRM_HOLE_RENDERING_PIPELINE.md` 出屏链路、`KEYBOARD_INPUT.md`、`RUNTIME_SIZE_REPORT.md` 体积清单）。
- `debug/`：本地调试页（不随包）。
- `8001779591038449.1_0_0.amr`：打包产物（Git LFS 入库）。

## 构建与打包

```sh
npm run build          # 产出 8001779591038449.1_0_0.amr
```

构建前自动执行 `scripts/sync_generated.sh` 同步单源文件。**assets 变更后打包若报 xkb 相关 ENOENT，先清缓存**：

```sh
rm -rf .falcon_ .falcon_tmp && npm run build
```

WebKit 内核与 `wpe-drm-minimal` 在 arm64 交叉编译服务器上构建（cmake 配置、PGO 采样重编流程、jsc shim 等细节较多，团队内部见构建服务器 `~/wpe-lite2/stripped/...` 文档）。

## 运行

1. 安装 amr（`miniapp_cli install <amr>`）。
2. 启动 MiniApp 进入 `index` 启动页，选显示模式，点启动浏览器；或直接 `miniapp_cli start 8001779591038449 frame`。
3. `frame` 页显示全屏 `<hole>`，WPE 从包内 runtime 起并直接 DRM 出屏。Home/退出时 JSAPI 停 WPE 释放 DRM。

可写数据在 `$dataDir/browser/`：`wpe-drm.log`（每次启动截断重写）、`browser.sqlite3`（Profile/标签/历史/收藏）、`profiles/p<ID>/runtime/{data,cache}`（站点数据）、`profiles/p<ID>/cookies.sqlite`（CookieJar）、`chrome-render-state.ini`（轻量绘制 IPC）、`keyboard/`、`fontconfig-cache/` 等。旧 `browser-state.ini` 和 `runtime-tmp` 仅在首次升级时迁入 DEFAULT，随后分别备份为 `.migrated.bak` 和移动到 Profile 目录。

### 常用运行期开关（env，均有默认值，详见 run.sh）

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `WPE_VIDEO_OVERLAY` | `1` | 视频 KMS overlay 直出，`0` 回退软件路径 |
| `WPE_CHROME_LAYOUT` | `inset` | 工具栏让位方式，`resize` 仅调试 |
| `WPE_USE_SYSTEM_GST` | `0` | `1` 时附加系统 GStreamer 插件目录（调试用） |
| `WPE_GPU_MODE` | `auto` | `auto` 探测失败回 CPU；`off` 强制 CPU；`required` GPU 失败即退出 |
| `WPE_WEBRTC` | `1` | 启用 GStreamer WebRTC backend |
| `WPE_WEBRTC_CAPTURE` | `deny` | 本地音视频采集策略；产品模式必须保持 `deny` |
| `WPE_INPUT_PROFILE` | `auto` | `auto` 按域名选择 `browser/game`；`game` 不把拖动转换成滚动 |
| `WPE_PROFILE_OVERRIDE` | 空 | `guest` 或持久 Profile ID；正常启动留空以恢复上次用户 |
| `WPE_BROWSER_DB` | `$WPE_VAR_DIR/browser.sqlite3` | 浏览器 Profile 元数据 SQLite |
| `WPE_DRM_FENCE_TIMEOUT_MS` | `2000` | CPU 读取 DMA-BUF 前等待 rendering fence 的上限 |
| `WPE_DRM_PARTIAL_COPY` | `0` | 默认整帧复制以避免丢帧后出现残缺块 |
| `WPE_TOUCH_HORIZONTAL_SCROLL` | `1` | 宽页横向滑动 |
| `WEBKIT_SKIA_ENABLE_CPU_RENDERING` | `1` | Skia 原生 CPU 光栅化（勿走软件 GL 模拟） |
| `WEBKIT_DISPLAY_REFRESH_THROTTLE_FPS` | `30` | 合成帧率上限 |
| `MSE_MAX_BUFFER_SIZE` | `V:40M,A:8M` | MSE 每 SourceBuffer 缓冲上限 |

## 设备调试

```sh
adb shell "ps | grep -E 'wpe-drm-minimal|WPEWebProcess|WPENetworkProcess'"
adb shell "tail -n 200 /userdisk/secondary/miniapp/data/mini_app/pkg/8001779591038449/data/browser/wpe-drm.log"
adb shell "cat /sys/kernel/debug/dri/0/state"        # DRM plane 状态（视频直出看 Esmart overlay plane 是否挂 NV12 fb）
adb shell "killall wpe-drm-minimal WPEWebProcess WPENetworkProcess"
```

WebRTC 基础验收可在地址栏打开包内 `tests/webrtc-loopback.html`。成功日志应包含 `PASS ICE + DataChannel`；云游戏开始推流后还应看到 `mppvideodec`，并在 DRM state 中出现 NV12 framebuffer。自测页不发送视频，因为产品 runtime 不打包本地视频编码器。

热替换内核 lib 的坑：**amr 安装时会把 lib 符号链接实体化成 `.so`/`.so.1`/`.so.1.10.2` 三份完整拷贝，动态链接器按 soname 加载 `.so.1`**——只推 `.so.1.10.2` 不够，需三份同时替换。

Profile 切换由 `wpe-drm-minimal` 以退出码 `75` 请求，`run.sh` 只重启 WPE 子进程；MiniApp watchdog、DRM hole 和已加载的 GPU 模块保持不变。退出 Guest 后其 ephemeral NetworkSession 和临时目录会被删除。

## Git / 大文件

Git LFS 管理超大文件：`assets/wpe-runtime/lib/libWPEWebKit-2.0.so.1.10.2`、`*.amr` 打包产物。

克隆前先装好 git-lfs 最省事，LFS 内容会随 `git clone` 自动下载：

```sh
brew install git-lfs   # 或 apt install git-lfs
git lfs install        # 每台机器装一次即可
git clone https://github.com/ls-jl/WPE4YDPv2.git
```

如果已经 clone 过才装 git-lfs，工作区里这两个文件此时只是几十字节的指针文本，需要补拉一次：

```sh
git lfs install && git lfs pull
```

**注意**：GitHub 网页的 Download ZIP 不会下载 LFS 真实内容（打包出来的还是指针文本），必须走 `git clone`。

`libs/libjsapi_browser_12345.so`、`.falcon_/` 等生成物与缓存不入库。Mali 专有 `.so` 和设备 KO 也被 `.gitignore` 明确排除；只有 staging 脚本、探针、GBM ABI shim 和哈希清单逻辑入库。

## 许可证

本项目原创代码（`src/`、`jsapi/`、`scripts/`、`tools/` 等）采用 [MIT License](LICENSE)。

`wpe-drm/` 目录下镜像自 WPE WebKit 项目的文件保留其原始许可证（BSD-2-Clause / LGPL-2.0-or-later），**不受根目录 `LICENSE` 覆盖**。完整的第三方许可清单、原因见 `wpe-drm/webkit-patches/README.md`。

Mali 用户态 blob 与内核模块属于厂商专有内容，不由本仓库 MIT 许可证授权再分发，也不会提交到公开 Git；使用者须自行确认设备厂商授权。

## 后续方向

- 视频 overlay 二期：无 RGA 场景 CPU NV12 旋转兜底；plane 空闲时 in-fence 同步。
- runtime 瘦身：见 `tools/RUNTIME_SIZE_REPORT.md`。
