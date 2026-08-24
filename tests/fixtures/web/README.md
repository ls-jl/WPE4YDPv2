# Web Fixtures

Small local pages for manual DRM, timing, touch, audio and video regression
tests. Serve this directory over HTTP or open individual files through WPE's
allowed `file:` navigation during device diagnostics.

`keyboard-custom-focus.html` models an SMS-code wrapper that focuses a hidden
numeric input. Use it to verify that trusted `focusin` opens the system keyboard
once while script-only autofocus remains blocked by the native activation gate.
