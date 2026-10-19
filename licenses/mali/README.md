# Mali G52 Userspace Driver

The selected AArch64 binary is `libmali-bifrost-g52-g13p0-dummy-gbm.so`
from Rockchip's libmali mirror maintained by Jeffy Chen. `source.json` pins
the revision, path and SHA-256; no firmware-extracted userspace binary is
used by the GPU staging script.

- Upstream: https://github.com/JeffyCN/mirrors/tree/34f626298b0e8f884545365b66bb47678266e8d7
- License: `END_USER_LICENCE_AGREEMENT.txt`, ARM LES-PRE-20769.
- Attribution and upstream file scope: `upstream-copyright`.
- Buildroot also uses this mirror and classifies the driver as proprietary:
  https://github.com/buildroot/buildroot/blob/master/package/rockchip-mali/rockchip-mali.mk

This is **not open-source software and not MIT**. EULA sections 1.1(ii)
and 1.2 expressly permit redistribution under their conditions: retain all
notices, include the EULA, and do not use ARM/licensor branding to market
applications. Use is restricted to applications for licensed Mali-based
products. Other restrictions, including export controls, remain applicable;
read the complete EULA before use or distribution.

The binary is copied unchanged as `gpu/mali/lib/libmali.so.1`. Small
project-authored MIT ABI facades provide `libEGL.so.1`, `libGLESv2.so.2`
and `libgbm.so.1` through a DT_NEEDED dependency, without modifying the
vendor ELF or duplicating its 43MB payload. The project GBM compatibility
shim is also MIT. No firmware `libmali_hook` or vendor wrapper is retained.
Staging includes this directory and the wrappers' MIT license under
`share/licenses/mali/` in the runtime.

## Kernel Modules And Release Boundary

The EULA **does not license** `bifrost_kbase.ko`, `x7_gpu_dt_enable.ko`,
old firmware wrappers, or the rest of the bundled runtime. Existing KO
inputs remain local-only pending a separate source/license compliance
review; an ELF license marker alone does not satisfy source obligations.
The default public build remains CPU-only, and the public archive gate
still rejects GPU/KO packages. A locally built GPU AMR must not be presented
as cleared for public distribution merely because its userspace driver has
an explicit redistribution grant.

## Reproduce

```sh
bash scripts/build_gpu_helpers.sh
bash scripts/stage_gpu_runtime.sh
```

Download and license checksums must match before staging. Driver suitability
is separately tested with `wpe-gpu-probe`; a license is not a guarantee of
compatibility with a particular kbase kernel or display pipeline.

## Validation Scope (2026-10-06)

On the connected RK3562 ARM64 pen with kernel 5.10.160, the unchanged
g13p0 binary passed GLES shader/readback, linear DMA-BUF export/import,
CPU mapping and DRM framebuffer creation with the project ABI facades.
A separate, temporary WPE profile ran the local RAF fixture for 20 seconds
at approximately 30fps with GPU composition and RGA rotation (960x266,
270 degrees). Supervisor cleanup removed its children and owned modules.
No installed application, existing Profile, or system library was replaced
by that test. This does not establish compatibility with every firmware,
long-running games, WebRTC, or every screen orientation.
