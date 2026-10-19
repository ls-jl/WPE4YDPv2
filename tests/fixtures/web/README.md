# Web Fixtures

Small local pages for manual DRM, timing, touch, audio and video regression
tests. Serve this directory over HTTP or open individual files through WPE's
allowed `file:` navigation during device diagnostics.

`keyboard-custom-focus.html` models an SMS-code wrapper that focuses a hidden
numeric input. Use it to verify that trusted `focusin` opens the system keyboard
once while script-only autofocus remains blocked by the native activation gate.

`webrtc-data-loopback.html` connects two local PeerConnections and echoes ten
messages in each direction. The expected title ends in `rx=10 reply=10` and
`ice=completed/completed`; it does not test remote audio/video reception.

`vpu-test.html` expects a sibling `test-video.mp4`. Supply a local H.264 fixture;
HTTP servers must implement byte ranges. No external runtime or paid game is
needed for this decoder/DRM video ACK test.
