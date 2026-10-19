import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'

// Compile the actual post-patch functions with small API stubs. Pixel and
// driver correctness still require the native WebKit build and device workload.
function postPatchFile(patch, file) {
  const lines = fs.readFileSync(`wpe-drm/webkit-patches/${patch}`, 'utf8').split('\n')
  let selected = false
  let hunk = false
  const output = []
  for (const line of lines) {
    if (line.startsWith('diff --git ')) { selected = line.endsWith(` b/${file}`); hunk = false }
    else if (selected && line.startsWith('@@ ')) hunk = true
    else if (selected && hunk && (line[0] === ' ' || line[0] === '+')) output.push(line.slice(1))
  }
  assert.ok(output.length, `missing production source: ${file}`)
  return output.join('\n')
}

function method(source, signature) {
  const start = source.indexOf(signature)
  assert.notEqual(start, -1, signature)
  const open = source.indexOf('{', start)
  let depth = 1
  let end = open + 1
  while (depth && end < source.length) {
    if (source[end] === '{') depth++
    if (source[end] === '}') depth--
    end++
  }
  assert.equal(depth, 0)
  return source.slice(start, end)
}

function body(source, signature) {
  const code = method(source, signature)
  return code.slice(code.indexOf('{') + 1, -1)
}

const wheel = postPatchFile('0720-scroll-phases-and-bounded-prefetch.patch', 'Source/WebKit/Shared/WebWheelEventCoalescer.cpp')
const tile = postPatchFile('0740-skia-partial-tile-copy-on-write.patch', 'Source/WebCore/platform/graphics/skia/SkiaBackingStore.cpp')
const producer = postPatchFile('0740-skia-partial-tile-copy-on-write.patch', 'Source/WebCore/platform/graphics/texmap/coordinated/CoordinatedTileBuffer.cpp')
const touch = postPatchFile('0750-isolate-native-touch-gesture-completions.patch', 'Source/WebKit/UIProcess/API/wpe/WPEWebViewPlatform.cpp')
const shortDrag = postPatchFile('0760-end-short-native-drags.patch', 'Source/WebKit/UIProcess/API/wpe/WPEWebViewPlatform.cpp')
const phase = shortDrag.match(/auto phase =[^;]+;/)
assert.ok(phase)
const whole = tile.match(/if \(dirtyRect\.size\(\) == tileRect\.size\(\)\) \{([\s\S]*?)\} else/)
assert.ok(whole)
const retention = tile.match(/auto\* retainedTexture = m_texture\.get\(\);([\s\S]*?)retainedTexture\);/)
assert.ok(retention)
const copyOnWrite = tile.match(/if \(m_texture && m_texture->refCount\(\) > 2\) \{([\s\S]*?)\} else\s+ensureTexture\(tileRect\.size\(\), buffer\);/)
assert.ok(copyOnWrite)
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'wpe-gpu-tile-protocol-'))
try {
  const source = `
#include <cassert>
#include <utility>
#include <vector>
#include <fcntl.h>
#include "RenderingFenceRetirement.h"
#include "NativeTouchGestureState.h"
#include "NativeScrollGestureState.h"
enum { WPE_EVENT_TOUCH_DOWN, WPE_EVENT_TOUCH_MOVE, WPE_EVENT_TOUCH_UP };
struct WPEEvent { unsigned type, sequence, time; };
unsigned wpe_event_get_event_type(WPEEvent* event) { return event->type; }
unsigned wpe_event_touch_get_sequence_id(WPEEvent* event) { return event->sequence; }
unsigned wpe_event_get_time(WPEEvent* event) { return event->time; }
struct ViewPlatform {
    WPE::NativeTouchGestureState m_nativeTouchGesture;
    WPE::NativeScrollGestureState m_nativeScroll;
    bool consumesGestureCompletion(WPEEvent*) const;
};
${method(touch, 'bool ViewPlatform::consumesGestureCompletion(')}
#define PLATFORM(x) PLATFORM_##x
#define PLATFORM_COCOA 0
#define PLATFORM_GTK 0
#define USE(x) USE_##x
#define USE_LIBWPE 0
#define ENABLE(x) ENABLE_##x
#define ENABLE_WPE_PLATFORM 1
struct WebWheelEvent {
  enum class Phase { None, Began, Changed, Ended, Cancelled };
  Phase p = Phase::Changed, momentum = Phase::None;
  int position() const { return 1; }
  int globalPosition() const { return 1; }
  int modifiers() const { return 0; }
  int granularity() const { return 0; }
  Phase phase() const { return p; }
  Phase momentumPhase() const { return momentum; }
  bool hasPreciseScrollingDeltas() const { return true; }
};
struct WebWheelEventCoalescer {
  struct Queue { unsigned size() const { return 1; } } m_wheelEventQueue;
  static bool canCoalesce(const WebWheelEvent&, const WebWheelEvent&);
  bool shouldDispatchEventNow(const WebWheelEvent&) const;
};
struct GestureController { bool began; };
bool wpe_gesture_controller_is_drag_begin(GestureController* controller) { return controller->began; }
WebWheelEvent::Phase nativePhase(WPEEvent* event, GestureController* gestureController) {
  ${phase[0]}
  return phase;
}
constexpr unsigned wheelEventQueueSizeThreshold = 10;
${method(wheel, 'bool WebWheelEventCoalescer::canCoalesce(')}
${method(wheel, 'bool WebWheelEventCoalescer::shouldDispatchEventNow(')}
struct BitmapTexture {
  int references = 1;
  int identifier;
  int writes = 0;
  void ref() { ++references; }
  void deref() { --references; }
  int refCount() const { return references; }
  int id() const { return identifier; }
  void copyFromExternalTexture(int, int, int) { ++writes; }
};
struct Pointer {
  BitmapTexture* value;
  BitmapTexture* get() const { return value; }
  BitmapTexture* operator->() const { return value; }
  explicit operator bool() const { return value; }
  BitmapTexture* releaseNonNull() { auto* result=value; value=nullptr; return result; }
};
namespace WTF { template<class T> T&& move(T& value) { return std::move(value); } }
struct Image {
  void (*release)(void*); void* context; int identifier;
  ~Image() { release(context); }
};
namespace SkColorSpace { int MakeSRGB() { return 0; } }
namespace SkImages {
  Image* BorrowTextureFrom(int gpu, int identifier, int, int, int, int, void (*release)(void*), void* context) {
    if (gpu < 0) { release(context); return nullptr; }
    return new Image { release, context, identifier };
  }
}
struct RetainedCopies {
  std::vector<BitmapTexture*> values;
  void append(BitmapTexture* texture) { values.push_back(texture); }
  void complete() { for (auto* texture : values) texture->deref(); values.clear(); }
};
struct CoordinatedAcceleratedTileBuffer {
  static BitmapTexture* create(BitmapTexture* texture) { return texture; }
};
void replace(Pointer& m_texture, Pointer texture, Image*& m_cachedImage, RetainedCopies& pendingGPUReads) {
  auto retireTexture=[&](RetainedCopies& pendingGPUReads) {
${body(tile, 'void SkiaBackingStore::Tile::retireTexture(')}
  };
${whole[1]}
}
Image* borrow(Pointer m_texture, bool fail = false) {
  Image* m_cachedImage = nullptr;
  int grContext=fail ? -1 : 0, backendTexture=m_texture.get()->identifier, kTopLeft_GrSurfaceOrigin=0, colorType=0, kPremul_SkAlphaType=0;
  auto* retainedTexture = m_texture.get();
${retention[1]}retainedTexture);
  return m_cachedImage;
}
int IntRect(int, int size) { return size; }
struct TileRect { int size() const { return 256; } };
void partial(Pointer& m_texture, BitmapTexture& fresh, RetainedCopies& pendingGPUReads) {
  TileRect tileRect;
  int buffer=0;
  auto ensureTexture=[&](int, int) { if (!m_texture || m_texture->refCount() > 2) m_texture.value=&fresh; };
  if (m_texture && m_texture->refCount() > 2) {
${copyOnWrite[1]}
  } else
    ensureTexture(tileRect.size(), buffer);
  m_texture->copyFromExternalTexture(99, 16, 0);
}
${method(producer, 'class PaintedTextureRetirement')};
int main() {
  GestureController controller {true};
  WPEEvent move {WPE_EVENT_TOUCH_MOVE, 1, 100}, up {WPE_EVENT_TOUCH_UP, 1, 120};
  assert(nativePhase(&move, &controller) == WebWheelEvent::Phase::Began);
  assert(nativePhase(&up, &controller) == WebWheelEvent::Phase::Ended);
  controller.began = false;
  assert(nativePhase(&move, &controller) == WebWheelEvent::Phase::Changed);
  assert(nativePhase(&up, &controller) == WebWheelEvent::Phase::Ended);
  ViewPlatform view;
  view.m_nativeTouchGesture.begin(95, false);
  view.m_nativeTouchGesture.begin(339, false);
  WPEEvent oldDown { WPE_EVENT_TOUCH_DOWN, 95, 100 };
  WPEEvent currentDown { WPE_EVENT_TOUCH_DOWN, 339, 200 };
  WPEEvent currentUp { WPE_EVENT_TOUCH_UP, 339, 240 };
  assert(view.consumesGestureCompletion(&oldDown));
  assert(!view.consumesGestureCompletion(&currentDown));
  assert(!view.consumesGestureCompletion(&currentUp));
  view.m_nativeScroll.begin(339, 210, 220);
  view.m_nativeScroll.finish(339, 240);
  assert(view.consumesGestureCompletion(&currentUp));
  view.m_nativeTouchGesture.begin(340, false);
  assert(view.consumesGestureCompletion(&currentDown));
  WebWheelEvent a, b;
  assert(WebWheelEventCoalescer::canCoalesce(a,b));
  for (auto phase : {WebWheelEvent::Phase::Began, WebWheelEvent::Phase::Ended, WebWheelEvent::Phase::Cancelled}) {
    b.p=phase;
    assert(!WebWheelEventCoalescer::canCoalesce(a,b));
    assert(WebWheelEventCoalescer().shouldDispatchEventNow(b));
  }
  b.p=a.p; b.momentum=WebWheelEvent::Phase::Ended;
  assert(!WebWheelEventCoalescer::canCoalesce(a,b));
  assert(WebWheelEventCoalescer().shouldDispatchEventNow(b));
  BitmapTexture old {2, 10}, incoming {2, 20};
  Pointer texture { &old };
  auto* image = borrow(texture);
  assert(old.references == 3);
  Image* cached = image;
  RetainedCopies outgoing;
  replace(texture, Pointer { &incoming }, cached, outgoing);
  assert(texture.get() == &incoming && cached == nullptr);
  assert(old.identifier == image->identifier && incoming.identifier == 20);
  assert(old.references == 3 && outgoing.values.size() == 1);
  delete image;
  assert(old.references == 2); // Skia release alone does not authorize GPU pool reuse.
  outgoing.complete(); // The outgoing GL read fence has now completed.
  assert(old.references == 1);
  assert(borrow(texture, true) == nullptr);
  assert(incoming.references == 2); // Skia also calls release on factory failure.
  BitmapTexture borrowed {3, 30}, fresh {2, 40}, idle {2, 50};
  Pointer destination {&borrowed};
  RetainedCopies copies;
  partial(destination, fresh, copies);
  assert(borrowed.writes == 0 && fresh.writes == 2);
  assert(destination.get() == &fresh && copies.values.size() == 1 && copies.values[0] == &borrowed);
  destination.value=&idle;
  partial(destination, fresh, copies);
  assert(destination.get() == &idle && idle.writes == 1 && copies.values.size() == 1);
  copies.complete();
  BitmapTexture painted {1, 60};
  int descriptors[2];
  assert(pipe(descriptors) == 0);
  {
    PaintedTextureRetirement retirement;
    retirement.retain(descriptors[0], painted);
    assert(painted.references == 2); // Dropped prefetch still cannot return to the pool.
    assert(write(descriptors[1], "x", 1) == 1);
  }
  assert(painted.references == 1);
  assert(fcntl(descriptors[0], F_GETFD) == -1 && errno == EBADF);
  close(descriptors[1]);
}
`
  const input = path.join(directory, 'production.cpp')
  const binary = path.join(directory, 'production-test')
  fs.writeFileSync(input, source)
  const sanitizerFlags = process.env.WPE_TEST_SANITIZERS === '1'
    ? ['-O1', '-g', '-fno-pie', '-no-pie', '-fno-omit-frame-pointer', '-fsanitize=address,undefined'] : []
  const glib = execFileSync('pkg-config', ['--cflags', '--libs', 'glib-2.0'], { encoding: 'utf8' }).trim().split(/\s+/)
  execFileSync('c++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', ...sanitizerFlags, '-Iwpe-drm', input, ...glib, '-o', binary])
  execFileSync(binary)
  console.log('production GPU tile ownership and WPE scroll-phase tests passed')
} finally {
  fs.rmSync(directory, { recursive: true, force: true })
}
