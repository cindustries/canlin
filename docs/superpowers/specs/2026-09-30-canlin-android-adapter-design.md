# CanLin — Android as a driver layer (NativeActivity adapter)

Status: draft for review (2026-09-30). Decided with the owner: NativeActivity instead of
SDL2 on Android; no root in v1. It amends the Android row of the
[system design](2026-09-22-canlin-system-design.md) §2 and adds an Android wait to the
[native loop](2026-09-24-canlin-native-loop-design.md) §4. It follows the platform
contract of the [event runtime](2026-09-22-canlin-event-runtime-design.md) §3 and the
conventions of the [core API](2026-09-23-canlin-core-api-design.md).

## 1. Idea

On an Android device CanLin is the whole user experience and Android is the driver
stack underneath it: kernel, Wi-Fi, charging, power management and later audio stay
Android's; screen, input, settings surface and every module belong to CanLin. No module,
view or model ever sees an Android concept. Modules query capabilities through
`canlin.sys` exactly as on every other host.

CanLin already renders in software into an RGB buffer. Android's `ANativeWindow_lock`
hands out a CPU-writable window buffer, so the adapter has the same shape as rawfb:
copy the dirty rows, post the buffer. No OpenGL, no SDL2, no Java code.

The platform services the adapter implements (§6) are a C interface that canlin-os
implements too. If a device later runs without Android (vendor kernel + own initramfs),
only the driver changes; CanLin and the modules do not.

## 2. First device and floor

Lenovo A10-70 (A7600-H): MediaTek MT8382, 4× Cortex-A7 1.3 GHz (ARMv7-A, NEON),
Mali-400 MP2, 1 GB RAM, 16 GB flash, 1280×800 IPS 10.1″, Wi-Fi + 3G, touch only.
Shipped with Android 4.2, officially updated to 4.4.

| Item | Value | Why |
|---|---|---|
| `minSdkVersion` | 19 (Android 4.4) | the device is updated to 4.4 first; API 17 would need an older NDK for no gain |
| NDK | r25c | last NDK that targets API 19; r26 requires 21 |
| ABI | `armeabi-v7a`, NDK default flags (Thumb-2, NEON available) | portable code still must not *require* SIMD |
| Java/Kotlin code | none (`android:hasCode="false"`) | framework calls go through JNI from C (§6.2) |
| Build | Makefile + NDK clang + `aapt2`/`zipalign`/`apksigner`; no Gradle | small, same build entry point as every other target |

The device does not lower the CPU floor (the P2 stays the floor). It is the first ARM
target and the first touch-only device, so it adds its own failure classes: `char` is
unsigned on ARM EABI; unaligned `LDRD`/`LDM`/VFP accesses fault; bionic is neither glibc
nor musl and the NDK hides APIs newer than API 19 at compile time.

Newer Android versions refuse to install apps with a very old `targetSdkVersion`.
`targetSdkVersion` for devices other than the first one is an open point (§10); raising
it changes permission behaviour and must be reviewed, not bumped silently.

## 3. Process, threads and glue

The app is one `NativeActivity` using `android_native_app_glue` from the NDK (compiled
from the NDK tree like libc, not vendored). The glue runs `android_main` on its own
thread; that thread is CanLin's **owner thread** and runs `canlin_loop_run`. The
Android UI thread only forwards activity callbacks through the glue's command pipe.

Android wait (new row for native loop §4): the owner thread's `ALooper` is the one
blocking primitive. The glue's command and input idents are registered on it. The loop's
descriptors (sources' `pollfds` + wake pipe) go into one private epoll set owned by the
adapter, and only that epoll descriptor is registered with the Looper, once. Changes to
the set between iterations are applied to the private epoll set. (Implemented in k72:
sources such as `canlin_http` close their sockets on their own, and a per-descriptor
`ALooper_addFd`/`removeFd` pairing on KitKat risks stale Looper records on fd reuse.) The
wait is `ALooper_pollOnce(timeout)` with the timeout computed by loop step 6. Idle stays
idle: no descriptor ready, no deadline, no command → infinite wait. Commands and input
are drained within the loop's bounded steps, never inside a Lua callback.

## 4. Lifecycle

| Glue command | CanLin reaction |
|---|---|
| `APP_CMD_INIT_WINDOW` | the output appears: size, format (§5), DPI; full invalidate |
| `APP_CMD_TERM_WINDOW` | the output is gone; nothing is presented until a new window arrives |
| `APP_CMD_WINDOW_RESIZED`, `APP_CMD_CONFIG_CHANGED` | re-read metrics; relayout and invalidate the output (UI §2.1) |
| `APP_CMD_GAINED_FOCUS` | re-apply immersive mode (§7) |
| `APP_CMD_PAUSE` / `APP_CMD_STOP` | suspension as event runtime §7: stop capture, keep durable work, no presenting |
| `APP_CMD_RESUME` | recheck capabilities, reconnect as needed, redraw; each due timer fires once (core API D4) |
| `APP_CMD_SAVE_STATE` | nothing: correctness never depends on a final flush |
| `APP_CMD_LOW_MEMORY` | log; release unpinned font sizes and other caches |
| `APP_CMD_DESTROY` | set quit; the loop returns. The process may also be killed without any command |
| `APP_CMD_WINDOW_REDRAW_NEEDED` | full present |

`PAUSE`, `STOP` and `TERM_WINDOW` also release a held touch. On the A7600-H, resuming
after power-on delivers a `TERM_WINDOW` + `INIT_WINDOW` pair; the second window gets a
full present and the module state is kept (verified in k72).

The clock is `CLOCK_MONOTONIC`. It stops while the device sleeps; per core API D4 this
adapter documents it: a timer due during deep sleep fires once after wake-up, late by
the sleep duration. The adapter test states that delay.

Orientation is fixed to landscape in the manifest for v1. Rotation is a later decision
together with the rotation sensor (§10).

## 5. Output and presenting

- **One output**, identity `android:main` (UI §2.1: mobile stays one display).
- **Format:** `ANativeWindow_setBuffersGeometry(win, 0, 0, WINDOW_FORMAT_RGBX_8888)`.
  If the window reports another format after locking (e.g. `RGB_565`), the present
  converts to it. Phase 0 (§9) records what this device actually delivers.
- **Present:** after `canlin_render_end()` reports dirty rows, lock with the dirty row
  range as bounds (full width). The platform may enlarge the bounds (first frame, other
  back buffer); every row inside the **returned** bounds is written from CanLin's full
  RGB frame, converting to the window format. Rows outside are the platform's. Then
  `ANativeWindow_unlockAndPost`. A failed lock drops that present, keeps the frame dirty
  and logs once per failure streak; it never blocks the loop.
- **DPI factor** = `AConfiguration_getDensity()` / 160 (Android's own baseline, so touch
  targets match other Android apps). Physical size differs per device; the trusted
  per-output UI scale (UI §2.1) corrects it. Phase 0 decides whether this device's
  default scale is 100 % or higher.
- Memory: 1280×800 RGB frame + shadow ≈ 6 MB; the window buffers are Android's.
- Verified on the A7600-H (k72): the lock reports `RGBX_8888` (format 2), stride 1280.
  With the navigation bar still shown (until §7) the window is 1280×752.

## 6. Input and platform services

### 6.1 Input

- **Touch:** `AMotionEvent` of the first pointer (pointer id of `ACTION_DOWN`) maps to
  the normalised primary button press/motion/release in output pixels through
  `canlin_module_post_input`, stamped at post (core API §6, canlin#37 stays open for
  hardware timestamps). Further pointers are ignored in v1; `ACTION_CANCEL` releases the
  held button at its last position. **Open decision:** a real input overflow clears a
  pressed on-screen-keyboard key without typing it, while a release at the last position
  over such a key types it; k72 implements the release. Whether cancel should behave like
  overflow instead is undecided. Scroll comes from the UI's existing touch
  drag handling, not from Android gestures.
- **Keys:** Back maps to Escape and is always consumed (CanLin is the home screen; the
  activity never finishes on Back). Menu (this device's navigation bar has a Menu
  button) opens/closes the shell's switcher. Volume keys are returned unhandled so Android's
  volume control keeps working. A hardware or Bluetooth keyboard delivers keys and text
  like any other key source.
- **Text input:** the output's input class is touch; a focused field shows CanLin's own
  on-screen keyboard (canlin#45). The Android IME is never requested.
- The decoding of Android events into `(action, pointer, x, y, key)` is a thin
  device-only layer; the mapping from those values to normalised input is a pure
  function tested on the host (§8).

### 6.2 Platform services (the "driver" interface)

A new C interface, `include/canlin/platform.h`, for services that belong to the device,
not to a module. Android implements it by JNI calls from the owner thread (attached once
with `AttachCurrentThread`) and NDK APIs; canlin-os implements it with sysfs,
wpa_supplicant and ALSA. v1 set:

| Service | Android implementation | Who may use it |
|---|---|---|
| `power`: battery level, charging | sticky `ACTION_BATTERY_CHANGED` via `registerReceiver(null, …)` | shell status line; `canlin.sys` exposes level/charging read-only |
| `screen`: keep awake | `ANativeActivity_setWindowFlags(AWINDOW_FLAG_KEEP_SCREEN_ON)` | shell; later a module capability with its own review |
| `screen`: brightness | window attribute `screenBrightness` via JNI (no system permission) | trusted settings only |
| `net`: online, transport, Wi-Fi signal | `ConnectivityManager`, `WifiManager` | shell status; `canlin.sys` exposes online/offline only |
| `wifi`: scan, list, connect, forget | `WifiManager.startScan`/`getScanResults`/`addNetwork`/`enableNetwork` | **trusted settings only** |
| `system`: open Android settings | `startActivity(Settings.ACTION_SETTINGS)` | trusted settings, behind a confirmation (admin escape hatch, §7) |

Without Java code there is no `BroadcastReceiver`. Asynchronous Android state (scan
results, connectivity changes) is read by bounded polling on a core timer: every few
seconds while the settings view that needs it is open, and on resume. If a receiver ever
becomes unavoidable, adding a Java class is a change to this spec, not an implementation
detail.

JNI calls are native work on the owner thread and follow event runtime §2: each call is
short and bounded; a call that can block (none in the v1 set is expected to) moves to a
worker with a bounded completion. JNI local references are released per call.

**Hostile-module review (house rule):** modules get no SSIDs, no scan lists, no network
names and no settings access — a scan list is a location fingerprint. What reaches Lua
is `canlin.sys` status (battery level/charging, online/offline), read-only. Wi-Fi
credentials entered in the settings go straight to Android and are not stored by CanLin.

Audio (OpenSL ES, available since API 9) is named here but specified with the audio
spec (system design §8); it is not part of v1 of this adapter.

## 7. Appliance mode without root

Manifest as built in k72: `NativeActivity`, `hasCode="false"`, `launchMode="singleTask"`,
`configChanges` covering orientation/screen size/keyboard, `windowSoftInputMode=
"stateAlwaysHidden"`, theme `Theme.NoTitleBar.Fullscreen`, landscape, `minSdkVersion` and
`targetSdkVersion` 19, `allowBackup="false"`, permissions `INTERNET` and
`ACCESS_NETWORK_STATE`. The package name `industries.conflict.canlin` is a placeholder
awaiting the owner's decision: changing it later is a new app identity and loses the
app's local data. APK signing: v1 (JAR) + v2 + v3, debug keystore generated into `build/`.

- The manifest declares the activity for `MAIN` + `LAUNCHER` and `MAIN` + `HOME` +
  `DEFAULT`. The operator selects CanLin as the default home app once ("always"). After
  that the device boots straight into CanLin, and Home returns to it.
- Immersive sticky mode (API 19) hides status and navigation bars; it is re-applied on
  focus gain. An edge swipe still shows the bars transiently. Without root this is not a
  lock: a determined user can reach Android. v1 accepts this and says so.
- Operator setup (documented, manual): update to 4.4, set the screen lock to "none",
  choose CanLin as home, connect Wi-Fi (in CanLin or Android). Whether the stock
  launcher can be disabled from `adb shell` without root is checked in phase 0.
- Escape hatches for the operator: the trusted "Android settings" action (§6.2) and adb.
- Root-based hardening (disabling packages, blocking the notification shade) is a later
  phase with its own decision; nothing in v1 depends on it.

## 8. Storage, credentials and TLS

- The data volume is `ANativeActivity->internalDataPath` (app-private, ext4 on `/data`).
  The persistence barrier is the native one (file + directory `fsync`, system design §6).
- `android:allowBackup="false"`, so `adb backup` cannot extract local data or secrets.
  Secrets follow the accounts spec §5; app-private storage is the device-local store,
  the Android Keystore is not used in v1.
- **TLS:** CanLin's own mbedtls, independent of Android's outdated TLS stack. The CA
  bundle ships with the app; Android's system CA store (frozen at 4.4, missing current
  roots) is never used. Same bundle source as the canlin-os images (canlin-os#4).
- Permissions: `INTERNET`, `ACCESS_NETWORK_STATE`, `ACCESS_WIFI_STATE`,
  `CHANGE_WIFI_STATE`, `WAKE_LOCK` if keep-awake needs it. Nothing else in v1.

## 9. Phases and evidence

**Phase 0 — inventory (stock Android 4.4, adb, no root).** One karr card, like canlin#3
for the P2. Record: `getprop` (SDK, model, platform, `ro.sf.lcd_density`), `wm size` /
`wm density`, `/proc/cpuinfo` features, free RAM with Android idle, `/data` free space,
`getevent -p` (touch device, ranges, multi-touch protocol), `dumpsys SurfaceFlinger`
(window formats), `dumpsys wifi` / `dumpsys battery`, readable `/sys/class/power_supply`
and backlight nodes, whether current roots (e.g. ISRG Root X1) are missing from
`/system/etc/security/cacerts`, whether `pm disable-user` works from `adb shell`.

**Phase 1 — adapter.** APK build (§2), glue + Android wait (§3), lifecycle (§4),
present (§5), input (§6.1), own CA bundle. Needs the runtime pieces the desktop needs
anyway: native loop wiring (canlin#61), the assistant module (canlin#40) and a minimal
profile/key store. Done when the shell with the assistant runs on the tablet against a
real provider.

**Phase 2 — platform services** (§6.2) and the trusted settings surface for Wi-Fi and
brightness.

**Phase 3 — appliance** (§7): home app, immersive mode, operator setup document.

Evidence, reported separately (house rule 8):

| Gate | What it proves |
|---|---|
| host `make test` | portable logic, including the pure input mapping, lifecycle state machine and RGB→window-format row conversion with enlarged bounds |
| armv7 portable suite on the device (`adb push` to `/data/local/tmp`, run, collect exit codes) | the portable suites under bionic on real ARM — the first ARM gate |
| armv7 portable suite under `qemu-arm` without a device | optional, to be proven in phase 1 (static NDK executables); ARM semantics in CI, not bionic behaviour on hardware |
| manual device qualification | first frame, touch, Back/Home, power button pause/resume, reboot into CanLin, Wi-Fi loss/return, TLS to a real provider with the own CA bundle, idle without wakeups, memory after an hour |

None of these replaces the P2-QEMU gate for i686, and host tests prove nothing about
the device.

### Phase 0 result (2026-09-30, adb, stock firmware, no changes made)

| Item | Found |
|---|---|
| Firmware | Android 4.4.2 (API 19), `A7600H_A442_000_029_150324_ROW`, user build, release keys; `ro.secure=1`, `ro.debuggable=0` (no `adb root`) — the 4.4 update is already installed |
| Kernel | Linux 3.4.67 SMP PREEMPT (MediaTek ALPS.KK1.MP1, `ro.mediatek.platform=MT6582`) |
| CPU | ARMv7 Cortex-A7 r0p3, features `neon vfpv4 idiva idivt tls`; 598–1300 MHz, cores hot-plugged (1–2 online when idle) |
| RAM | 976 MB total; with Android idle ≈ 600 MB free incl. caches, ≈ 338 MB used; 512 MB zram swap |
| Storage | `/data` 12.8 GB, 12.3 GB free (ext4) |
| Display | 1280×800, 58.2 Hz, density 160 (`xdpi = ydpi = 160` reported) → DPI factor 1.0 by §5. Physical ≈ 149 dpi; whether 125 % is the better default is a device-test question |
| Window formats | app and framebuffer surfaces use `RGBA_8888` (format 1), wallpaper `RGBX_8888` (2); Mali-400 MP, GLES 2.0, HW composer on |
| Touch | `mtk-tpd` (`/dev/input/event4`), single-touch ABS plus multi-touch protocol A (no `ABS_MT_SLOT`), 0–1280 × 0–800, direct |
| Keys | `mtk-kpd` (power, volume, …), `ACCDET` (headset), `em6781` |
| Battery | `/sys/class/power_supply/battery/{capacity,status}` readable by `shell`; `dumpsys battery` works |
| Backlight | `/sys/class/leds/lcd-backlight/brightness` (0–255), owned `system:radio`, not writable by the app → brightness via window attribute (§6.2) as planned |
| Wi-Fi | enabled through the Android framework; MediaTek combo chip brought up by `wmt_loader` / `6620_launcher` services before `wpa_supplicant` (relevant for §10.6) |
| CA store | 156 system roots; **ISRG Root X1 (`6187b673.0`) is missing** — confirms the own CA bundle of §8 |
| NDK libraries | `libandroid.so`, `libjnigraphics.so`, `libOpenSLES.so` present |
| Appliance | stock home app `com.lenovo.launcherhd`; `pm disable-user` and `pm block` exist in `pm`, whether `shell` may run them on another package is not tested yet (it changes the device) |
| Shell groups | `shell` is in `graphics` and `input` — irrelevant for the APK, noted for experiments |

## 10. Open points

1. `targetSdkVersion` for devices beyond the first one, and the permission changes it
   brings (runtime permissions, location permission for Wi-Fi scans on newer Android).
2. Rotation and the rotation sensor.
3. Multi-touch beyond the primary pointer (pinch in the graph editor).
4. Whether SDL2 is still needed anywhere besides desktop and iOS once this adapter exists.
5. Root-based hardening as its own phase.
6. Running the same device without Android (vendor kernel + canlin-os initramfs): kept
   possible by §6.2, decided only after Wi-Fi bring-up without Android userspace is shown
   to be feasible.
