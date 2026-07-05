# WPE4YDPv2

词典笔 Direct WPE 浏览器壳。当前主线不是 MiniApp canvas/RGBFrame 刷图，而是：

```text
MiniApp 启动壳 -> frame 页全屏 <hole> -> JSAPI fork/exec 包内 WPE runtime -> WPE 直接 DRM overlay 出屏
```

MiniApp 只负责启动页、生命周期、系统键盘桥和全屏 `<hole>` 承载页；网页渲染、原生工具栏、触摸、滚动和 DRM 提交都由包内 WPE runtime 处理。

## 当前能力

- 手动启动浏览器，默认不自动抢占 DRM。
- 两种显示模式：
  - 原生模式：WPE 方向跟 MiniApp/系统配置方向一致。
  - 横屏旋转：在系统方向基础上旋转到横屏方向，用于横屏浏览体验。
- 包内 WPE runtime：安装后直接从 `assets/wpe-runtime/` 运行，不再依赖外部 `/userdisk/wpe-drm2`。
- 全屏 `<hole>`：MiniApp plane 挖洞露出下方 WPE DRM plane。
- 系统配置自适应：优先读取 `/etc/miniapp/resources/cfg.json`，并结合 DRM mode 和 MiniApp hole 尺寸解析 `panelSize/drmMode/viewport/rotation/touchRotation`。
- 系统键盘桥：WPE 写请求文件，MiniApp 拉起 HaasUI 键盘，再把确认结果写回 WPE。
- 内置字体、CA 证书、Mesa 软件渲染、GStreamer 运行库和 WPE/WebKit 运行资源。

## 目录结构

- `src/`：MiniApp 前端壳。
  - `src/pages/index/`：启动页，显示模式选择、键盘测试、启动入口。
  - `src/pages/frame/`：全屏 `<hole>` 承载页，负责启动/停止 WPE、watchdog、键盘桥轮询。
  - `src/pages/index0|90|180|270/`、`src/pages/frame0|90|180|270/`：框架旋转别名页，配合 `app.json` 的 `page_rotation_sku`。
  - `src/utils/keyboard.js`：HaasUI 系统键盘封装。
- `jsapi/`：MiniApp native JSAPI 源码，模块名 `browser`。
- `libs/`：打包用 JSAPI so。
  - `libs/arm64-orange/libjsapi_browser.so`：设备 ABI 目录下的实际 so（唯一入库源）。
  - `libs/libjsapi_browser_12345.so`：MiniApp 模块加载入口用命名，由 `scripts/sync_generated.sh` 在构建前从上面拷贝生成，不入库。
- `assets/wpe-runtime/`：包内直跑 WPE runtime。
  - `run.sh`：WPE 启动脚本，由 `scripts/sync_generated.sh` 从 `wpe-drm/run.sh`（唯一维护源）拷贝生成。
  - `wpe-drm-minimal`：Direct DRM WPE 启动器。
  - `lib/`、`libexec/`、`share/`：WPE/WebKit/Mesa/GStreamer 等运行依赖。
  - `etc/ssl/certs/ca-certificates.crt`：HTTPS 证书包。
  - `assets/fonts/`：内置字体。
- `wpe-drm/`：Direct DRM WPE 启动器和 DRM view 源码镜像。
- `tools/`：开发文档。
  - `tools/DRM_HOLE_RENDERING_PIPELINE.md`：DRM 出屏到 MiniApp `<hole>` 展示的完整链路文档。
  - `tools/KEYBOARD_INPUT.md`：系统键盘调用参考。
  - `tools/miniapp_docs.md`：MiniApp 文档整理。
- `debug/`：本地调试页面（含 benchmark/*-test.html 等测试页，不随 runtime 打包）和触摸校准辅助文件。
- `8001779591038449.1_0_0.amr`：当前打包产物（不入库）。

## 构建

```sh
npm run build
```

构建前会自动执行 `scripts/sync_generated.sh`，把单源文件（`wpe-drm/run.sh`、`libs/arm64-orange/libjsapi_browser.so`）同步到打包位置。

如果构建缓存中存在损坏的 runtime symlink，先清理缓存再构建：

```sh
rm -rf .falcon_ .falcon_tmp
npm run build
```

构建成功后会生成：

```text
8001779591038449.1_0_0.amr
```

常见构建警告：

- `theme-default` 缺失：当前项目不依赖该主题，通常可忽略。
- `browser/global` 模块无法静态解析：这是设备端 native JSAPI / HaasUI 模块，构建期警告通常可忽略。

## 运行方式

1. 安装 AMR 到设备。
2. 启动 MiniApp 后停留在 `index` 启动页。
3. 选择显示模式：
   - `原生模式`：跟随设备 MiniApp 框架方向。
   - `横屏旋转`：浏览器画面旋转到横屏方向。
4. 点击 `启动浏览器`。
5. 进入 `frame` 页后，MiniApp 显示全屏 `<hole>`，WPE 从包内 runtime 启动并通过 DRM overlay 出屏。

Home 或页面退出时，`frame` 会调用 JSAPI 停止 WPE，释放 DRM plane。重新进入 MiniApp 后仍回到启动页，不会自动启动浏览器。

## 关键运行路径

安装后 runtime 应来自 MiniApp 包目录：

```text
$workspace/assets/wpe-runtime
```

可写数据仍放在 `$dataDir/browser/`：

```text
$dataDir/browser/wpe-drm.log
$dataDir/browser/browser.pid
$dataDir/browser/browser-state.ini
$dataDir/browser/chrome-render-state.ini
$dataDir/browser/keyboard/
$dataDir/browser/runtime-tmp/
$dataDir/browser/fontconfig-cache/
$dataDir/browser/gst-registry.bin
```

不应该再依赖外部：

```text
/userdisk/wpe-drm2
/userdisk/mesa
/userdisk/chroot/rootfs/debian-12-arm64/opt/wpe-hostabi
```

## 设备调试

查看进程：

```sh
adb shell "ps | grep -E 'wpe-drm-minimal|WPEWebProcess|WPENetworkProcess|WPEGPUProcess'"
```

查看 WPE 日志：

```sh
adb shell "find /userdisk -name wpe-drm.log 2>/dev/null | head"
adb shell "tail -n 200 <日志路径>"
```

查看 DRM state：

```sh
adb shell "cat /sys/kernel/debug/dri/0/state"
```

清理残留 WPE 进程：

```sh
adb shell "killall wpe-drm-minimal WPEWebProcess WPENetworkProcess WPEGPUProcess 2>/dev/null || true"
```

保持屏幕亮起：

```sh
adb shell "hal-screen keep"
```

## Git / 大文件

本仓库使用 Git LFS 保存超大产物：

- `assets/wpe-runtime/lib/libWPEWebKit-2.0.so.1.10.2`

打包产物 `*.amr`、CMake 构建目录 `build-jsapi-host/` 与生成的 `libs/libjsapi_browser_12345.so` 均不入库。

首次克隆后需要确保本机可用 Git LFS：

```sh
git lfs install
git lfs pull
```

缓存和中间产物不应提交：

```text
.cache/
.falcon_/
.falcon_tmp/
.rollback/
```

## 详细文档

Direct DRM 出屏、buffer 路径、WPE plane 与 MiniApp `<hole>` 合成、触摸/键盘桥、常见故障排查，见：

```text
tools/DRM_HOLE_RENDERING_PIPELINE.md
```
