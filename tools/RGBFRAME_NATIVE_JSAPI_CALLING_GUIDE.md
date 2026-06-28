# RGBFrame Native JSAPI 调用文档

本文说明当前测试 miniapp 如何调用 native `rgbframe` JSAPI，把 native 线程生成或写入的帧通过 miniapp canvas 全屏显示。当前实现对应：

- JS 页面：`src/pages/frame/frame.vue`
- Native 模块：`jsapi/src/jsapi_rgbframe/`
- JSAPI 模块名：`rgbframe`
- 导出对象：`rgbFramePlayer`
- 已验证设备画布尺寸：`1210x568`

## 1. 基本机制

当前机制复用了原视频播放器的刷帧路径：

1. JS 页面创建全屏 `canvas`。
2. JS 通过 `getImageData(0, 0, width, height)` 拿到 `ImageData`。
3. JS 把 `ImageData` 的 `ArrayBuffer` 传给 native：`rgbFramePlayer.setImageDataBuffer(buffer)`。
4. native 线程直接写这个 buffer。
5. native 每写完一帧就触发 `onDrawFrame`。
6. JS 回调里只做一件事：`ctx.putImageData(imageData, 0, 0)`。

像素内容必须由 native 写入，JS 侧只负责获取 canvas buffer 和提交刷新。

## 2. JS 侧最小调用顺序

必须按这个顺序调用：

```js
import { rgbFramePlayer } from 'rgbframe'

const canvas = this.$refs.canvas
const ctx = typeof createCanvasContext === 'function'
  ? createCanvasContext(canvas)
  : canvas.getContext('2d')

const width = 1210
const height = 568
const imageData = ctx.getImageData(0, 0, width, height)
const buffer = imageData.buffer || imageData.data.buffer

rgbFramePlayer.setVideoInfo(width, height)
rgbFramePlayer.setImageDataBuffer(buffer)
rgbFramePlayer.setFrameInterval(33)

const drawHandler = () => {
  ctx.putImageData(imageData, 0, 0)
}
rgbFramePlayer.onDrawFrame.on(drawHandler)

rgbFramePlayer.start()
```

停止时要解除事件并停止 native 线程：

```js
rgbFramePlayer.stop()
rgbFramePlayer.onDrawFrame.off(drawHandler)
```

## 3. 全分辨率调用方式

不要硬编码 `480x960` 或 `960x266`。当前设备在横屏 `270` 旋转下实际 canvas 是 `1210x568`。

推荐先用 `$dom.getComponentRect()` 读取 canvas 实际布局尺寸：

```js
let rectResult = this.$page.$dom.getComponentRect(this.$refs.canvas)
if (rectResult && rectResult.then) {
  rectResult = await rectResult
}

const size = rectResult && (rectResult.size || rectResult)
const width = Math.round(Number(size.width))
const height = Math.round(Number(size.height))
```

如果拿不到 DOM 尺寸，再用 `$falcon.env.deviceWidth/deviceHeight` 兜底。旋转主题为 `90` 或 `270` 且 `deviceWidth < deviceHeight` 时，需要交换宽高：

```js
const width = Number($falcon.env.deviceWidth)
const height = Number($falcon.env.deviceHeight)
const theme = $falcon.env.custom && $falcon.env.custom.theme
const rotated = typeof theme === 'string' &&
  (theme.indexOf('90') >= 0 || theme.indexOf('270') >= 0)

const canvasSize = rotated && width < height
  ? { width: height, height: width }
  : { width, height }
```

当前项目已经在 `frame.vue` 内实现这套逻辑，并会在设备日志输出：

```text
rgbframe canvas size 1210x568
```

## 4. Native JSAPI 接口

### `setVideoInfo(width, height)`

设置后续写帧的逻辑尺寸。

- 参数：两个正整数
- 返回：`true`
- 错误：
  - 参数不是数字：`TypeError`
  - 宽高小于等于 0：`RangeError`

调用示例：

```js
rgbFramePlayer.setVideoInfo(1210, 568)
```

### `setImageDataBuffer(buffer)`

把 canvas `ImageData` 背后的 `ArrayBuffer` 交给 native 保存。

- 参数：`ArrayBuffer`
- 返回：buffer 字节数
- 要求：buffer 至少为 `width * height * 4` 字节

调用示例：

```js
const imageData = ctx.getImageData(0, 0, width, height)
const buffer = imageData.buffer || imageData.data.buffer
rgbFramePlayer.setImageDataBuffer(buffer)
```

注意：JS 必须保留 `imageData` 引用，不能让它被 GC 回收。当前页面通过 `this.imageData = ...` 保存。

### `setFrameInterval(ms)`

设置 native 线程的帧间隔。

- 参数：毫秒
- 实际范围：`5ms` 到 `1000ms`
- 返回：实际设置值

常用值：

```js
rgbFramePlayer.setFrameInterval(33) // 约 30 FPS
rgbFramePlayer.setFrameInterval(16) // 约 60 FPS，CPU 压力更高
```

### `start()`

启动 native 写帧线程。

调用前必须已经完成：

1. `setVideoInfo(width, height)`
2. `setImageDataBuffer(buffer)`
3. 确认 `buffer.byteLength >= width * height * 4`

如果条件不满足，会抛出：

```text
rgbframe is not ready
```

### `stop()`

停止 native 写帧线程。

页面隐藏或退出时必须调用，避免后台继续写 buffer。

### `isRunning()`

返回 native 线程是否正在运行。

### `onDrawFrame`

native 写完一帧后触发的事件信号。

```js
const drawHandler = () => {
  ctx.putImageData(imageData, 0, 0)
}

rgbFramePlayer.onDrawFrame.on(drawHandler)
rgbFramePlayer.onDrawFrame.off(drawHandler)
```

回调里不要做耗时计算，只提交 `putImageData`。帧内容应由 native 生成或从 native/chroot 侧拷贝。

## 5. 像素格式

当前传给 native 的不是 RGB565，也不是 YUV420，而是 miniapp canvas `ImageData` 的 4 字节像素 buffer。

实际写入顺序按原视频播放器行为实现：

```text
byte 0: B
byte 1: G
byte 2: R
byte 3: A，固定 0xff
```

每行 stride：

```text
stride = width * 4
```

总大小：

```text
bufferSize = width * height * 4
```

如果外部来源是 RGB888，需要写入时交换 R/B：

```cpp
px[0] = b;
px[1] = g;
px[2] = r;
px[3] = 0xff;
```

如果外部来源是 RGB565 或 YUV420，需要在 native 侧转换到上述 4 字节格式后再写 buffer。

## 6. 生命周期建议

页面加载：

1. 获取 canvas 尺寸。
2. 设置 `canvas :width` 和 `:height`。
3. 等待一次布局 tick。
4. `getImageData()`。
5. 调用 `setVideoInfo()`。
6. 调用 `setImageDataBuffer()`。
7. 注册 `onDrawFrame`。
8. `start()`。

页面显示：

```js
onShow() {
  if (this.initialized) {
    rgbFramePlayer.start()
  }
}
```

页面隐藏：

```js
onHide() {
  if (this.initialized) {
    rgbFramePlayer.stop()
  }
}
```

页面销毁：

```js
onUnload() {
  rgbFramePlayer.stop()
  rgbFramePlayer.onDrawFrame.off(this.drawHandler)
}
```

如果分辨率变化，必须先 `stop()`，重新 `getImageData()`，再重新传 `setVideoInfo()` 和 `setImageDataBuffer()`。

## 7. Native 模块注册

native 侧通过 `custom_init_jsapis()` 注册 QuickJS C module：

```cpp
extern "C" JQUICK_EXPORT void custom_init_jsapis()
{
    registerCModuleLoader("rgbframe", &rgbframe::rgbframe_module_load);
}
```

JS 侧用普通 ESM import：

```js
import { rgbFramePlayer } from 'rgbframe'
```

模块导出在 `JSRgbFrameModule.cpp` 中声明：

```cpp
static std::vector<std::string> exportList = {
    "rgbFramePlayer"
};
```

## 8. 打包要求

当前固件上，仅把 so 放到：

```text
libs/arm64-orange/libjsapi_rgbframe.so
```

不会被 `miniapp_cli install` 转换成可加载 native JSAPI。

必须额外放一份扁平路径：

```text
libs/libjsapi_rgbframe_12345.so
```

构建后 AMR 里应能看到：

```text
libs/libjsapi_rgbframe_12345.so
libs/arm64-orange/libjsapi_rgbframe.so
```

安装到设备后，框架会把扁平 so 改名为带 hash 的可加载文件，例如：

```text
/userdisk/secondary/miniapp/data/mini_app/pkg/8001779290443383/a/libs/libjsapi_rgbframe_12345_480183727.so
```

如果安装目录只有：

```text
libs/arm64-orange/libjsapi_rgbframe.so
```

启动时会报：

```text
ReferenceError: could not load module filename 'rgbframe'
```

## 9. 构建、安装、启动

构建：

```bash
npm run build:bin
```

检查 AMR 内容：

```bash
unzip -l 8001779290443383.1_0_0.amr | grep -E 'manifest|frame.js.bin|libjsapi_rgbframe|icon'
```

推送并安装：

```bash
adb push 8001779290443383.1_0_0.amr /userdisk/8001779290443383.1_0_0.amr
adb shell 'miniapp_cli install /userdisk/8001779290443383.1_0_0.amr'
```

启动 frame 页：

```bash
adb shell 'miniapp_cli start 8001779290443383 frame'
```

不要使用：

```bash
miniapp_cli start 8001779290443383 --frame
```

这个固件会把 `--frame` 当作页面文件名，尝试加载 `--frame.js`。

抓图验证：

```bash
adb shell 'miniapp_cli capture /tmp/rgbframe.png'
adb pull /tmp/rgbframe.png ./rgbframe.png
file rgbframe.png
```

连续抓两张看动画是否在跑：

```bash
adb shell 'miniapp_cli capture /tmp/a.png; sleep 1; miniapp_cli capture /tmp/b.png; md5sum /tmp/a.png /tmp/b.png'
```

MD5 不同代表 native 帧在变化。

## 10. 设备日志验证

确认 so 被动态加载：

```bash
adb shell 'tail -n 220 /userdata/applog/YD_PEN_APP.log | grep -Ei "rgbframe|dynamic_load_jsapi|ReferenceError|TypeError"'
```

正常应看到类似：

```text
dynamic_load_jsapi /userdisk/secondary/miniapp/data/mini_app/pkg/8001779290443383/a/libs/libjsapi_rgbframe_12345_480183727.so
rgbframe canvas size 1210x568
```

确认进程 maps：

```bash
adb shell 'pid=$(pidof miniapp); grep -i rgbframe /proc/$pid/maps'
```

正常应看到 `libjsapi_rgbframe_...so`。

## 11. 常见问题

### `ReferenceError: could not load module filename 'rgbframe'`

原因通常是 native so 没被框架加载。

检查：

```bash
adb shell 'find /userdisk/secondary/miniapp/data/mini_app/pkg/8001779290443383 -maxdepth 4 -type f | sort'
```

安装目录必须有类似：

```text
libs/libjsapi_rgbframe_12345_480183727.so
```

只有 `libs/arm64-orange/libjsapi_rgbframe.so` 不够。

### `rgbframe is not ready`

`start()` 前缺少必要初始化。

检查：

- 是否先调用了 `setVideoInfo(width, height)`
- 是否调用了 `setImageDataBuffer(buffer)`
- buffer 是否至少为 `width * height * 4`
- `width/height` 是否和 `getImageData()` 一致

### 画面只有一半或拉伸错位

通常是 canvas 属性尺寸和实际显示尺寸不一致。

必须同时满足：

- `<canvas :width="frameWidth" :height="frameHeight" />`
- CSS 为全屏：`width: 100vw; height: 100vh;`
- `getImageData(0, 0, frameWidth, frameHeight)`
- `setVideoInfo(frameWidth, frameHeight)`

当前设备应输出：

```text
rgbframe canvas size 1210x568
```

### 画面黑屏

可能是设备熄屏、页面没在前台、或者未启动 frame 页。

检查：

```bash
adb shell 'miniapp_cli start 8001779290443383 frame'
adb shell 'miniapp_cli capture /tmp/check.png'
```

### 连不上 `miniapp_cli`

如果 `miniapp_cli` 报 `Error connect`，一般是 miniapp 容器 IPC 状态异常。等待几秒后重试；如果仍失败，让系统的 `runDictPen` 重新拉起 miniapp：

```bash
adb shell 'pid=$(pidof miniapp); kill -9 $pid; sleep 5; pidof miniapp'
```

不要手动长期启动第二个 `/usr/bin/miniapp` 实例，否则会出现两个 miniapp 进程争用 IPC。

## 12. 作为 chroot 画面桥接时的约束

当前 JSAPI 的直接输入是 canvas `ImageData` 的 `ArrayBuffer`，chroot 容器不能直接调用这个 JSAPI。

推荐桥接方式：

1. miniapp 页面负责创建 canvas 和注册 native JSAPI。
2. native so 持有 canvas buffer。
3. chroot 侧把画面写到一个共享通道，例如 shared memory、unix socket、pipe 或 mmap 文件。
4. native so 的 worker 线程从共享通道读取最新帧。
5. native so 转换到 `B,G,R,0xff` 后写入 canvas buffer。
6. native so 触发 `onDrawFrame`，JS 调用 `putImageData`。

这样 JS 不参与像素生成，只承担最后一次 canvas 提交。
