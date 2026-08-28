// ============================================================================
//  dimmer.cpp
//
//  A click-through screen dimmer overlay for Linux / X11, rendered with Vulkan,
//  plus a small Xlib control panel.
//
//  One overlay window (and one slider in the panel) is created per physical
//  monitor, as reported by XRandR (XRRGetMonitors) on the default screen.
//  Passing --geometry opts back out of this and creates a single overlay of
//  the requested size instead, as before.
//
//  Key properties of each overlay window:
//    * 32-bit ARGB visual  -> the window itself has real per-pixel alpha.
//    * VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR -> the compositor blends the
//      swapchain image with whatever is behind it (your desktop).
//    * Empty XFixes input region (ShapeInput) -> the overlay is completely
//      transparent to input. Mouse clicks, motion and keyboard events all go
//      straight through to the applications underneath. We never grab or
//      intercept anything.
//
//  The control panel is a separate, ordinary window. It is the only thing that
//  takes input, and it only ever receives events addressed to itself. Run with
//  --no-panel for a headless overlay driven purely by signals:
//      SIGUSR1  -> darker (-1 on the visibility scale, on the focused screen)
//      SIGUSR2  -> lighter (+1)
//      SIGINT / SIGTERM -> quit
//
//  Requires a running compositor (picom, mutter, kwin, ...). Under a bare
//  X server with no compositor there is nothing to blend against.
//
//  Build:  see CMakeLists.txt, or
//      g++ -std=c++17 -O2 dimmer.cpp -o dimmer -lvulkan -lX11 -lXfixes -lXext
// ============================================================================

#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrandr.h>

#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static float clampf(float v, float lo, float hi)
{
    return std::max(lo, std::min(hi, v));
}

// ---------------------------------------------------------------------------
// The dim scale
//
// One scale is used everywhere: `level` is the percentage of the desktop that
// stays visible through the overlay, from 0 (darkest we allow) to 100 (no dim
// at all). The command line, the signals and the panel slider all speak this
// scale, so the number the user sees is always the number they set.
// ---------------------------------------------------------------------------

static float levelToAlpha(float level)
{
    float t = level / 100.0f;
    return 0.7 * (1.0f - t) + 0.0 * t;
}

// ---------------------------------------------------------------------------
// Signal plumbing (self-pipe trick so we can wake up poll())
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_quit      = 0;
static int                   g_sigPipe[2] = {-1, -1};

// ---------------------------------------------------------------------------
// Command line options
// ---------------------------------------------------------------------------

struct Options {
    float level = 50.0f; // % of desktop visible, applied to every screen at startup
};

// ---------------------------------------------------------------------------
// Panel geometry and colours
// ---------------------------------------------------------------------------

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// Source colours; each is resolved to a pixel value at startup so the panel
// works on any default visual, not just 24-bit TrueColor.
struct PanelTheme {
    unsigned long background   = 0x1b1d21;
    unsigned long mutedText    = 0x9aa2ad;
    unsigned long track        = 0x3a3f46;
    unsigned long trackFilled  = 0x00a2ff;
    unsigned long knob         = 0xffffff;

    void resolve(Display* dpy, Colormap cmap)
    {
        unsigned long* fields[] = {
            &background, &mutedText,
            &track, &trackFilled, &knob,
        };
        for (unsigned long* f : fields) {
            XColor c {};
            c.red   = (unsigned short)(((*f >> 16) & 0xFF) * 257);
            c.green = (unsigned short)(((*f >> 8)  & 0xFF) * 257);
            c.blue  = (unsigned short)(( *f        & 0xFF) * 257);
            c.flags = DoRed | DoGreen | DoBlue;
            if (XAllocColor(dpy, cmap, &c))
                *f = c.pixel;
        }
    }
};

// ---------------------------------------------------------------------------
// One overlay window: everything tied to a single X11 screen's Vulkan surface
// and swapchain, plus the dim level the user has set for that screen.
// ---------------------------------------------------------------------------

struct Overlay {
    int      index  = 0;   // 0-based monitor index, used only for labels
    Window   win    = 0;
    int      x      = 0;   // position and size actually created
    int      y      = 0;
    int      width  = 0;
    int      height = 0;

    float level = 50.0f;  // % of desktop visible on this screen
    float alpha = 0.0f;   // Vulkan clear alpha derived from level, applied instantly
    bool  dirty = true;   // level/alpha changed since the last draw

    VkSurfaceKHR   surface     = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain   = VK_NULL_HANDLE;
    VkFormat       format      = VK_FORMAT_UNDEFINED;
    VkExtent2D     extent      = {0, 0};
    bool           swapchainUsable = false;
    bool           premultiplied   = true;

    std::vector<VkImage>       images;
    std::vector<VkImageView>   views;
    std::vector<VkFramebuffer> framebuffers;
    VkRenderPass               renderPass = VK_NULL_HANDLE;

    VkCommandBuffer cmd            = VK_NULL_HANDLE;
    VkSemaphore     imageAvailable = VK_NULL_HANDLE;
    VkSemaphore     renderFinished = VK_NULL_HANDLE;
    VkFence         inFlight       = VK_NULL_HANDLE;
};

// ---------------------------------------------------------------------------
// The application
// ---------------------------------------------------------------------------

class Dimmer {
public:
    explicit Dimmer(const Options& opt) : m_opt(opt) {}

    void run()
    {
        createOverlays();
        m_panel.height = 20 + 80 * (int)m_overlays.size();
        createPanel();
        createInstance();
        createSurfaces();
        pickPhysicalDevice();
        createDevice();
        createCommandPool();
        for (Overlay& ov : m_overlays) {
            createSyncObjects(ov);
            allocateCommandBuffer(ov);
            createSwapchain(ov);
        }
        mainLoop();
        cleanup();
    }

private:
    // ---- Dim state --------------------------------------------------------

    void initState()
    {
        for (Overlay& ov : m_overlays) {
            ov.level = m_opt.level;
            ov.alpha = levelToAlpha(ov.level);
        }
    }

    void setLevel(int idx, float level)
    {
        Overlay& ov = m_overlays[idx];
        ov.level = clampf(level, 0.0f, 100.0f);
        ov.alpha = levelToAlpha(ov.level);
        ov.dirty = true;   // needs re-rendering with the new value
    }

    // ---- Overlay windows ----------------------------------------------------

    // One rectangle per overlay to create: either every physical monitor
    // XRandR reports, or a single user-requested --geometry region.
    std::vector<Rect> planOverlayGeometry(Window root) const
    {
        int evBase = 0, errBase = 0;
        if (XRRQueryExtension(m_dpy, &evBase, &errBase)) {
            int n = 0;
            XRRMonitorInfo* mons = XRRGetMonitors(m_dpy, root, True, &n);
            if (mons && n > 0) {
                std::vector<Rect> geos;
                geos.reserve(n);
                for (int i = 0; i < n; ++i) {
                    Rect r{ mons[i].x, mons[i].y, mons[i].width, mons[i].height };
                    // Mirrored (cloned) monitors report identical geometry and
                    // show identical pixels, so give them one overlay/slider
                    // instead of two that would fight over the same screen.
                    bool duplicate = std::any_of(geos.begin(), geos.end(),
                        [&](const Rect& g) {
                            return g.x == r.x && g.y == r.y && g.w == r.w && g.h == r.h;
                        });
                    if (!duplicate)
                        geos.push_back(r);
                }
                XRRFreeMonitors(mons);
                return geos;
            }
            if (mons) XRRFreeMonitors(mons);
        }

        // No RandR, or it reported nothing: one overlay spanning the screen.
        return { Rect{ 0, 0, DisplayWidth(m_dpy, m_screen), DisplayHeight(m_dpy, m_screen) } };
    }

    void createOverlays()
    {
        m_dpy = XOpenDisplay(nullptr);
        if (!m_dpy)
            std::exit(1);

        m_screen = DefaultScreen(m_dpy);
        Window root = RootWindow(m_dpy, m_screen);

        // A 32-bit TrueColor visual is what gives every overlay window an
        // alpha channel; one visual/colormap pair is shared by all of them
        // since they all live on the same screen.
        XVisualInfo vinfo {};
        if (!XMatchVisualInfo(m_dpy, m_screen, 32, TrueColor, &vinfo))
            std::exit(1);
        m_visual = vinfo.visual;
        m_colormap = XCreateColormap(m_dpy, root, m_visual, AllocNone);

        std::vector<Rect> geos = planOverlayGeometry(root);
        m_overlays.resize(geos.size());
        initState();

        for (int i = 0; i < (int)m_overlays.size(); ++i) {
            Overlay& ov = m_overlays[i];
            ov.index  = i;
            ov.x      = geos[i].x;
            ov.y      = geos[i].y;
            ov.width  = geos[i].w;
            ov.height = geos[i].h;

            XSetWindowAttributes swa {};
            swa.colormap          = m_colormap;
            swa.background_pixel  = 0;   // fully transparent, no garbage on map
            swa.border_pixel      = 0;
            swa.override_redirect = True;
            swa.event_mask        = StructureNotifyMask | ExposureMask;

            unsigned long mask = CWColormap | CWBackPixel | CWBorderPixel |
                                 CWOverrideRedirect | CWEventMask;

            ov.win = XCreateWindow(m_dpy, root,
                                   ov.x, ov.y,
                                   (unsigned)ov.width, (unsigned)ov.height,
                                   0, 32, InputOutput, m_visual, mask, &swa);
            if (!ov.win)
                std::exit(1);

            char title[32];
            std::snprintf(title, sizeof(title), "dimmer-%d", i);
            XStoreName(m_dpy, ov.win, title);

            setWindowHints(ov.win);
            makeInputTransparent(ov.win);

            XMapRaised(m_dpy, ov.win);
        }
        XFlush(m_dpy);
    }

    // Ask the window manager to keep us on top and out of the way. These hints
    // only matter when --managed is used; with override-redirect the WM ignores
    // the window completely.
    void setWindowHints(Window win)
    {
        Atom wmType   = XInternAtom(m_dpy, "_NET_WM_WINDOW_TYPE", False);
        Atom typeDock = XInternAtom(m_dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
        XChangeProperty(m_dpy, win, wmType, XA_ATOM, 32, PropModeReplace,
                        (unsigned char*)&typeDock, 1);

        Atom wmState = XInternAtom(m_dpy, "_NET_WM_STATE", False);
        Atom states[] = {
            XInternAtom(m_dpy, "_NET_WM_STATE_ABOVE", False),
            XInternAtom(m_dpy, "_NET_WM_STATE_SKIP_TASKBAR", False),
            XInternAtom(m_dpy, "_NET_WM_STATE_SKIP_PAGER", False),
            XInternAtom(m_dpy, "_NET_WM_STATE_STICKY", False),
        };
        XChangeProperty(m_dpy, win, wmState, XA_ATOM, 32, PropModeReplace,
                        (unsigned char*)states,
                        (int)(sizeof(states) / sizeof(states[0])));

        // Present on every virtual desktop.
        Atom desktop = XInternAtom(m_dpy, "_NET_WM_DESKTOP", False);
        unsigned long allDesktops = 0xFFFFFFFF;
        XChangeProperty(m_dpy, win, desktop, XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char*)&allDesktops, 1);

        // Never take focus.
        XWMHints hints {};
        hints.flags = InputHint;
        hints.input = False;
        XSetWMHints(m_dpy, win, &hints);
    }

    // ---- The part that makes us NOT steal events from other programs ------
    //
    // The bounding shape stays as-is (so we are still drawn), but the *input*
    // shape is set to an empty region. The X server then treats every pixel of
    // this window as "not there" for hit testing, and delivers pointer and
    // keyboard events to whatever window is below. We install no grabs, no
    // event selection on other windows, and no XTest/record hooks.
    void makeInputTransparent(Window win)
    {
        int evBase = 0, errBase = 0;
        if (!XFixesQueryExtension(m_dpy, &evBase, &errBase))
            std::exit(1);

        XserverRegion emptyRegion = XFixesCreateRegion(m_dpy, nullptr, 0);
        XFixesSetWindowShapeRegion(m_dpy, win, ShapeInput, 0, 0, emptyRegion);
        XFixesDestroyRegion(m_dpy, emptyRegion);
    }

    // ---- Control panel ----------------------------------------------------

    void createPanel()
    {
        int panelScreen = DefaultScreen(m_dpy);
        Colormap cmap = DefaultColormap(m_dpy, panelScreen);
        m_theme.resolve(m_dpy, cmap);

        m_panel.win = XCreateSimpleWindow(
            m_dpy, RootWindow(m_dpy, panelScreen),
            100, 100,
            (unsigned)m_panel.width, (unsigned)m_panel.height,
            0, m_theme.background, m_theme.background);

        XSelectInput(m_dpy, m_panel.win,
                     ExposureMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | KeyPressMask);

        XStoreName(m_dpy, m_panel.win, "Dimmer");

        XClassHint cls {};
        char name[] = "dimmer";
        char klass[] = "Dimmer";
        cls.res_name  = name;
        cls.res_class = klass;
        XSetClassHint(m_dpy, m_panel.win, &cls);

        // Fixed size: the layout below is not resizable.
        XSizeHints size {};
        size.flags      = PMinSize | PMaxSize;
        size.min_width  = size.max_width  = m_panel.width;
        size.min_height = size.max_height = m_panel.height;
        XSetWMNormalHints(m_dpy, m_panel.win, &size);

        m_wmDelete = XInternAtom(m_dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(m_dpy, m_panel.win, &m_wmDelete, 1);

        m_panel.gc = XCreateGC(m_dpy, m_panel.win, 0, nullptr);
        m_panel.font = XLoadQueryFont(m_dpy, "fixed");
        if (m_panel.font)
            XSetFont(m_dpy, m_panel.gc, m_panel.font->fid);

        // Draw off-screen and blit, so dragging the slider does not flicker.
        m_panel.pixmap = XCreatePixmap(m_dpy, m_panel.win,
                                       (unsigned)m_panel.width,
                                       (unsigned)m_panel.height,
                                       (unsigned)DefaultDepth(m_dpy, panelScreen));

        XMapWindow(m_dpy, m_panel.win);
    }

    // Each slider occupies an 80px-tall block; a single slider's block plus
    // the panel's 20px bottom margin reproduces the original fixed layout.
    Rect trackRect(int i) const
    {
        // Taller than the drawn groove so the whole band is clickable.
        return { 30, i * 80 + 52, m_panel.width - 60, 28 };
    }

    int sliderAt(int px, int py) const
    {
        for (int i = 0; i < (int)m_overlays.size(); ++i)
            if (trackRect(i).contains(px, py)) return i;
        return -1;
    }

    void drawText(Drawable d, int x, int baseline, const char* s)
    {
        XDrawString(m_dpy, d, m_panel.gc, x, baseline, s, (int)std::strlen(s));
    }

    void drawPanel()
    {
        if (!m_panel.win) return;

        Pixmap px = m_panel.pixmap;
        GC gc = m_panel.gc;
        const int ascent = m_panel.font ? m_panel.font->ascent : 10;

        XSetForeground(m_dpy, gc, m_theme.background);
        XFillRectangle(m_dpy, px, gc, 0, 0,
                       (unsigned)m_panel.width, (unsigned)m_panel.height);

        bool multi = m_overlays.size() > 1;
        for (int i = 0; i < (int)m_overlays.size(); ++i) {
            char label[64];
            if (multi)
                std::snprintf(label, sizeof(label), "Screen %d visible: %d%%",
                              i + 1, (int)lroundf(m_overlays[i].level));
            else
                std::snprintf(label, sizeof(label), "Desktop visible: %d%%",
                              (int)lroundf(m_overlays[i].level));
            XSetForeground(m_dpy, gc, m_theme.mutedText);
            drawText(px, 24, i * 80 + 20 + ascent / 2, label);

            Rect tr = trackRect(i);
            int x0 = tr.x, x1 = tr.x + tr.w;
            int y  = tr.y + tr.h / 2;
            float knobFrac = clampf(m_overlays[i].level / 100.0f, 0.0f, 1.0f);
            int knobX = x0 + (int)((x1 - x0) * knobFrac);

            XSetForeground(m_dpy, gc, m_theme.track);
            XFillRectangle(m_dpy, px, gc, x0, y - 2, (unsigned)tr.w, 4);
            XSetForeground(m_dpy, gc, m_theme.trackFilled);
            XFillRectangle(m_dpy, px, gc, x0, y - 2, (unsigned)(knobX - x0), 4);
            XSetForeground(m_dpy, gc, m_theme.knob);
            XFillArc(m_dpy, px, gc, knobX - 9, y - 9, 18, 18, 0, 360 * 64);
        }

        XCopyArea(m_dpy, px, m_panel.win, gc, 0, 0,
                  (unsigned)m_panel.width, (unsigned)m_panel.height, 0, 0);
    }

    void setLevelFromPointer(int idx, int px)
    {
        Rect tr = trackRect(idx);
        float v = clampf((float)(px - tr.x) / (float)tr.w, 0.0f, 1.0f);
        setLevel(idx, v * 100.0f);
    }

    // ---- Vulkan instance / surface / device -------------------------------

    void createInstance()
    {
        VkApplicationInfo app {};
        app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName   = "dimmer";
        app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app.pEngineName        = "none";
        app.apiVersion         = VK_API_VERSION_1_0;

        const char* extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
        };

        VkInstanceCreateInfo ci {};
        ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo        = &app;
        ci.enabledExtensionCount   = 2;
        ci.ppEnabledExtensionNames = extensions;

        // Enable validation only if the layer is present and DIMMER_VALIDATE=1.
        const char* validationLayer = "VK_LAYER_KHRONOS_validation";
        if (getenv("DIMMER_VALIDATE") && layerAvailable(validationLayer)) {
            ci.enabledLayerCount   = 1;
            ci.ppEnabledLayerNames = &validationLayer;
        }

        vkCreateInstance(&ci, nullptr, &m_instance);
    }

    static bool layerAvailable(const char* name)
    {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        vkEnumerateInstanceLayerProperties(&count, layers.data());
        for (const auto& l : layers)
            if (std::strcmp(l.layerName, name) == 0) return true;
        return false;
    }

    void createSurfaces()
    {
        for (Overlay& ov : m_overlays) {
            VkXlibSurfaceCreateInfoKHR ci {};
            ci.sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
            ci.dpy    = m_dpy;
            ci.window = ov.win;
            vkCreateXlibSurfaceKHR(m_instance, &ci, nullptr, &ov.surface);
        }
    }

    void pickPhysicalDevice()
    {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
        if (count == 0)
            std::exit(1);
            //  fatal("no Vulkan physical devices found");
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

        int bestScore = -1;
        for (VkPhysicalDevice dev : devices) {
            uint32_t family = 0;
            if (!findPresentQueueFamily(dev, family)) continue;
            if (!supportsSwapchain(dev))              continue;

            VkPhysicalDeviceProperties props {};
            vkGetPhysicalDeviceProperties(dev, &props);
            int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 2
                      : (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) ? 1 : 0;
            if (score > bestScore) {
                bestScore     = score;
                m_phys        = dev;
                m_queueFamily = family;
            }
        }
        if (m_phys == VK_NULL_HANDLE)
            std::exit(1);
    }

    // A family must be able to present to every overlay's surface: with one
    // overlay this is the same check as before, with more than one it also
    // rules out the rare case of a GPU that can't drive a second screen.
    bool findPresentQueueFamily(VkPhysicalDevice dev, uint32_t& outFamily) const
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families.data());

        for (uint32_t i = 0; i < count; ++i) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            bool presentsToAll = true;
            for (const Overlay& ov : m_overlays) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, ov.surface, &present);
                if (!present) { presentsToAll = false; break; }
            }
            if (presentsToAll) { outFamily = i; return true; }
        }
        return false;
    }

    static bool supportsSwapchain(VkPhysicalDevice dev)
    {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> exts(count);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, exts.data());
        for (const auto& e : exts)
            if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
                return true;
        return false;
    }

    void createDevice()
    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci {};
        qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = m_queueFamily;
        qci.queueCount       = 1;
        qci.pQueuePriorities = &priority;

        const char* deviceExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

        VkDeviceCreateInfo ci {};
        ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.queueCreateInfoCount    = 1;
        ci.pQueueCreateInfos       = &qci;
        ci.enabledExtensionCount   = 1;
        ci.ppEnabledExtensionNames = deviceExts;

        vkCreateDevice(m_phys, &ci, nullptr, &m_device);
        vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
    }

    void createSyncObjects(Overlay& ov)
    {
        VkSemaphoreCreateInfo sci {};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(m_device, &sci, nullptr, &ov.imageAvailable);
        vkCreateSemaphore(m_device, &sci, nullptr, &ov.renderFinished);

        // One fence per in-flight frame, so we can reuse the command buffer
        // without stalling the queue after every present.
        VkFenceCreateInfo fci {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(m_device, &fci, nullptr, &ov.inFlight);
    }

    void createCommandPool()
    {
        VkCommandPoolCreateInfo ci {};
        ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.queueFamilyIndex = m_queueFamily;
        ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(m_device, &ci, nullptr, &m_cmdPool);
    }

    void allocateCommandBuffer(Overlay& ov)
    {
        VkCommandBufferAllocateInfo ai {};
        ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool        = m_cmdPool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_device, &ai, &ov.cmd);
    }

    // ---- Swapchain --------------------------------------------------------

    VkSurfaceFormatKHR chooseSurfaceFormat(const Overlay& ov) const
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_phys, ov.surface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_phys, ov.surface, &count, formats.data());
        if (count == 0)
            std::exit(1);

        // Prefer a plain UNORM format: clear values then map 1:1 to stored
        // pixels, which keeps the pre-multiplied maths simple and exact.
        for (const auto& f : formats)
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM ||
                 f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return f;

        return formats[0];
    }

    VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(Overlay& ov, const VkSurfaceCapabilitiesKHR& caps)
    {
        if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
            ov.premultiplied = true;
            return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
        }
        if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) {
            ov.premultiplied = false;
            return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
        }
        if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
            // The X11 visual is ARGB, so "inherit" behaves pre-multiplied here.
            ov.premultiplied = true;
            return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        }
        ov.premultiplied = false;
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }

    void createSwapchain(Overlay& ov)
    {
        VkSurfaceCapabilitiesKHR caps {};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_phys, ov.surface, &caps);

        VkSurfaceFormatKHR fmt = chooseSurfaceFormat(ov);
        ov.format = fmt.format;

        if (caps.currentExtent.width != UINT32_MAX) {
            ov.extent = caps.currentExtent;
        } else {
            ov.extent.width  = std::max(caps.minImageExtent.width,
                              std::min(caps.maxImageExtent.width,  (uint32_t)ov.width));
            ov.extent.height = std::max(caps.minImageExtent.height,
                              std::min(caps.maxImageExtent.height, (uint32_t)ov.height));
        }
        if (ov.extent.width == 0 || ov.extent.height == 0) {
            ov.swapchainUsable = false;   // window is currently unviewable
            return;
        }

        // Low latency swapchain: two images are enough for a flat overlay.
        uint32_t imageCount = std::max(2u, caps.minImageCount);
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
            imageCount = caps.maxImageCount;

        VkSwapchainCreateInfoKHR ci {};
        ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface          = ov.surface;
        ci.minImageCount    = imageCount;
        ci.imageFormat      = fmt.format;
        ci.imageColorSpace  = fmt.colorSpace;
        ci.imageExtent      = ov.extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform     = caps.currentTransform;
        ci.compositeAlpha   = chooseCompositeAlpha(ov, caps);
        ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;   // always supported
        ci.clipped          = VK_TRUE;
        ci.oldSwapchain     = VK_NULL_HANDLE;

        vkCreateSwapchainKHR(m_device, &ci, nullptr, &ov.swapchain);
        ov.swapchainUsable = true;

        uint32_t n = 0;
        vkGetSwapchainImagesKHR(m_device, ov.swapchain, &n, nullptr);
        ov.images.resize(n);
        vkGetSwapchainImagesKHR(m_device, ov.swapchain, &n, ov.images.data());

        createRenderPass(ov);
        createViewsAndFramebuffers(ov);
    }

    void createRenderPass(Overlay& ov)
    {
        VkAttachmentDescription color {};
        color.format         = ov.format;
        color.samples        = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref {};
        ref.attachment = 0;
        ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass {};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &ref;

        VkSubpassDependency dep {};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo ci {};
        ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments    = &color;
        ci.subpassCount    = 1;
        ci.pSubpasses      = &subpass;
        ci.dependencyCount = 1;
        ci.pDependencies   = &dep;

        vkCreateRenderPass(m_device, &ci, nullptr, &ov.renderPass);
    }

    void createViewsAndFramebuffers(Overlay& ov)
    {
        ov.views.resize(ov.images.size());
        ov.framebuffers.resize(ov.images.size());

        for (size_t i = 0; i < ov.images.size(); ++i) {
            VkImageViewCreateInfo vi {};
            vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image    = ov.images[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format   = ov.format;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            vkCreateImageView(m_device, &vi, nullptr, &ov.views[i]);

            VkFramebufferCreateInfo fi {};
            fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fi.renderPass      = ov.renderPass;
            fi.attachmentCount = 1;
            fi.pAttachments    = &ov.views[i];
            fi.width           = ov.extent.width;
            fi.height          = ov.extent.height;
            fi.layers          = 1;
            vkCreateFramebuffer(m_device, &fi, nullptr, &ov.framebuffers[i]);
        }
    }

    void destroySwapchain(Overlay& ov)
    {
        if (m_device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(m_device);

        for (VkFramebuffer fb : ov.framebuffers)
            if (fb) vkDestroyFramebuffer(m_device, fb, nullptr);
        ov.framebuffers.clear();

        for (VkImageView v : ov.views)
            if (v) vkDestroyImageView(m_device, v, nullptr);
        ov.views.clear();

        if (ov.renderPass) {
            vkDestroyRenderPass(m_device, ov.renderPass, nullptr);
            ov.renderPass = VK_NULL_HANDLE;
        }
        if (ov.swapchain) {
            vkDestroySwapchainKHR(m_device, ov.swapchain, nullptr);
            ov.swapchain = VK_NULL_HANDLE;
        }
        ov.images.clear();
        ov.swapchainUsable = false;
    }

    void recreateSwapchain(Overlay& ov)
    {
        destroySwapchain(ov);
        createSwapchain(ov);
    }

    // ---- Drawing ----------------------------------------------------------

    VkClearValue clearValue(const Overlay& ov) const
    {
        const float a = ov.alpha;
        const float r = m_tintR, g = m_tintG, b = m_tintB;
        VkClearValue cv {};
        if (ov.premultiplied) {
            // Colour channels must already be scaled by alpha.
            cv.color = {{ r * a, g * a, b * a, a }};
        } else {
            cv.color = {{ r, g, b, a }};
        }
        return cv;
    }

    // Returns false if the swapchain needs to be rebuilt.
    bool drawFrame(Overlay& ov)
    {
        if (!ov.swapchainUsable) return false;

        // Wait for the previous submission before reusing ov.cmd.
        vkWaitForFences(m_device, 1, &ov.inFlight, VK_TRUE, UINT64_MAX);

        uint32_t imageIndex = 0;
        VkResult r = vkAcquireNextImageKHR(m_device, ov.swapchain, UINT64_MAX,
                                           ov.imageAvailable, VK_NULL_HANDLE,
                                           &imageIndex);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) return false;
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
            std::exit(1);

        vkResetFences(m_device, 1, &ov.inFlight);
        vkResetCommandBuffer(ov.cmd, 0);

        VkCommandBufferBeginInfo bi {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(ov.cmd, &bi);

        VkClearValue cv = clearValue(ov);

        VkRenderPassBeginInfo rp {};
        rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass        = ov.renderPass;
        rp.framebuffer       = ov.framebuffers[imageIndex];
        rp.renderArea.offset = {0, 0};
        rp.renderArea.extent = ov.extent;
        rp.clearValueCount   = 1;
        rp.pClearValues      = &cv;

        // The whole overlay is a single flat translucent quad, so a clear is
        // all the "rendering" we need.
        vkCmdBeginRenderPass(ov.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(ov.cmd);

        vkEndCommandBuffer(ov.cmd);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si {};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &ov.imageAvailable;
        si.pWaitDstStageMask    = &waitStage;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &ov.cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &ov.renderFinished;
        vkQueueSubmit(m_queue, 1, &si, ov.inFlight);

        VkPresentInfoKHR pi {};
        pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &ov.renderFinished;
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &ov.swapchain;
        pi.pImageIndices      = &imageIndex;

        r = vkQueuePresentKHR(m_queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return false;
        if (r != VK_SUCCESS)
            std::exit(1);
        return true;
    }

    // ---- Main loop --------------------------------------------------------

    static int fullRepaint(const Overlay& ov) { return (int)ov.images.size() + 1; }

    int overlayIndexForWindow(Window w) const
    {
        for (int i = 0; i < (int)m_overlays.size(); ++i)
            if (m_overlays[i].win == w) return i;
        return -1;
    }

    void handleXEvents(std::vector<int>& framesToRender, std::vector<bool>& needRecreate,
                       bool& repaintPanel)
    {
        while (XPending(m_dpy)) {
            XEvent ev;
            XNextEvent(m_dpy, &ev);
            switch (ev.type) {
                case ConfigureNotify: {
                    int idx = overlayIndexForWindow(ev.xconfigure.window);
                    if (idx >= 0 &&
                        (ev.xconfigure.width  != (int)m_overlays[idx].extent.width ||
                         ev.xconfigure.height != (int)m_overlays[idx].extent.height))
                        needRecreate[idx] = true;
                    break;
                }

                case Expose: {
                    if (ev.xexpose.window == m_panel.win) {
                        repaintPanel = true;
                    } else {
                        int idx = overlayIndexForWindow(ev.xexpose.window);
                        if (idx >= 0)
                            framesToRender[idx] = std::max(framesToRender[idx], fullRepaint(m_overlays[idx]));
                    }
                    break;
                }

                case MapNotify: {
                    int idx = overlayIndexForWindow(ev.xmap.window);
                    if (idx >= 0)
                        framesToRender[idx] = std::max(framesToRender[idx], fullRepaint(m_overlays[idx]));
                    break;
                }

                case ButtonPress: {
                    if (ev.xbutton.window != m_panel.win || ev.xbutton.button != Button1)
                        break;
                    int idx = sliderAt(ev.xbutton.x, ev.xbutton.y);
                    if (idx >= 0) {
                        m_panel.draggingIndex = idx;
                        m_focusedOverlay = idx;
                        setLevelFromPointer(idx, ev.xbutton.x);
                        repaintPanel = true;
                    }
                    break;
                }

                case MotionNotify:
                    if (m_panel.draggingIndex >= 0 && ev.xmotion.window == m_panel.win) {
                        setLevelFromPointer(m_panel.draggingIndex, ev.xmotion.x);
                        repaintPanel = true;
                    }
                    break;

                case ButtonRelease:
                    if (ev.xbutton.window == m_panel.win)
                        m_panel.draggingIndex = -1;
                    break;

                case KeyPress: {
                    if (ev.xkey.window != m_panel.win) break;
                    KeySym ks = XLookupKeysym(&ev.xkey, 0);
                    float level = m_overlays[m_focusedOverlay].level;
                    if (ks == XK_Escape || ks == XK_q) {
                        g_quit = 1;
                    } else if (ks == XK_Left || ks == XK_Down) {
                        setLevel(m_focusedOverlay, level - 1.0f);
                        repaintPanel = true;
                    } else if (ks == XK_Right || ks == XK_Up) {
                        setLevel(m_focusedOverlay, level + 1.0f);
                        repaintPanel = true;
                    } 
                    break;
                }

                case ClientMessage:
                    if (ev.xclient.window == m_panel.win &&
                        (Atom)ev.xclient.data.l[0] == m_wmDelete)
                        g_quit = 1;   // closing the panel quits: nothing else can
                    break;

                default:
                    break;
            }
        }
    }

    void mainLoop()
    {
        // Every swapchain image has to be filled at least once, plus a spare.
        std::vector<int> framesToRender(m_overlays.size());
        for (size_t i = 0; i < m_overlays.size(); ++i)
            framesToRender[i] = fullRepaint(m_overlays[i]);
        std::vector<bool> needRecreate(m_overlays.size(), false);
        bool repaintPanel = m_panel.win != 0;

        while (!g_quit) {
            // 1. Drain X events. We only ever receive events for our own
            //    windows; other clients are untouched.
            handleXEvents(framesToRender, needRecreate, repaintPanel);

            if (g_quit) break;

            // 2. Rebuild any swapchain whose geometry changed.
            bool anyRecreated = false;
            for (size_t i = 0; i < m_overlays.size(); ++i) {
                if (needRecreate[i]) {
                    needRecreate[i] = false;
                    recreateSwapchain(m_overlays[i]);
                    framesToRender[i] = fullRepaint(m_overlays[i]);
                    anyRecreated = true;
                }
            }
            if (anyRecreated) continue;

            // 3. Any overlay whose level changed needs re-rendering; a full
            //    repaint avoids a stale value lingering in another buffer.
            for (size_t i = 0; i < m_overlays.size(); ++i) {
                if (m_overlays[i].dirty) {
                    m_overlays[i].dirty = false;
                    framesToRender[i] = std::max(framesToRender[i], fullRepaint(m_overlays[i]));
                }
            }

            if (repaintPanel) {
                repaintPanel = false;
                drawPanel();
            }

            // 4. Render only when something actually changed.
            bool renderedAny = false;
            for (size_t i = 0; i < m_overlays.size(); ++i) {
                Overlay& ov = m_overlays[i];
                if (framesToRender[i] > 0 && ov.swapchainUsable) {
                    if (!drawFrame(ov)) needRecreate[i] = true;
                    else                --framesToRender[i];
                    renderedAny = true;
                }
            }
            if (renderedAny) continue;

            // 5. Nothing to do: sleep until X or a signal wakes us. This keeps
            //    the overlay at ~0% CPU while it sits on screen.
            XFlush(m_dpy);
            struct pollfd fds[2];
            fds[0].fd = ConnectionNumber(m_dpy); fds[0].events = POLLIN; fds[0].revents = 0;
            fds[1].fd = g_sigPipe[0];            fds[1].events = POLLIN; fds[1].revents = 0;

            bool anyUnusable = std::any_of(m_overlays.begin(), m_overlays.end(),
                                           [](const Overlay& o) { return !o.swapchainUsable; });
            int timeout = anyUnusable ? 200 : -1;
            int n = poll(fds, 2, timeout);
            if (n < 0 && errno != EINTR) break;

            if (fds[1].revents & POLLIN) {
                char buf[64];
                while (read(g_sigPipe[0], buf, sizeof(buf)) > 0) {}
            }
            if (anyUnusable)
                for (size_t i = 0; i < m_overlays.size(); ++i)
                    if (!m_overlays[i].swapchainUsable) needRecreate[i] = true;
        }

        if (m_device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(m_device);
    }

    // ---- Teardown ---------------------------------------------------------

    void cleanup()
    {
        for (Overlay& ov : m_overlays) {
            destroySwapchain(ov);
            if (ov.imageAvailable) vkDestroySemaphore(m_device, ov.imageAvailable, nullptr);
            if (ov.renderFinished) vkDestroySemaphore(m_device, ov.renderFinished, nullptr);
            if (ov.inFlight)       vkDestroyFence(m_device, ov.inFlight, nullptr);
            if (ov.surface)        vkDestroySurfaceKHR(m_instance, ov.surface, nullptr);
        }

        if (m_cmdPool)  vkDestroyCommandPool(m_device, m_cmdPool, nullptr);
        if (m_device)   vkDestroyDevice(m_device, nullptr);
        if (m_instance) vkDestroyInstance(m_instance, nullptr);

        if (m_dpy) {
            if (m_panel.pixmap) XFreePixmap(m_dpy, m_panel.pixmap);
            if (m_panel.font)   XFreeFont(m_dpy, m_panel.font);
            if (m_panel.gc)     XFreeGC(m_dpy, m_panel.gc);
            if (m_panel.win)    XDestroyWindow(m_dpy, m_panel.win);
            for (Overlay& ov : m_overlays)
                if (ov.win) XDestroyWindow(m_dpy, ov.win);
            if (m_colormap) XFreeColormap(m_dpy, m_colormap);
            XFlush(m_dpy);
        }
    }

    // ---- State ------------------------------------------------------------

    struct Panel {
        Window       win           = 0;
        Pixmap       pixmap        = 0;
        GC           gc            = nullptr;
        XFontStruct* font          = nullptr;
        int          width         = 460;
        int          height        = 100;   // recomputed once overlay count is known
        int          draggingIndex = -1;    // slider index being dragged, or -1
    };

    Options    m_opt;
    Panel      m_panel;
    PanelTheme m_theme;
    Atom       m_wmDelete = None;

    float m_tintR = 0.0f, m_tintG = 0.0f, m_tintB = 0.0f;   // tint is shared across every screen

    Display* m_dpy      = nullptr;
    int      m_screen   = 0;
    Colormap m_colormap = 0;   // shared by every overlay window
    Visual*  m_visual   = nullptr;
    std::vector<Overlay> m_overlays;
    int m_focusedOverlay = 0;   // which slider arrow keys / signals act on

    VkInstance       m_instance    = VK_NULL_HANDLE;
    VkPhysicalDevice m_phys        = VK_NULL_HANDLE;
    VkDevice         m_device      = VK_NULL_HANDLE;
    uint32_t         m_queueFamily = 0;
    VkQueue          m_queue       = VK_NULL_HANDLE;
    VkCommandPool    m_cmdPool     = VK_NULL_HANDLE;
};

// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    Options opt;
    if (argc > 1)
        opt.level = atof(argv[1]) * 100;

    // Xlib is used from one thread only, but XInitThreads() is harmless and
    // protects against drivers that spin up helper threads.
    XInitThreads();

    Dimmer app(opt);
    app.run();
    return 0;
}
