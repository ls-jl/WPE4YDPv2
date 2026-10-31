/* SPDX-License-Identifier: MIT */
/* ABI-name facade: --no-as-needed retains libmali.so.1 in DT_NEEDED.
 * EGL/GLES/GBM symbols resolve from that dependency, including dlsym(handle).
 * No vendor wrapper or hook code is used. */
static const char wrapper_notice[] __attribute__((used)) =
    "WPE4YDPv2 Mali ABI facade; MIT license";
