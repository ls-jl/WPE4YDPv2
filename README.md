# WPE4YDPv2

[English](README.en.md) | 中文

词典笔（Rockchip RK3562，1GB RAM / 4×A53 / 960x266 长条屏 / 无 GPU / 有 VPU+RGA）上的 Direct WPE 浏览器。整体链路：

```text
MiniApp 启动壳 -> frame 页全屏 <hole> -> JSAPI fork/exec 包内 WPE runtime -> WPE 直接 DRM 出屏
                                                     └-> 视频走独立 KMS overlay plane 直出（hole-punch）
```

MiniApp 只负责启动页、生命周期、系统键盘桥和全屏 `<hole>` 承载页；网页渲染、原生工具栏、触摸滚动、DRM 提交和视频直出都由包内 WPE runtime（定制 WebKit + `wpe-drm-minimal`）负责。

## 核心特性

- **包内自包含 runtime**：WebKit、Mesa 软渲染、GStreamer（含从设备拷入的 MPP 硬解/ALSA 等 13 个插件）、字体、CA 证书全部随包，安装后不依赖任何外部目录。
- **视频 KMS overlay 直出（hole-punch）**：`mppvideodec` VPU 硬解 + RGA 硬件预旋转输出 NV12 dmabuf，经 unix socket 送 UI 进程放到空闲 Esmart overlay plane 直接扫描输出。
- **内核按机型深度定制**：`-mcpu=cortex-a53` + ThinLTO + **PGO**（抖音/bilibili/百度等真机负载采样 profile-use）；裁剪 SAMPLING_PROFILER / REMOTE_INSPECTOR / WEBDRIVER / JAVASCRIPT_SHELL / PDFJS / MATHML / GPU_PROCESS 等。
- **低内存自保**：内存上限按物理内存 68% 动态设定（1GB 机型 673MB，kill 阈值 0.92）；同一 URL 120 秒内被杀 3 次自动回主页的崩溃熔断；MSE 每 SourceBuffer 缓冲上限。
- **单任务内存倾斜**：浏览器 oom_score_adj=-600、启动时 drop_caches、运行期 swappiness=100（退出恢复），后台进程冷页压进 512MB swap。
- **移动 UA + 站点档案**：默认 Android Chrome UA；支持按站点切换 desktop/mobile 档案。
- 原生工具栏 `inset` 布局（显隐不 resize WebView）、横向滚动、系统键盘桥、双显示模式（原生/横屏旋转）。

## 目录结构

- `src/`：MiniApp 前端壳（`pages/index` 启动页、`pages/frame` 全屏 hole 承载页、`utils/` 显示解析/生命周期/键盘桥）。
- `jsapi/`：MiniApp native JSAPI（模块名 `browser`）源码。
- `libs/arm64-orange/libjsapi_browser.so`：JSAPI so（唯一入库源；`libs/libjsapi_browser_12345.so` 由构建脚本生成，不入库）。
- `assets/wpe-runtime/`：包内 WPE runtime。
  - `run.sh`：启动脚本（由 `scripts/sync_generated.sh` 从 `wpe-drm/run.sh` 单源同步），所有运行期调优 env 的唯一维护处。
  - `wpe-drm-minimal`：Direct DRM 浏览器壳（chrome 工具栏/多标签/历史/键盘桥/崩溃熔断）。
  - `lib/libWPEWebKit-2.0.so.1.10.2`：定制 WebKit（Git LFS）。`lib/gstreamer-1.0/` 含设备拷入的解码插件。
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

可写数据在 `$dataDir/browser/`：`wpe-drm.log`（每次启动截断重写）、`browser-state.ini`（标签/主页持久化）、`chrome-render-state.ini`、`keyboard/`、`fontconfig-cache/` 等。

### 常用运行期开关（env，均有默认值，详见 run.sh）

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `WPE_VIDEO_OVERLAY` | `1` | 视频 KMS overlay 直出，`0` 回退软件路径 |
| `WPE_CHROME_LAYOUT` | `inset` | 工具栏让位方式，`resize` 仅调试 |
| `WPE_USE_SYSTEM_GST` | `0` | `1` 时附加系统 GStreamer 插件目录（调试用） |
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

热替换内核 lib 的坑：**amr 安装时会把 lib 符号链接实体化成 `.so`/`.so.1`/`.so.1.10.2` 三份完整拷贝，动态链接器按 soname 加载 `.so.1`**——只推 `.so.1.10.2` 不够，需三份同时替换。

远程导航技巧：改 `$dataDir/browser/browser-state.ini` 的 `[tab0] url=` 后重启浏览器即恢复到该页。

## Git / 大文件

Git LFS 管理超大文件：`assets/wpe-runtime/lib/libWPEWebKit-2.0.so.1.10.2`、`*.amr` 打包产物。克隆后：

```sh
git lfs install && git lfs pull
```

`libs/libjsapi_browser_12345.so`、`.falcon_/` 等生成物与缓存不入库。

## 许可证

本项目原创代码（`src/`、`jsapi/`、`scripts/`、`tools/` 等）采用 [MIT License](LICENSE)。

`wpe-drm/` 目录下镜像自 WPE WebKit 项目的文件保留其原始许可证（BSD-2-Clause / LGPL-2.0-or-later），**不受根目录 `LICENSE` 覆盖**。完整的第三方许可清单、原因见 `wpe-drm/webkit-patches/README.md`。

## 后续方向

- 视频 overlay 二期：无 RGA 场景 CPU NV12 旋转兜底；plane 空闲时 in-fence 同步。
- runtime 瘦身：见 `tools/RUNTIME_SIZE_REPORT.md`。
