# Android root standalone process example

This example shows the shape of running Dear ImGui from a native executable
started as root, without an Activity or `android_native_app_glue`.

The important difference from `example_android_opengl3` is window ownership:

- app example: Android framework gives you an `ANativeWindow`
- root process: you create a SurfaceFlinger layer yourself, then render into it

This uses `dlopen("libgui.so")` to call private SurfaceFlinger client ABI
without private headers. That makes the build NDK-only, but the runtime ABI is
not stable. You may need to adjust the symbol candidates in `main.cpp` for your
Android release.

## Build sketch

```sh
export NDK_HOME=/path/to/android-ndk
./examples/example_android_root_opengl3/build_android.sh

adb push /tmp/imgui-root-build/imgui_root_android /data/local/tmp/
adb shell su -c 'chmod 755 /data/local/tmp/imgui_root_android'
adb shell su -c '/data/local/tmp/imgui_root_android --width 1272 --height 2772'
```

Use the real display size from `adb shell wm size`. If the layer size does not
match the display, raw touch coordinates from `/dev/input` will not line up with
the rendered ImGui surface.

On many devices the exact private API signatures differ across Android
releases. The rendering, EGL, and ImGui parts are stable; the `LibGuiAbi` block
is the part you normally adjust per device/API level.

Useful device-side symbol check:

```sh
adb shell su -c 'nm -D /system/lib64/libgui.so | c++filt | grep -E "SurfaceComposerClient::(getDefault|createSurface)|SurfaceControl::getSurface|Transaction::(setLayer|setPosition|show|apply)"'
```

## Touch pass-through

The layer is created without an Android input channel and ImGui reads raw events
from `/dev/input`. On Android versions with overlay touch-obscuring protection,
a full-screen untrusted top layer can still make the app underneath unable to
receive touches. This example therefore marks the layer as a trusted overlay via
`Transaction::setTrustedOverlay(..., true)` when that symbol is available.

That makes blank areas pass through to the underlying app. It does not provide
true per-ImGui-widget interception: raw `/dev/input` reading is only a copy of
the hardware stream, so the lower app still receives the same touch. For strict
"ImGui consumes only hovered windows, everything else passes through", use one
of these heavier routes:

- create/update a hidden `InputWindowInfo` with a touchable region matching the
  ImGui windows, which requires more private ABI probing
- `EVIOCGRAB` the real touch device, consume touches that begin inside ImGui,
  and replay non-ImGui touches through `/dev/uinput`

Pass `--untrusted-overlay` only when debugging the platform behavior.
