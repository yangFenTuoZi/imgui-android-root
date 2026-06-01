// dear imgui: standalone Android root process + OpenGL ES 3 example
//
// This is intentionally not an Activity example. It creates a SurfaceFlinger
// layer from a native executable, builds an EGL window surface on top of that
// layer, and runs Dear ImGui in a normal process main().
//
// Requirements:
// - root or shell context with permission to create SurfaceFlinger layers
// - libgui.so symbols matching one of the ABI shims below
// - per-Android-version adjustment may be required around LibGuiAbi

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <android/log.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <cstddef>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <vector>

#define LOG_TAG "ImGuiRoot"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static constexpr int32_t kPixelFormatRgba8888 = 1; // PIXEL_FORMAT_RGBA_8888

static volatile sig_atomic_t g_Running = 1;

static constexpr uint32_t kAndroidNativeWindowMagic = 0x5f776e64; // "_wnd"

static void OnSignal(int)
{
    g_Running = 0;
}

static ANativeWindow* FindNativeWindowBase(void* surface)
{
    auto* bytes = static_cast<unsigned char*>(surface);
    for (size_t offset = 0; offset <= 128; offset += alignof(void*))
    {
        uint32_t magic = 0;
        memcpy(&magic, bytes + offset, sizeof(magic));
        if (magic == kAndroidNativeWindowMagic)
        {
            auto* window = reinterpret_cast<ANativeWindow*>(bytes + offset);
            LOGI("ANativeWindow base found at Surface + %zu", offset);
            return window;
        }
    }

    LOGE("ANativeWindow magic not found near Surface object %p", surface);
    return nullptr;
}

struct Options
{
    int width = 1080;
    int height = 2400;
    int layer = 0x7fffffff;
    float dpi_scale = 2.5f;
    bool trusted_overlay = true;
    bool touch_swap_xy = false;
    bool touch_invert_x = false;
    bool touch_invert_y = false;
};

static Options ParseOptions(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--help") == 0)
        {
            printf("usage: %s [--width px] [--height px] [--layer z] [--scale factor] [--untrusted-overlay] [--touch-swap-xy] [--touch-invert-x] [--touch-invert-y]\n", argv[0]);
            exit(0);
        }
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)
            options.width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc)
            options.height = atoi(argv[++i]);
        else if (strcmp(argv[i], "--layer") == 0 && i + 1 < argc)
            options.layer = atoi(argv[++i]);
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
            options.dpi_scale = strtof(argv[++i], nullptr);
        else if (strcmp(argv[i], "--untrusted-overlay") == 0)
            options.trusted_overlay = false;
        else if (strcmp(argv[i], "--touch-swap-xy") == 0)
            options.touch_swap_xy = true;
        else if (strcmp(argv[i], "--touch-invert-x") == 0)
            options.touch_invert_x = true;
        else if (strcmp(argv[i], "--touch-invert-y") == 0)
            options.touch_invert_y = true;
    }
    return options;
}

struct SpOpaque
{
    void* ptr = nullptr;

    // android::sp<T> is a non-trivial C++ type. Keeping this shim non-trivial
    // makes AArch64 return-by-value calls use the same indirect return ABI.
    ~SpOpaque() {}
};

class DynamicLibrary
{
public:
    bool Open(const char* name)
    {
        m_handle = dlopen(name, RTLD_NOW);
        if (m_handle == nullptr)
        {
            LOGE("dlopen(%s) failed: %s", name, dlerror());
            return false;
        }
        return true;
    }

    void Close()
    {
        if (m_handle != nullptr)
        {
            dlclose(m_handle);
            m_handle = nullptr;
        }
    }

    template <typename T>
    T Symbol(const char* name) const
    {
        return reinterpret_cast<T>(dlsym(m_handle, name));
    }

private:
    void* m_handle = nullptr;
};

class LibGuiAbi
{
public:
    bool Load()
    {
        if (!m_libgui.Open("libgui.so"))
            return false;
        if (!m_libutils.Open("libutils.so"))
            return false;

        String8_ctor = m_libutils.Symbol<String8Ctor>("_ZN7android7String8C1EPKc");
        String8_dtor = m_libutils.Symbol<String8Dtor>("_ZN7android7String8D1Ev");
        SurfaceComposerClient_ctor = m_libgui.Symbol<SccCtor>("_ZN7android21SurfaceComposerClientC1Ev");
        SurfaceComposerClient_dtor = m_libgui.Symbol<SccDtor>("_ZN7android21SurfaceComposerClientD1Ev");
        SurfaceComposerClient_onFirstRef = m_libgui.Symbol<SccOnFirstRef>("_ZN7android21SurfaceComposerClient10onFirstRefEv");
        SurfaceComposerClient_initCheck = m_libgui.Symbol<SccInitCheck>("_ZNK7android21SurfaceComposerClient9initCheckEv");

        static const char* const surface_control_get_surface_symbols[] = {
            "_ZN7android14SurfaceControl10getSurfaceEv",
            "_ZNK7android14SurfaceControl10getSurfaceEv",
        };
        SurfaceControl_getSurface = Find<SurfaceControlGetSurface>(surface_control_get_surface_symbols);
        SurfaceControl_createSurface = m_libgui.Symbol<SurfaceControlGetSurface>("_ZN7android14SurfaceControl13createSurfaceEv");
        LayerMetadata_ctor = m_libgui.Symbol<LayerMetadataCtor>("_ZN7android3gui13LayerMetadataC1Ev");

        Transaction_ctor = m_libgui.Symbol<TransactionCtor>("_ZN7android21SurfaceComposerClient11TransactionC1Ev");
        Transaction_dtor = m_libgui.Symbol<TransactionDtor>("_ZN7android21SurfaceComposerClient11TransactionD1Ev");
        Transaction_setLayer = m_libgui.Symbol<TransactionSetLayer>("_ZN7android21SurfaceComposerClient11Transaction8setLayerERKNS_2spINS_14SurfaceControlEEEi");
        Transaction_setPosition = m_libgui.Symbol<TransactionSetPosition>("_ZN7android21SurfaceComposerClient11Transaction11setPositionERKNS_2spINS_14SurfaceControlEEEff");
        Transaction_show = m_libgui.Symbol<TransactionShow>("_ZN7android21SurfaceComposerClient11Transaction4showERKNS_2spINS_14SurfaceControlEEE");
        Transaction_hide = m_libgui.Symbol<TransactionShow>("_ZN7android21SurfaceComposerClient11Transaction4hideERKNS_2spINS_14SurfaceControlEEE");
        Transaction_setTrustedOverlay = m_libgui.Symbol<TransactionSetTrustedOverlay>("_ZN7android21SurfaceComposerClient11Transaction17setTrustedOverlayERKNS_2spINS_14SurfaceControlEEEb");
        Transaction_apply_bool = m_libgui.Symbol<TransactionApplyBool>("_ZN7android21SurfaceComposerClient11Transaction5applyEb");
        Transaction_apply_bool_bool = m_libgui.Symbol<TransactionApplyBoolBool>("_ZN7android21SurfaceComposerClient11Transaction5applyEbb");
        Transaction_apply_void = m_libgui.Symbol<TransactionApplyVoid>("_ZN7android21SurfaceComposerClient11Transaction5applyEv");

        static const char* const create_surface_legacy_symbols[] = {
            "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8Ejjij",
        };
        static const char* const create_surface_parent_symbols[] = {
            "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjiiRKNS_2spINS_7IBinderEEENS_3gui13LayerMetadataEPj",
            "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijRKNS_2spINS_7IBinderEEEPNS_13LayerMetadataEPj",
            "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijRKNS_2spINS_7IBinderEEEPNS_3gui13LayerMetadataEPj",
        };
        create_surface_legacy = Find<CreateSurfaceLegacy>(create_surface_legacy_symbols);
        create_surface_parent = Find<CreateSurfaceWithParent>(create_surface_parent_symbols);

        if (SurfaceComposerClient_ctor == nullptr ||
            String8_ctor == nullptr ||
            String8_dtor == nullptr ||
            SurfaceComposerClient_initCheck == nullptr ||
            (SurfaceControl_getSurface == nullptr && SurfaceControl_createSurface == nullptr) ||
            Transaction_ctor == nullptr ||
            Transaction_setLayer == nullptr ||
            Transaction_setPosition == nullptr ||
            Transaction_show == nullptr ||
            (Transaction_apply_bool == nullptr && Transaction_apply_bool_bool == nullptr && Transaction_apply_void == nullptr) ||
            (create_surface_legacy == nullptr && create_surface_parent == nullptr))
        {
            LOGE("libgui ABI symbols not found; inspect /system/lib64/libgui.so with nm -D | c++filt");
            DumpMissingSymbols();
            return false;
        }

        return true;
    }

    void Close()
    {
        m_libutils.Close();
        m_libgui.Close();
    }

    bool CreateClient(void* storage, size_t storage_size)
    {
        memset(storage, 0, storage_size);
        SurfaceComposerClient_ctor(storage);
        if (SurfaceComposerClient_onFirstRef != nullptr)
            SurfaceComposerClient_onFirstRef(storage);

        int32_t status = SurfaceComposerClient_initCheck(storage);
        if (status != 0)
        {
            LOGE("SurfaceComposerClient::initCheck failed: %d", status);
            return false;
        }
        return true;
    }

    void DestroyClient(void* client)
    {
        if (SurfaceComposerClient_dtor != nullptr)
            SurfaceComposerClient_dtor(client);
    }

    SpOpaque CreateSurface(void* client, const Options& options)
    {
        SpOpaque surface_control;
        alignas(std::max_align_t) unsigned char name[128];
        alignas(std::max_align_t) unsigned char metadata[1024];
        memset(name, 0, sizeof(name));
        memset(metadata, 0, sizeof(metadata));

        String8_ctor(name, "imgui-root");
        if (LayerMetadata_ctor != nullptr)
            LayerMetadata_ctor(metadata);

        if (create_surface_parent != nullptr)
        {
            SpOpaque parent;
            uint32_t transform_hint = 0;
            surface_control = create_surface_parent(client,
                                                    name,
                                                    static_cast<uint32_t>(options.width),
                                                    static_cast<uint32_t>(options.height),
                                                    kPixelFormatRgba8888,
                                                    0,
                                                    parent,
                                                    metadata,
                                                    &transform_hint);
        }
        else
        {
            surface_control = create_surface_legacy(client,
                                                    name,
                                                    static_cast<uint32_t>(options.width),
                                                    static_cast<uint32_t>(options.height),
                                                    kPixelFormatRgba8888,
                                                    0);
        }

        String8_dtor(name);
        return surface_control;
    }

    SpOpaque GetSurface(const SpOpaque& surface_control)
    {
        SpOpaque surface;
        if (SurfaceControl_getSurface != nullptr)
            surface = SurfaceControl_getSurface(surface_control.ptr);
        else
            surface = SurfaceControl_createSurface(surface_control.ptr);
        return surface;
    }

    void ShowSurface(const SpOpaque& surface_control, const Options& options)
    {
        alignas(std::max_align_t) unsigned char transaction[4096];
        memset(transaction, 0, sizeof(transaction));
        Transaction_ctor(transaction);
        Transaction_setLayer(transaction, surface_control, options.layer);
        Transaction_setPosition(transaction, surface_control, 0.0f, 0.0f);
        if (options.trusted_overlay)
        {
            if (Transaction_setTrustedOverlay != nullptr)
                Transaction_setTrustedOverlay(transaction, surface_control, true);
            else
                LOGE("Transaction::setTrustedOverlay(bool) not found; touches may be blocked by Android overlay security");
        }
        Transaction_show(transaction, surface_control);
        Apply(transaction);
        if (Transaction_dtor != nullptr)
            Transaction_dtor(transaction);
    }

    void HideSurface(const SpOpaque& surface_control)
    {
        if (surface_control.ptr == nullptr || Transaction_hide == nullptr)
            return;

        alignas(std::max_align_t) unsigned char transaction[4096];
        memset(transaction, 0, sizeof(transaction));
        Transaction_ctor(transaction);
        Transaction_hide(transaction, surface_control);
        Apply(transaction);
        if (Transaction_dtor != nullptr)
            Transaction_dtor(transaction);
    }

private:
    using SccCtor = void (*)(void*);
    using SccDtor = void (*)(void*);
    using SccOnFirstRef = void (*)(void*);
    using SccInitCheck = int32_t (*)(void*);
    using String8Ctor = void (*)(void*, const char*);
    using String8Dtor = void (*)(void*);
    using LayerMetadataCtor = void (*)(void*);
    using CreateSurfaceLegacy = SpOpaque (*)(void*, const void*, uint32_t, uint32_t, int32_t, uint32_t);
    using CreateSurfaceWithParent = SpOpaque (*)(void*, const void*, uint32_t, uint32_t, int32_t, int32_t, const SpOpaque&, const void*, uint32_t*);
    using SurfaceControlGetSurface = SpOpaque (*)(void*);
    using TransactionCtor = void (*)(void*);
    using TransactionDtor = void (*)(void*);
    using TransactionSetLayer = void* (*)(void*, const SpOpaque&, int32_t);
    using TransactionSetPosition = void* (*)(void*, const SpOpaque&, float, float);
    using TransactionShow = void* (*)(void*, const SpOpaque&);
    using TransactionSetTrustedOverlay = void* (*)(void*, const SpOpaque&, bool);
    using TransactionApplyVoid = void (*)(void*);
    using TransactionApplyBool = void (*)(void*, bool);
    using TransactionApplyBoolBool = void (*)(void*, bool, bool);

    template <typename T, size_t N>
    T Find(const char* const (&names)[N]) const
    {
        for (const char* name : names)
        {
            T symbol = m_libgui.Symbol<T>(name);
            if (symbol != nullptr)
            {
                LOGI("resolved symbol: %s", name);
                return symbol;
            }
        }
        return nullptr;
    }

    void Apply(void* transaction)
    {
        if (Transaction_apply_bool_bool != nullptr)
            Transaction_apply_bool_bool(transaction, false, false);
        else if (Transaction_apply_bool != nullptr)
            Transaction_apply_bool(transaction, false);
        else
            Transaction_apply_void(transaction);
    }

    void DumpMissingSymbols() const
    {
        if (SurfaceComposerClient_ctor == nullptr)
            LOGE("missing SurfaceComposerClient constructor");
        if (String8_ctor == nullptr || String8_dtor == nullptr)
            LOGE("missing android::String8 constructor/destructor");
        if (SurfaceComposerClient_initCheck == nullptr)
            LOGE("missing SurfaceComposerClient::initCheck");
        if (create_surface_legacy == nullptr && create_surface_parent == nullptr)
            LOGE("missing supported SurfaceComposerClient::createSurface overload");
        if (SurfaceControl_getSurface == nullptr)
            LOGE("missing SurfaceControl::getSurface/createSurface");
        if (Transaction_ctor == nullptr)
            LOGE("missing SurfaceComposerClient::Transaction constructor");
        if (Transaction_setLayer == nullptr)
            LOGE("missing Transaction::setLayer");
        if (Transaction_setPosition == nullptr)
            LOGE("missing Transaction::setPosition");
        if (Transaction_show == nullptr)
            LOGE("missing Transaction::show");
        if (Transaction_apply_bool == nullptr && Transaction_apply_bool_bool == nullptr && Transaction_apply_void == nullptr)
            LOGE("missing Transaction::apply");
    }

    DynamicLibrary m_libgui;
    DynamicLibrary m_libutils;
    SccCtor SurfaceComposerClient_ctor = nullptr;
    SccDtor SurfaceComposerClient_dtor = nullptr;
    SccOnFirstRef SurfaceComposerClient_onFirstRef = nullptr;
    SccInitCheck SurfaceComposerClient_initCheck = nullptr;
    CreateSurfaceLegacy create_surface_legacy = nullptr;
    CreateSurfaceWithParent create_surface_parent = nullptr;
    SurfaceControlGetSurface SurfaceControl_getSurface = nullptr;
    SurfaceControlGetSurface SurfaceControl_createSurface = nullptr;
    String8Ctor String8_ctor = nullptr;
    String8Dtor String8_dtor = nullptr;
    LayerMetadataCtor LayerMetadata_ctor = nullptr;
    TransactionCtor Transaction_ctor = nullptr;
    TransactionDtor Transaction_dtor = nullptr;
    TransactionSetLayer Transaction_setLayer = nullptr;
    TransactionSetPosition Transaction_setPosition = nullptr;
    TransactionShow Transaction_show = nullptr;
    TransactionShow Transaction_hide = nullptr;
    TransactionSetTrustedOverlay Transaction_setTrustedOverlay = nullptr;
    TransactionApplyVoid Transaction_apply_void = nullptr;
    TransactionApplyBool Transaction_apply_bool = nullptr;
    TransactionApplyBoolBool Transaction_apply_bool_bool = nullptr;
};

class AndroidSurface
{
public:
    bool Create(const Options& options)
    {
        if (!m_abi.Load())
            return false;
        if (!m_abi.CreateClient(m_client_storage, sizeof(m_client_storage)))
            return false;

        m_control = m_abi.CreateSurface(m_client_storage, options);
        if (m_control.ptr == nullptr)
        {
            LOGE("createSurface returned null SurfaceControl");
            return false;
        }

        m_abi.ShowSurface(m_control, options);
        m_visible = true;

        m_surface = m_abi.GetSurface(m_control);
        if (m_surface.ptr == nullptr)
        {
            LOGE("SurfaceControl::getSurface returned null");
            Destroy();
            return false;
        }

        m_window = FindNativeWindowBase(m_surface.ptr);
        if (m_window == nullptr)
        {
            Destroy();
            return false;
        }
        ANativeWindow_acquire(m_window);
        ANativeWindow_setBuffersGeometry(m_window, options.width, options.height, kPixelFormatRgba8888);
        m_width = options.width;
        m_height = options.height;
        return true;
    }

    void Destroy()
    {
        Hide();
        if (m_window != nullptr)
        {
            ANativeWindow_release(m_window);
            m_window = nullptr;
        }
        m_abi.DestroyClient(m_client_storage);
        m_abi.Close();
        m_surface = {};
        m_control = {};
    }

    ANativeWindow* Window() const { return m_window; }
    int Width() const { return m_width; }
    int Height() const { return m_height; }

    void Hide()
    {
        if (m_visible)
        {
            m_abi.HideSurface(m_control);
            m_visible = false;
        }
    }

private:
    LibGuiAbi m_abi;
    alignas(std::max_align_t) unsigned char m_client_storage[4096] = {};
    SpOpaque m_control;
    SpOpaque m_surface;
    ANativeWindow* m_window = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_visible = false;
};

class EglState
{
public:
    bool Create(ANativeWindow* window)
    {
        m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (m_display == EGL_NO_DISPLAY)
        {
            LOGE("eglGetDisplay failed");
            return false;
        }
        if (eglInitialize(m_display, nullptr, nullptr) != EGL_TRUE)
        {
            LOGE("eglInitialize failed: 0x%x", eglGetError());
            return false;
        }

        const EGLint config_attributes[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 0,
            EGL_STENCIL_SIZE, 0,
            EGL_NONE,
        };

        EGLint config_count = 0;
        if (eglChooseConfig(m_display, config_attributes, &m_config, 1, &config_count) != EGL_TRUE || config_count == 0)
        {
            LOGE("eglChooseConfig failed: 0x%x", eglGetError());
            return false;
        }

        EGLint native_format = 0;
        eglGetConfigAttrib(m_display, m_config, EGL_NATIVE_VISUAL_ID, &native_format);
        ANativeWindow_setBuffersGeometry(window, 0, 0, native_format);

        const EGLint context_attributes[] = {
            EGL_CONTEXT_CLIENT_VERSION, 3,
            EGL_NONE,
        };
        m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, context_attributes);
        if (m_context == EGL_NO_CONTEXT)
        {
            LOGE("eglCreateContext failed: 0x%x", eglGetError());
            return false;
        }

        m_surface = eglCreateWindowSurface(m_display, m_config, window, nullptr);
        if (m_surface == EGL_NO_SURFACE)
        {
            LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
            return false;
        }

        if (eglMakeCurrent(m_display, m_surface, m_surface, m_context) != EGL_TRUE)
        {
            LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
            return false;
        }

        eglSwapInterval(m_display, 1);
        return true;
    }

    void Destroy()
    {
        if (m_display != EGL_NO_DISPLAY)
        {
            eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (m_surface != EGL_NO_SURFACE)
                eglDestroySurface(m_display, m_surface);
            if (m_context != EGL_NO_CONTEXT)
                eglDestroyContext(m_display, m_context);
            eglTerminate(m_display);
        }

        m_display = EGL_NO_DISPLAY;
        m_surface = EGL_NO_SURFACE;
        m_context = EGL_NO_CONTEXT;
    }

    void SwapBuffers()
    {
        eglSwapBuffers(m_display, m_surface);
    }

private:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLConfig m_config = nullptr;
    EGLSurface m_surface = EGL_NO_SURFACE;
    EGLContext m_context = EGL_NO_CONTEXT;
};

class LinuxTouchInput
{
public:
    void OpenAll(const Options& options)
    {
        m_width = static_cast<float>(options.width);
        m_height = static_cast<float>(options.height);
        m_swap_xy = options.touch_swap_xy;
        m_invert_x = options.touch_invert_x;
        m_invert_y = options.touch_invert_y;

        for (int i = 0; i < 32; ++i)
        {
            char path[64];
            snprintf(path, sizeof(path), "/dev/input/event%d", i);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                continue;

            Device device;
            if (!ProbeDevice(fd, path, device))
            {
                close(fd);
                continue;
            }

            device.pfd.fd = fd;
            device.pfd.events = POLLIN;
            m_devices.push_back(device);
            LOGI("opened touch device %s x=[%d,%d] y=[%d,%d]",
                 path,
                 device.x_abs.minimum,
                 device.x_abs.maximum,
                 device.y_abs.minimum,
                 device.y_abs.maximum);
        }
    }

    void CloseAll()
    {
        for (Device& device : m_devices)
            close(device.pfd.fd);
        m_devices.clear();
    }

    void Poll(ImGuiIO& io)
    {
        if (m_devices.empty())
            return;

        std::vector<pollfd> fds;
        fds.reserve(m_devices.size());
        for (const Device& device : m_devices)
            fds.push_back(device.pfd);

        int ready = poll(fds.data(), fds.size(), 0);
        if (ready <= 0)
            return;

        for (size_t i = 0; i < fds.size(); ++i)
        {
            pollfd& pfd = fds[i];
            if ((pfd.revents & POLLIN) == 0)
                continue;

            input_event event;
            while (read(pfd.fd, &event, sizeof(event)) == sizeof(event))
                HandleEvent(m_devices[i], event, io);
        }
    }

private:
    struct Device
    {
        pollfd pfd = {};
        int x_code = ABS_MT_POSITION_X;
        int y_code = ABS_MT_POSITION_Y;
        input_absinfo x_abs = {};
        input_absinfo y_abs = {};
        int slot = 0;
        int slot_count = 1;
        std::array<int, 16> tracking_ids = {};
        std::array<float, 16> xs = {};
        std::array<float, 16> ys = {};
        bool has_mt_slots = false;
        bool button_down = false;
    };

    static bool GetAbsInfo(int fd, int code, input_absinfo* info)
    {
        memset(info, 0, sizeof(*info));
        return ioctl(fd, EVIOCGABS(code), info) == 0 && info->maximum > info->minimum;
    }

    bool ProbeDevice(int fd, const char* path, Device& device)
    {
        if (GetAbsInfo(fd, ABS_MT_POSITION_X, &device.x_abs) &&
            GetAbsInfo(fd, ABS_MT_POSITION_Y, &device.y_abs))
        {
            device.x_code = ABS_MT_POSITION_X;
            device.y_code = ABS_MT_POSITION_Y;
            input_absinfo slot_abs = {};
            if (GetAbsInfo(fd, ABS_MT_SLOT, &slot_abs))
            {
                device.has_mt_slots = true;
                device.slot_count = slot_abs.maximum - slot_abs.minimum + 1;
                if (device.slot_count > static_cast<int>(device.tracking_ids.size()))
                    device.slot_count = static_cast<int>(device.tracking_ids.size());
            }
            for (int& tracking_id : device.tracking_ids)
                tracking_id = -1;
            return true;
        }

        if (GetAbsInfo(fd, ABS_X, &device.x_abs) &&
            GetAbsInfo(fd, ABS_Y, &device.y_abs))
        {
            device.x_code = ABS_X;
            device.y_code = ABS_Y;
            device.tracking_ids[0] = 0;
            return true;
        }

        LOGI("skipped non-touch input device %s", path);
        return false;
    }

    static float ScaleAxis(int value, const input_absinfo& abs, float size)
    {
        float t = static_cast<float>(value - abs.minimum) / static_cast<float>(abs.maximum - abs.minimum);
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
        return t * size;
    }

    void HandleEvent(Device& device, const input_event& event, ImGuiIO& io)
    {
        if (event.type == EV_ABS)
        {
            if (event.code == ABS_MT_SLOT && device.has_mt_slots)
            {
                if (event.value >= 0 && event.value < device.slot_count)
                    device.slot = event.value;
            }
            else if (event.code == ABS_MT_TRACKING_ID && device.has_mt_slots)
            {
                device.tracking_ids[device.slot] = event.value;
            }
            else if (event.code == device.x_code)
                device.xs[device.slot] = ScaleAxis(event.value, device.x_abs, m_swap_xy ? m_height : m_width);
            else if (event.code == device.y_code)
                device.ys[device.slot] = ScaleAxis(event.value, device.y_abs, m_swap_xy ? m_width : m_height);
            else if (event.code == ABS_MT_TRACKING_ID)
                device.tracking_ids[0] = event.value;
        }
        else if (event.type == EV_KEY && (event.code == BTN_TOUCH || event.code == BTN_LEFT))
        {
            device.button_down = event.value != 0;
            if (!device.has_mt_slots)
                device.tracking_ids[0] = event.value != 0 ? 0 : -1;
        }
        else if (event.type == EV_SYN && event.code == SYN_REPORT)
        {
            int active_slot = -1;
            for (int i = 0; i < device.slot_count; ++i)
            {
                if (device.tracking_ids[i] >= 0)
                {
                    active_slot = i;
                    break;
                }
            }

            bool down = active_slot >= 0 || device.button_down;
            int report_slot = active_slot >= 0 ? active_slot : device.slot;
            float x = device.xs[report_slot];
            float y = device.ys[report_slot];
            if (m_swap_xy)
            {
                float old_x = x;
                x = y;
                y = old_x;
            }
            if (m_invert_x)
                x = m_width - x;
            if (m_invert_y)
                y = m_height - y;

            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, down);
        }
    }

    std::vector<Device> m_devices;
    float m_width = 1.0f;
    float m_height = 1.0f;
    bool m_swap_xy = false;
    bool m_invert_x = false;
    bool m_invert_y = false;
};

static void SetupImGui(const Options& options)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "/data/local/tmp/imgui-root.ini";
    io.DisplaySize = ImVec2(static_cast<float>(options.width), static_cast<float>(options.height));

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(options.dpi_scale);
    style.FontScaleDpi = options.dpi_scale;

    ImGui_ImplOpenGL3_Init("#version 300 es");
}

static void DrawUi(bool* show_demo_window)
{
    ImGui::SetNextWindowPos(ImVec2(40, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 360), ImGuiCond_FirstUseEver);

    ImGui::Begin("Root ImGui");
    ImGui::Text("Standalone native process");
    ImGui::Text("pid: %d", getpid());
    ImGui::Checkbox("Demo window", show_demo_window);
    ImGui::Separator();
    ImGui::Text("Touch input comes from /dev/input/event*.");
    ImGui::Text("Press Ctrl+C in adb shell to exit.");
    ImGui::Text("%.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
    ImGui::End();

    if (*show_demo_window)
        ImGui::ShowDemoWindow(show_demo_window);
}

int main(int argc, char** argv)
{
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    Options options = ParseOptions(argc, argv);
    LOGI("starting %dx%d layer=%d scale=%.2f", options.width, options.height, options.layer, options.dpi_scale);

    AndroidSurface android_surface;
    if (!android_surface.Create(options))
        return 1;

    EglState egl;
    if (!egl.Create(android_surface.Window()))
    {
        android_surface.Destroy();
        return 1;
    }

    LinuxTouchInput input;
    input.OpenAll(options);

    SetupImGui(options);

    bool show_demo_window = true;
    ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    while (g_Running)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(android_surface.Width()), static_cast<float>(android_surface.Height()));
        input.Poll(io);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        DrawUi(&show_demo_window);

        ImGui::Render();
        glViewport(0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y));
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        egl.SwapBuffers();
    }

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    egl.SwapBuffers();
    android_surface.Hide();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    input.CloseAll();
    egl.Destroy();
    android_surface.Destroy();
    LOGI("stopped");
    return 0;
}
