# Runtime Size Report

当前包内 runtime 位于 `assets/wpe-runtime/`，总大小约 `401M`。本文件记录体积现状和后续瘦身方向，不在本轮直接删除核心运行库。

## Largest Libraries

```text
147M  lib/libWPEWebKit-2.0.so.1.10.2（2026-07-07 重编：-mcpu=cortex-a53 + ThinLTO + PGO（抖音/bilibili/百度真机采样 profile-use）+ 裁剪 SAMPLING_PROFILER/REMOTE_INSPECTOR/WEBDRIVER/JSSHELL/PDFJS/MATHML/GPU_PROCESS + Rockchip hole-punch 视频 KMS overlay 直出）
 30M  lib/libicudata.so.72.1
 10M  lib/libgio-2.0.so.0
5.4M  lib/libepoxy.so.0
4.8M  lib/libglib-2.0.so.0
4.5M  lib/libicui18n.so.72
3.1M  lib/libcrypto.so.1.1
2.6M  lib/libicuuc.so.72
2.2M  lib/libgnutls.so.30
2.0M  lib/libgobject-2.0.so.0
```

## Fonts

```text
 91M  assets/fonts/miniapp
2.8M  assets/fonts/dejavu
2.8M  assets/fonts/chroot
```

瘦身建议：保留 CJK 简体中文、Latin、基础符号和必要 fallback，移除未覆盖目标市场的脚本字体前先做百度、Bing、设置页和常见网页回归。

## GStreamer

```text
121 plugins
 15M lib/gstreamer-1.0
```

默认运行只使用包内插件。系统 `/usr/lib/gstreamer-1.0` 现在必须通过 `WPE_USE_SYSTEM_GST=1` 显式启用。后续白名单应以实际视频站点和本地测试页为准，优先保留 core/playback、typefind、decodebin、基础 audio/video parser、目标格式 demux/decoder。

以下 13 个插件与 `lib/libasound.so.2(.0.0)` **从设备 `/usr/lib` 拷入**（设备系统 GStreamer 构建，已验证在包内 GStreamer core 下可注册），提供 VPU 硬解与音频输出/解码，系统固件升级后需重新同步：

```text
libgstrockchipmpp.so（Rockchip VPU 硬解，依赖系统 /usr/lib/librockchip_mpp.so.1）
libgstalsa.so libgstautodetect.so（音频 sink；libasound 用设备版本保证
  /usr/lib/alsa-lib 模块与 /etc/asound.conf speexrate 链兼容）
libgstfaad.so libgstmpg123.so libgstopus.so libgstvorbis.so libgstflac.so（音频解码）
libgstisomp4.so libgstogg.so libgstwavparse.so libgstaudioparsers.so libgstid3demux.so（demux/parse）
```

## Do Not Remove Without Rebuild

- `libWPEWebKit-2.0.so*`
- `libexec/wpe-webkit-2.0/*Process`
- GLib/GIO/libsoup/TLS/ICU/fontconfig/freetype/harfbuzz
- Mesa/EGL/GBM/DRI 软件渲染路径
- `etc/ssl/certs/ca-certificates.crt`
