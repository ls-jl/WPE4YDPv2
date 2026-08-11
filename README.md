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
- **RGA 页面旋转快路径**：旋转设备上的 WebKit DMABuf 由包内 `librga.so.2` 直接写入 DRM dumb scanout，避免 CPU 每帧映射、读取和旋转 Mali buffer；RGA 不可用时自动回退原 CPU 路径。
- **视频 KMS overlay 直出（hole-punch）**：`mppvideodec` VPU 硬解 + RGA 硬件预旋转输出 NV12 dmabuf，经 v2 unix socket 协议送 UI 进程放到空闲 Esmart overlay plane；解码样本在收到对应 sequence release ACK 前不会复用。
- **WebRTC 云游戏链路**：WebKit 使用 GStreamer PeerConnection backend，包内提供 ICE/DTLS/SRTP/SCTP/RTP/Opus；允许远端音视频和 DataChannel，默认拒绝麦克风、摄像头和屏幕采集。云原神域名自动切换到原生多点触摸 `game` 输入模式。
- **内核按机型深度定制**：`-mcpu=cortex-a53` + ThinLTO + **PGO**（抖音/bilibili/百度等真机负载采样 profile-use）；裁剪 SAMPLING_PROFILER / REMOTE_INSPECTOR / WEBDRIVER / JAVASCRIPT_SHELL / PDFJS / MATHML / GPU_PROCESS 等。
- **低内存自保**：按启动余量选择 `448/512/640MB` 统一内存 Profile；1GB balanced 模式先在 40%/58% 回收，96% 才终止 WebProcess，避免系统仍有余量时过早停止重型页面；同一 URL 再次超限后显示可重试错误页，禁止循环重载。
- **低风险内存策略**：默认不改全局 swappiness、不执行 `drop_caches`；仅调整 WPE 自身 OOM 优先级，并通过显式诊断开关临时启用系统级实验。
- **移动 UA + 站点档案**：默认 Android Chrome UA；支持按站点切换 desktop/mobile 档案。
- **多用户 Profile 与 Cookie 持久化**：最多 8 个持久 Profile 和访客模式；Cookie、LocalStorage、IndexedDB、Service Worker、缓存、标签、历史、收藏和站点档案完全隔离。浏览器元数据使用 SQLite WAL，Cookie 使用 WebKit 独立 SQLite CookieJar。
- **Chrome Material 原生界面**：浅色/深色主题、共享圆角卡片和 native 图标、按压态、开关/单选/危险操作样式；GPU 旋转路径缓存完整 CPU 底图，菜单动画只重画 chrome，不重复读取 Mali DMA-BUF。
- **分组设置**：外观、网页、启动与搜索、隐私与数据、语言、关于。主题和工具栏自动隐藏全局保存；缩放、字体、网页能力、搜索引擎、语言和启动行为按 Profile 隔离，Guest 网页设置只在当前会话生效。语言同时控制 native 菜单、`Accept-Language` 和 `navigator.language(s)`。
- 原生工具栏 `inset` 布局（显隐不 resize WebView）、Chrome 风格三点分层菜单、地址栏直接键盘编辑、横向滚动、系统键盘桥、双显示模式（原生/横屏旋转）。

## 目录结构

- `src/`：MiniApp 前端壳（`pages/index` 启动页、`pages/frame` 全屏 hole 承载页、`utils/` 显示解析/生命周期/键盘桥）。
- `jsapi/`：MiniApp native JSAPI（模块名 `browser`）源码。
- `libs/arm64-orange/libjsapi_browser.so`：JSAPI so（唯一入库源；`libs/libjsapi_browser_12345.so` 由构建脚本生成，不入库）。
- `assets/wpe-runtime/`：包内 WPE runtime。
  - `run.sh`：启动脚本（由 `scripts/sync_generated.sh` 从 `wpe-drm/run.sh` 单源同步），所有运行期调优 env 的唯一维护处。
  - `wpe-drm-minimal`：Direct DRM 浏览器壳（chrome 工具栏/Profile/多标签/历史/收藏/Cookie/键盘桥/崩溃熔断）。
  - `lib/libWPEWebKit-2.0.so.1`：定制 WebKit 2.53.3 的唯一运行时 ELF（上游文件版本 1.10.2，Git LFS）。包内不保留会被 Falcon 展开成完整副本的 `.so` 软链接。`lib/gstreamer-1.0/` 含设备拷入的解码插件。
  - `tests/webrtc-loopback.html`：无需本机编码器的 ICE + DataChannel + 本地采集拒绝自测页。
  - `gpu/`：本地可选 Mali 用户态库和 `5.10.160` KO；专有二进制不提交 Git，由 `scripts/stage_gpu_runtime.sh` 按 SHA256 组装。
- `wpe-drm/`：Direct WPE launcher、DRM 平台源码和可复现 WebKit 补丁：
  - `wpe-drm-minimal.c`、`run.sh`：应用层。
  - `WPEViewDRM.cpp`（含 VideoOverlay 模块）、`WPEDisplayDRM.cpp/Private.h`、`WPEDRM.h/cpp`：WPEPlatform DRM 后端，对应 `Source/WebKit/WPEPlatform/wpe/drm/`。
  - `GStreamerHolePunchQuirkRockchip.{h,cpp}`：视频直出 WebProcess 端 quirk，对应 `Source/WebCore/platform/gstreamer/`。
  - `browser-chrome-model.*`、`browser-navigation.*`、`browser-profile-store.*`：可独立测试的面板、导航和数据库模块。
  - `runtime/`：由 `run.sh` 加载的 supervisor 子模块；构建时同步到包内。
  - `webkit-patches/{revision,series}`：从固定 WebKit revision 重放的四组生产补丁，不再保存实验补丁和整文件镜像。
- `tests/fixtures/web/`：本地网页、RAF、触摸、音视频 fixture，不随页面业务代码维护。
- `tools/diagnostics/`：诊断 C 工具及统一 Makefile。
- `tools/`：出屏、键盘、云游戏排障和 runtime 体积文档。
- `8001779591038449.1_0_0.amr`：打包产物（Git LFS 入库）。

## 构建与打包

```sh
npm run build          # 产出 8001779591038449.1_0_0.amr
npm test               # 逻辑 fixture、SQLite、shell、runtime 与 patch series 检查
```

构建前自动执行 `scripts/sync_generated.sh` 同步单源文件。**assets 变更后打包若报 xkb 相关 ENOENT，先清缓存**：

```sh
rm -rf .falcon_ .falcon_tmp && npm run build
```

WebKit 内核与 `wpe-drm-minimal` 在 ARM64 交叉编译服务器上构建。`scripts/build_webrtc_runtime_pve.sh` 会从 `wpe-drm/webkit-patches/revision` 创建隔离 worktree，按 `series` 重放生产补丁，并输出记录 revision、series hash、编译开关和 ELF SHA256 的 `build-manifest.json`。禁止继续原地修改长期共享 WebKit 源码树。

## 运行

1. 安装 amr（`miniapp_cli install <amr>`）。
2. 启动 MiniApp 进入 `index` 启动页，选显示模式，点启动浏览器；或直接 `miniapp_cli start 8001779591038449 frame`。
3. `frame` 页显示全屏 `<hole>`，WPE 从包内 runtime 起并直接 DRM 出屏。Home/退出时 JSAPI 停 WPE 释放 DRM。

可写数据在 `$dataDir/browser/`：`wpe-drm.log`（每次启动截断重写）、`browser.sqlite3`（Profile/标签/历史/收藏）、`profiles/p<ID>/runtime/{data,cache}`（站点数据）、`profiles/p<ID>/cookies.sqlite`（CookieJar）、`chrome-render-state.ini`（轻量绘制 IPC）、`keyboard/`、`fontconfig-cache/` 等。旧 `browser-state.ini` 和 `runtime-tmp` 仅在首次升级时迁入 DEFAULT，随后分别备份为 `.migrated.bak` 和移动到 Profile 目录。

### 常用运行期开关（env，均有默认值，详见 run.sh）

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `WPE_VIDEO_OVERLAY` | `1` | 视频 KMS overlay 直出，`0` 回退软件路径 |
| `WPE_CHROME_LAYOUT` | `resize` | 工具栏展开时真实压缩网页 viewport，避免覆盖页面顶部 |
| `WPE_USE_SYSTEM_GST` | `0` | `1` 时附加系统 GStreamer 插件目录（调试用） |
| `WPE_GPU_MODE` | `auto` | `auto` 探测失败回 CPU；`off` 强制 CPU；`required` GPU 失败即退出 |
| `WPE_GPU_MODE_FILE` | `$WPE_VAR_DIR/gpu-mode` | 设置页 GPU 开关的启动前镜像；`auto` 开启，`off` 关闭 |
| `WPE_WEBRTC` | `1` | 启用 GStreamer WebRTC backend |
| `WPE_WEBRTC_CAPTURE` | `deny` | 本地音视频采集策略；产品模式必须保持 `deny` |
| `WEBKIT_GST_FORCE_DEFAULT_ALSA_SINK` | `1` | 强制浏览器音频走系统 `default` ALSA PCM，继承系统音量路由 |
| `WEBKIT_GST_SYSTEM_VOLUME_MUTE_BRIDGE` | `1` | 系统 Master 为最低值时在 WebRTC direct sink 前执行真静音 |
| `WPE_INPUT_PROFILE` | `auto` | `auto` 按域名选择 `browser/game`；`game` 不把拖动转换成滚动 |
| `WPE_CHROME_GESTURE_*` | `28px/48px/160ms/350ms/800ms/500ms` | 双指三击移动、位置、落指、单击、间隔和静默判定阈值 |
| `WPE_PROFILE_OVERRIDE` | 空 | `guest` 或持久 Profile ID；正常启动留空以恢复上次用户 |
| `WPE_BROWSER_DB` | `$WPE_VAR_DIR/browser.sqlite3` | 浏览器 Profile 元数据 SQLite |
| `WPE_DRM_FENCE_TIMEOUT_MS` | `2000` | CPU 读取 DMA-BUF 前等待 rendering fence 的上限 |
| `WPE_DRM_PARTIAL_COPY` | `0` | 默认整帧复制以避免丢帧后出现残缺块 |
| `WPE_DRM_RGA_ROTATION` | `auto` | 旋转 DMABuf 优先使用包内 RGA；`off` 强制 CPU，`required` 用于诊断 |
| `WPE_TOUCH_HORIZONTAL_SCROLL` | `1` | 宽页横向滑动 |
| `WEBKIT_SKIA_ENABLE_CPU_RENDERING` | `1` | Skia 原生 CPU 光栅化（勿走软件 GL 模拟） |
| `WEBKIT_DISPLAY_REFRESH_THROTTLE_FPS` | `30` | 合成帧率上限 |
| `WEBKIT_FORCE_FULL_COMPOSITOR_REPAINT_ANIMATIONS` | `0` | 动画全帧重绘诊断开关；生产关闭以免游戏持续满帧重绘 |
| `WPE_MEMORY_PROFILE` | 自适应 | `conservative/balanced/large` 统一选择 WebProcess、JSC、MSE 和压力阈值 |
| `WPE_WEB_PROCESS_MEMORY_LIMIT_MB` | `448/512/640` | 按整机内存、启动余量和 swap 自动选择；显式设置时仍受安全范围校验 |
| `WPE_WEB_PROCESS_MEMORY_KILL_PERCENT` | `80/96/90` | `conservative/balanced/large` 的最终终止水位；标准和严格回收会先执行 |
| `WEBKIT_SYSTEM_MEMORY_PRESSURE_PERCENT` | `80/82/85` | 各 Profile 的整机 warning 阈值；critical 默认 `88/90/93` |
| `MSE_MAX_BUFFER_SIZE` | 自适应 | 每 SourceBuffer 为 `V:24M,A:4M`、`V:32M,A:6M` 或 `V:40M,A:8M` |

## 设备调试

```sh
adb shell "ps | grep -E 'wpe-drm-minimal|WPEWebProcess|WPENetworkProcess'"
adb shell "tail -n 200 /userdisk/secondary/miniapp/data/mini_app/pkg/8001779591038449/data/browser/wpe-drm.log"
adb shell "cat /sys/kernel/debug/dri/0/state"        # DRM plane 状态（视频直出看 Esmart overlay plane 是否挂 NV12 fb）
adb shell "killall wpe-drm-minimal WPEWebProcess WPENetworkProcess"
```

WebRTC 基础验收可在地址栏打开包内 `tests/webrtc-loopback.html`。成功日志应包含 `PASS ICE + DataChannel`；云游戏开始推流后还应看到 `mppvideodec`，并在 DRM state 中出现 NV12 framebuffer。自测页不发送视频，因为产品 runtime 不打包本地视频编码器。

包内 WebKit 只保留与 ELF SONAME 一致的实体 `libWPEWebKit-2.0.so.1`。不要重新加入 `.so` 或 `.so.1.10.2` 软链接：Falcon 会解引用它们并在 AMR 与设备安装目录中生成完整重复副本。热替换时只替换 `.so.1`。

Profile 切换由 `wpe-drm-minimal` 以退出码 `75` 请求，`run.sh` 只重启 WPE 子进程；MiniApp watchdog、DRM hole 和已加载的 GPU 模块保持不变。退出 Guest 后其 ephemeral NetworkSession 和临时目录会被删除。

电源按钮退出码为 `74`。`run.sh` 原子写入 `$WPE_VAR_DIR/browser-exit.json`，MiniApp watchdog 消费后返回启动页并显示“浏览器已关闭”；异常退出会显示对应错误码。About 页展示的是实际渲染路径（`MALI-G52 GPU` 或明确的 Skia CPU 回退原因），不是 GPU 开关偏好值。

## Git / 大文件

Git LFS 管理超大文件：`assets/wpe-runtime/lib/libWPEWebKit-2.0.so.1`、`*.amr` 打包产物。

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
- runtime 瘦身基线与白名单策略：见 `tools/RUNTIME_INVENTORY.md`。
