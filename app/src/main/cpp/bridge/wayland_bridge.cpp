#include <jni.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/input.h>

#include <vector>
#include <algorithm>

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include "xdg-shell-server-protocol.h"
#include "xkb-keymap.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_SEAL_SEAL   0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW   0x0004
#endif

#define TAG "WaylandBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ------------------- ENUMS & STRUCTURES -------------------
enum SurfaceRole {
    ROLE_NONE,
    ROLE_TOPLEVEL,
    ROLE_POPUP,
    ROLE_CURSOR
};

struct SurfaceData {
    struct wl_resource* surface_resource = nullptr;
    struct wl_resource* current_buffer   = nullptr;
    struct wl_resource* pending_buffer   = nullptr;
    bool buffer_attached                 = false;

    struct wl_resource* xdg_surface      = nullptr;
    struct wl_resource* role_resource    = nullptr;
    SurfaceRole role = ROLE_NONE;

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool has_parent = false;

    // Buffer destroy listener to prevent dangling pointers
    struct wl_listener current_buffer_destroy_listener;
    bool listening_to_buffer = false;

    std::vector<struct wl_resource*> frame_callbacks;
};

// ------------------- GLOBAL STATE & LOCKING -------------------
static pthread_mutex_t g_waylandMutex;
static pthread_mutex_t g_windowMutex = PTHREAD_MUTEX_INITIALIZER;

static struct wl_display*    g_display          = nullptr;
static struct wl_event_loop* g_eventLoop        = nullptr;
static ANativeWindow*        g_nativeWindow     = nullptr;

static std::vector<SurfaceData*> g_activeSurfaces;

static struct wl_resource*   g_seatResource     = nullptr;
static struct wl_resource*   g_focusedSurface   = nullptr;
static struct wl_resource* g_pointerFocusedSurface = nullptr;

struct KeyboardData {
    struct wl_resource* resource = nullptr;
    struct wl_resource* focused_surface = nullptr;
};

struct PointerData {
    struct wl_resource* resource = nullptr;
    struct wl_resource* focused_surface = nullptr;
    bool button_pressed = false;
};

struct TouchData {
    struct wl_resource* resource = nullptr;
    struct wl_resource* surface = nullptr;
    std::vector<int32_t> active_touch_ids;
};

static std::vector<TouchData*> g_touches;

static TouchData* find_touch_for_client(struct wl_client* client) {
    if (!client) return nullptr;
    for (TouchData* td : g_touches) {
        if (wl_resource_get_client(td->resource) == client) return td;
    }
    return nullptr;
}

static std::vector<KeyboardData*> g_keyboards;
static std::vector<PointerData*>  g_pointers;

static PointerData* find_pointer_for_client(struct wl_client* client) {
    if (!client) return nullptr;
    for (PointerData* pd : g_pointers) {
        if (wl_resource_get_client(pd->resource) == client) return pd;
    }
    return nullptr;
}

static KeyboardData* find_keyboard_for_client(struct wl_client* client) {
    if (!client) return nullptr;
    for (KeyboardData* kd : g_keyboards) {
        if (wl_resource_get_client(kd->resource) == client) return kd;
    }
    return nullptr;
}

static int      g_screenWidth   = 1080;
static int      g_screenHeight  = 1920;
static float    g_scaleFactor   = 1.0f;
static uint32_t g_modsDepressed = 0;

static uint32_t get_current_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void android_wayland_log_handler(const char *fmt, va_list argp) {
    __android_log_vprint(ANDROID_LOG_ERROR, "WaylandServer", fmt, argp);
}

// ------------------- JNI LIFECYCLE & ACTIVITY LAUNCHER -------------------
static JavaVM*  g_jvm                     = nullptr;
static jobject  g_bridgeListenerObject    = nullptr;
static jmethodID g_onClientLaunchMethod   = nullptr;
static jmethodID g_onClientExitMethod     = nullptr;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

static void notify_java_client_launched() {
    if (!g_jvm || !g_bridgeListenerObject || !g_onClientLaunchMethod) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }

    if (env) {
        env->CallVoidMethod(g_bridgeListenerObject, g_onClientLaunchMethod);
    }

    if (attached) {
        g_jvm->DetachCurrentThread();
    }
}

static void notify_java_client_exited() {
    if (!g_jvm || !g_bridgeListenerObject || !g_onClientExitMethod) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }

    if (env) {
        env->CallVoidMethod(g_bridgeListenerObject, g_onClientExitMethod);
    }

    if (attached) {
        g_jvm->DetachCurrentThread();
    }
}

// ------------------- FRAME CALLBACK HELPER -------------------
static void fire_frame_callbacks_locked(SurfaceData* sdata) {
    if (!sdata || sdata->frame_callbacks.empty()) return;

    uint32_t now = get_current_time_ms();
    for (struct wl_resource* cb : sdata->frame_callbacks) {
        wl_resource_set_destructor(cb, nullptr);
        wl_callback_send_done(cb, now);
        wl_resource_destroy(cb);
    }
    sdata->frame_callbacks.clear();
}

#include <EGL/egl.h>
#include <GLES3/gl3.h>

// ------------------- GLES HARDWARE RENDERER STATE -------------------
static EGLDisplay g_eglDisplay = EGL_NO_DISPLAY;
static EGLContext g_eglContext = EGL_NO_CONTEXT;
static EGLSurface g_eglSurface = EGL_NO_SURFACE;
static EGLConfig  g_eglConfig  = nullptr;

static GLuint g_glProgram   = 0;
static GLuint g_glTexture   = 0;
static GLint  g_uTextureLoc = -1;
static int    g_texWidth    = 0;
static int    g_texHeight   = 0;

static const char* VERTEX_SHADER_SRC =
        "#version 300 es\n"
        "layout(location = 0) in vec2 a_Position;\n"
        "layout(location = 1) in vec2 a_TexCoord;\n"
        "out vec2 v_TexCoord;\n"
        "void main() {\n"
        "    gl_Position = vec4(a_Position, 0.0, 1.0);\n"
        "    v_TexCoord  = a_TexCoord;\n"
        "}\n";

static const char* FRAGMENT_SHADER_SRC =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 v_TexCoord;\n"
        "uniform sampler2D u_Texture;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    // Sample Wayland ARGB [B, G, R, A] and hardware-swizzle to RGBA!\n"
        "    fragColor = texture(u_Texture, v_TexCoord).bgra;\n"
        "}\n";

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("Shader compile error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool init_gles_locked() {
    if (g_eglDisplay != EGL_NO_DISPLAY) return true;

    g_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_eglDisplay == EGL_NO_DISPLAY) return false;

    EGLint major, minor;
    if (!eglInitialize(g_eglDisplay, &major, &minor)) return false;

    const EGLint attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_BLUE_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_RED_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE
    };

    EGLint numConfigs;
    if (!eglChooseConfig(g_eglDisplay, attribs, &g_eglConfig, 1, &numConfigs) || numConfigs <= 0) {
        return false;
    }

    const EGLint ctxAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 3,
            EGL_NONE
    };
    g_eglContext = eglCreateContext(g_eglDisplay, g_eglConfig, EGL_NO_CONTEXT, ctxAttribs);
    if (g_eglContext == EGL_NO_CONTEXT) return false;

    return true;
}

static void init_gl_resources_locked() {
    if (g_glProgram != 0) return;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SHADER_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER_SRC);
    g_glProgram = glCreateProgram();
    glAttachShader(g_glProgram, vs);
    glAttachShader(g_glProgram, fs);
    glLinkProgram(g_glProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    g_uTextureLoc = glGetUniformLocation(g_glProgram, "u_Texture");

    glGenTextures(1, &g_glTexture);
    glBindTexture(GL_TEXTURE_2D, g_glTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    g_texWidth = 0;
    g_texHeight = 0;
}

// Fullscreen quad coordinates
static const float QUAD_VERTICES[] = {
        //  x,     y,    u,   v
        -1.0f,  1.0f, 0.0f, 0.0f, // Top-Left
        -1.0f, -1.0f, 0.0f, 1.0f, // Bottom-Left
        1.0f,  1.0f, 1.0f, 0.0f, // Top-Right
        1.0f, -1.0f, 1.0f, 1.0f  // Bottom-Right
};

// ------------------- COMPOSITING ENGINE -------------------

static void composite_and_post_locked() {
    if (!g_nativeWindow || g_eglDisplay == EGL_NO_DISPLAY) return;

    // Create EGL window surface if not already created
    if (g_eglSurface == EGL_NO_SURFACE) {
        g_eglSurface = eglCreateWindowSurface(g_eglDisplay, g_eglConfig, g_nativeWindow, nullptr);
        if (g_eglSurface == EGL_NO_SURFACE) return;
        eglMakeCurrent(g_eglDisplay, g_eglSurface, g_eglSurface, g_eglContext);
        init_gl_resources_locked();
    }

    // Find active toplevel buffer
    SurfaceData* toplevel = nullptr;
    for (auto it = g_activeSurfaces.rbegin(); it != g_activeSurfaces.rend(); ++it) {
        SurfaceData* s = *it;
        if (s && s->role == ROLE_TOPLEVEL && s->current_buffer) {
            toplevel = s;
            break;
        }
    }

    // If there are no surfaces, clear display to black
    if (!toplevel || !toplevel->current_buffer) {
        if (g_eglDisplay != EGL_NO_DISPLAY && g_eglSurface != EGL_NO_SURFACE) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(g_eglDisplay, g_eglSurface);
        }
        return;
    }

    struct wl_shm_buffer *shm_buf = wl_shm_buffer_get(toplevel->current_buffer);
    if (!shm_buf) return;

    int32_t width  = wl_shm_buffer_get_width(shm_buf);
    int32_t height = wl_shm_buffer_get_height(shm_buf);
    int32_t stride = wl_shm_buffer_get_stride(shm_buf);
    if (width <= 0 || height <= 0) return;

    glViewport(0, 0, g_screenWidth, g_screenHeight);

    wl_shm_buffer_begin_access(shm_buf);
    void* pixels = wl_shm_buffer_get_data(shm_buf);

    glBindTexture(GL_TEXTURE_2D, g_glTexture);

    // GLES 3 allows arbitrary row stride without CPU pitch padding
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);

    if (g_texWidth != width || g_texHeight != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        g_texWidth = width;
        g_texHeight = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    wl_shm_buffer_end_access(shm_buf);

    // Draw textured quad
    glUseProgram(g_glProgram);
    glUniform1i(g_uTextureLoc, 0);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), QUAD_VERTICES);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), &QUAD_VERTICES[2]);
    glEnableVertexAttribArray(1);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Post frame to Android SurfaceFlinger hardware composer
    eglSwapBuffers(g_eglDisplay, g_eglSurface);
}


// ------------------- SURFACE LIFECYCLE -------------------

static void on_surface_destroyed(struct wl_resource *resource) {
    pthread_mutex_lock(&g_waylandMutex);

    // 1. Clear focus pointers referencing this surface
    for (KeyboardData* kd : g_keyboards) {
        if (kd->focused_surface == resource) kd->focused_surface = nullptr;
    }
    for (PointerData* pd : g_pointers) {
        if (pd->focused_surface == resource) {
            pd->focused_surface = nullptr;
            pd->button_pressed = false;
        }
    }
    for (TouchData* td : g_touches) {
        if (td->surface == resource) {
            td->surface = nullptr;
            td->active_touch_ids.clear();
        }
    }

    SurfaceData* sdata = (SurfaceData*) wl_resource_get_user_data(resource);
    if (sdata) {
        // 2. Disconnect child resources to prevent use-after-free
        if (sdata->role_resource) {
            wl_resource_set_user_data(sdata->role_resource, nullptr);
            wl_resource_set_destructor(sdata->role_resource, nullptr);
            sdata->role_resource = nullptr;
        }
        if (sdata->xdg_surface) {
            wl_resource_set_user_data(sdata->xdg_surface, nullptr);
            wl_resource_set_destructor(sdata->xdg_surface, nullptr);
            sdata->xdg_surface = nullptr;
        }

        // 3. Unlink buffer destroy listener safely
        if (sdata->listening_to_buffer) {
            wl_list_remove(&sdata->current_buffer_destroy_listener.link);
            sdata->listening_to_buffer = false;
        }
        sdata->current_buffer = nullptr;
        sdata->pending_buffer = nullptr;

        // 4. Do NOT call wl_resource_destroy() here! Just unhook callbacks;
        // libwayland destroys client resources automatically on exit.
        for (struct wl_resource* cb : sdata->frame_callbacks) {
            wl_resource_set_user_data(cb, nullptr);
            wl_resource_set_destructor(cb, nullptr);
        }
        sdata->frame_callbacks.clear();

        // 5. Remove from active surfaces
        auto it = std::find(g_activeSurfaces.begin(), g_activeSurfaces.end(), sdata);
        if (it != g_activeSurfaces.end()) {
            g_activeSurfaces.erase(it);
        }
        delete sdata;
    }

    if (g_activeSurfaces.empty()) {
        notify_java_client_exited();
    }

    pthread_mutex_unlock(&g_waylandMutex);
}

static void surface_attach(struct wl_client*, struct wl_resource *resource, struct wl_resource *buffer, int32_t, int32_t) {
    SurfaceData* sdata = (SurfaceData*) wl_resource_get_user_data(resource);
    if (!sdata) return;
    sdata->pending_buffer = buffer;
    sdata->buffer_attached = true;
}

static void surface_commit(struct wl_client*, struct wl_resource *resource) {
    SurfaceData* sdata = (SurfaceData*) wl_resource_get_user_data(resource);
    if (!sdata) return;

    struct wl_resource* buffer_to_release = nullptr;

    if (sdata->buffer_attached) {
        sdata->buffer_attached = false;

        // If attaching a different buffer, release the previous one
        if (sdata->current_buffer && sdata->current_buffer != sdata->pending_buffer) {
            buffer_to_release = sdata->current_buffer;
        }

        // Unlink destroy listener from previous buffer
        if (sdata->listening_to_buffer) {
            wl_list_remove(&sdata->current_buffer_destroy_listener.link);
            sdata->listening_to_buffer = false;
        }

        sdata->current_buffer = sdata->pending_buffer;
        sdata->pending_buffer = nullptr;

        // Listen for client-side destruction of the current buffer
        if (sdata->current_buffer) {
            sdata->current_buffer_destroy_listener.notify = [](struct wl_listener *l, void*) {
                SurfaceData* s = wl_container_of(l, s, current_buffer_destroy_listener);
                s->current_buffer = nullptr;
                s->listening_to_buffer = false;
            };
            wl_resource_add_destroy_listener(sdata->current_buffer, &sdata->current_buffer_destroy_listener);
            sdata->listening_to_buffer = true;
        }
    }

    // Render new frame
    if (sdata->role == ROLE_TOPLEVEL || sdata->role == ROLE_POPUP) {
        pthread_mutex_lock(&g_windowMutex);
        composite_and_post_locked();
        pthread_mutex_unlock(&g_windowMutex);
    }

    // Safely notify Sway that the previous buffer can be reused
    if (buffer_to_release) {
        wl_buffer_send_release(buffer_to_release);
    }

    // Dispatch frame callbacks (VSync done)
    fire_frame_callbacks_locked(sdata);

    wl_display_flush_clients(g_display);
}

static void on_frame_callback_destroyed(struct wl_resource *resource) {
    SurfaceData* sdata = (SurfaceData*) wl_resource_get_user_data(resource);
    if (!sdata) return;

    auto it = std::find(sdata->frame_callbacks.begin(), sdata->frame_callbacks.end(), resource);
    if (it != sdata->frame_callbacks.end()) {
        sdata->frame_callbacks.erase(it);
    }
}

static const struct wl_surface_interface surface_interface = {
        .destroy              = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
        .attach               = surface_attach,
        .damage               = [](struct wl_client*, struct wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
        .frame = [](struct wl_client* c, struct wl_resource* resource, uint32_t id) {
            SurfaceData* sdata = (SurfaceData*) wl_resource_get_user_data(resource);
            struct wl_resource* cb = wl_resource_create(c, &wl_callback_interface, 1, id);
            if (!cb) return;

            if (sdata) {
                sdata->frame_callbacks.push_back(cb);
                wl_resource_set_implementation(cb, nullptr, sdata, on_frame_callback_destroyed);
            } else {
                wl_callback_send_done(cb, get_current_time_ms());
                wl_resource_destroy(cb);
            }
        },
        .set_opaque_region    = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
        .set_input_region     = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
        .commit               = surface_commit,
        .set_buffer_transform = [](struct wl_client*, struct wl_resource*, int32_t) {},
        .set_buffer_scale     = [](struct wl_client*, struct wl_resource*, int32_t) {},
        .damage_buffer        = [](struct wl_client*, struct wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
};

static void compositor_create_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    uint32_t version = wl_resource_get_version(resource);
    struct wl_resource* surf = wl_resource_create(client, &wl_surface_interface, version < 4 ? version : 4, id);

    SurfaceData* sdata = new SurfaceData();
    sdata->surface_resource = surf;

    pthread_mutex_lock(&g_waylandMutex);
    g_activeSurfaces.push_back(sdata);
    pthread_mutex_unlock(&g_waylandMutex);

    wl_resource_set_implementation(surf, &surface_interface, sdata, on_surface_destroyed);
}

static const struct wl_region_interface region_implementation = {
        .destroy  = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
        .add      = [](struct wl_client*, struct wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
        .subtract = [](struct wl_client*, struct wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
};

static void compositor_create_region(struct wl_client *client, struct wl_resource*, uint32_t id) {
    struct wl_resource *region = wl_resource_create(client, &wl_region_interface, 1, id);
    wl_resource_set_implementation(region, &region_implementation, nullptr, nullptr);
}

static const struct wl_compositor_interface compositor_interface = {
        .create_surface = compositor_create_surface,
        .create_region  = compositor_create_region,
};

static void bind_compositor(struct wl_client *client, void*, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface, version < 4 ? version : 4, id);
    wl_resource_set_implementation(resource, &compositor_interface, nullptr, nullptr);
}

// ------------------- XDG_WM_BASE (TOPLEVEL) -------------------
static void send_keyboard_focus_to(struct wl_resource* surface);

static const struct xdg_toplevel_interface toplevel_implementation = {
        .destroy          = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
        .set_parent       = [](struct wl_client*, struct wl_resource* r, struct wl_resource* parent) {
            SurfaceData* sdata = (SurfaceData*) wl_resource_get_user_data(r);
            if (sdata) sdata->has_parent = (parent != nullptr);
        },
        .set_title        = [](struct wl_client*, struct wl_resource*, const char*) {},
        .set_app_id       = [](struct wl_client*, struct wl_resource*, const char*) {},
        .show_window_menu = [](struct wl_client*, struct wl_resource*, struct wl_resource*, uint32_t, int32_t, int32_t) {},
        .move             = [](struct wl_client*, struct wl_resource*, struct wl_resource*, uint32_t) {},
        .resize           = [](struct wl_client*, struct wl_resource*, struct wl_resource*, uint32_t, uint32_t) {},
        .set_max_size     = [](struct wl_client*, struct wl_resource*, int32_t, int32_t) {},
        .set_min_size     = [](struct wl_client*, struct wl_resource*, int32_t, int32_t) {},
        .set_maximized    = [](struct wl_client*, struct wl_resource*) {},
        .unset_maximized  = [](struct wl_client*, struct wl_resource*) {},
        .set_fullscreen   = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
        .unset_fullscreen = [](struct wl_client*, struct wl_resource*) {},
        .set_minimized    = [](struct wl_client*, struct wl_resource*) {},
};

static void on_toplevel_destroyed(struct wl_resource *resource) {
    pthread_mutex_lock(&g_waylandMutex);
    SurfaceData* sdata = (SurfaceData*) wl_resource_get_user_data(resource);
    if (sdata) {
        sdata->role_resource = nullptr;
        sdata->role = ROLE_NONE;
    }
    pthread_mutex_unlock(&g_waylandMutex);
}

static void xdg_surface_get_toplevel(struct wl_client* client, struct wl_resource* resource, uint32_t id) {
    SurfaceData* sdata = (SurfaceData*) wl_resource_get_user_data(resource);

    uint32_t version = wl_resource_get_version(resource);
    struct wl_resource* toplevel = wl_resource_create(client, &xdg_toplevel_interface, version, id);
    wl_resource_set_implementation(toplevel, &toplevel_implementation, sdata, on_toplevel_destroyed);

    if (sdata) {
        sdata->role = ROLE_TOPLEVEL;
        sdata->role_resource = toplevel;
    }

    int logicalWidth  = (int)(g_screenWidth / g_scaleFactor);
    int logicalHeight = (int)(g_screenHeight / g_scaleFactor);

    struct wl_array states;
    wl_array_init(&states);

    // Provide ACTIVATED and MAXIMIZED states to activate Sway immediately
    uint32_t *state1 = (uint32_t*) wl_array_add(&states, sizeof(uint32_t));
    *state1 = XDG_TOPLEVEL_STATE_ACTIVATED;
    uint32_t *state2 = (uint32_t*) wl_array_add(&states, sizeof(uint32_t));
    *state2 = XDG_TOPLEVEL_STATE_MAXIMIZED;

    xdg_toplevel_send_configure(toplevel, logicalWidth, logicalHeight, &states);
    wl_array_release(&states);

    uint32_t serial = wl_display_next_serial(g_display);
    xdg_surface_send_configure(resource, serial);
    wl_display_flush_clients(g_display);

    if (sdata) {
        send_keyboard_focus_to(sdata->surface_resource);
    }

    notify_java_client_launched();
}

struct PositionerData {
    int32_t width = 200;
    int32_t height = 200;
    int32_t anchor_x = 0;
    int32_t anchor_y = 0;
    int32_t offset_x = 0;
    int32_t offset_y = 0;
};

static const struct xdg_positioner_interface positioner_implementation = {
        .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
        .set_size = [](struct wl_client*, struct wl_resource* r, int32_t w, int32_t h) {
            PositionerData* p = (PositionerData*) wl_resource_get_user_data(r);
            if (p) { p->width = w; p->height = h; }
        },
        .set_anchor_rect = [](struct wl_client*, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h) {
            PositionerData* p = (PositionerData*) wl_resource_get_user_data(r);
            if (p) { p->anchor_x = x; p->anchor_y = y + h; }
        },
        .set_anchor = [](struct wl_client*, struct wl_resource*, uint32_t) {},
        .set_gravity = [](struct wl_client*, struct wl_resource*, uint32_t) {},
        .set_constraint_adjustment = [](struct wl_client*, struct wl_resource*, uint32_t) {},
        .set_offset = [](struct wl_client*, struct wl_resource* r, int32_t x, int32_t y) {
            PositionerData* p = (PositionerData*) wl_resource_get_user_data(r);
            if (p) { p->offset_x = x; p->offset_y = y; }
        },
        .set_reactive = [](struct wl_client*, struct wl_resource*) {},
        .set_parent_size = [](struct wl_client*, struct wl_resource*, int32_t, int32_t) {},
        .set_parent_configure = [](struct wl_client*, struct wl_resource*, uint32_t) {},
};

static const struct xdg_popup_interface popup_implementation = {
        .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
        .grab = [](struct wl_client*, struct wl_resource*, struct wl_resource*, uint32_t) {},
        .reposition = [](struct wl_client*, struct wl_resource* r, struct wl_resource*, uint32_t token) {
            if (wl_resource_get_version(r) >= 3) {
                xdg_popup_send_repositioned(r, token);
            }
        },
};

static void on_popup_destroyed(struct wl_resource *resource) {
    pthread_mutex_lock(&g_waylandMutex);
    SurfaceData* sdata = (SurfaceData*) wl_resource_get_user_data(resource);
    if (sdata) {
        sdata->role_resource = nullptr;
        sdata->role = ROLE_NONE;
    }
    pthread_mutex_unlock(&g_waylandMutex);
}

static void xdg_surface_get_popup(struct wl_client* client, struct wl_resource* resource, uint32_t id,
                                  struct wl_resource* parent, struct wl_resource* positioner) {
    SurfaceData* sdata = (SurfaceData*) wl_resource_get_user_data(resource);

    uint32_t version = wl_resource_get_version(resource);
    struct wl_resource* popup = wl_resource_create(client, &xdg_popup_interface, version, id);
    wl_resource_set_implementation(popup, &popup_implementation, sdata, on_popup_destroyed);

    int pop_x = 0;
    int pop_y = 0;
    int pop_w = 200;
    int pop_h = 200;

    PositionerData* pdata = positioner ? (PositionerData*) wl_resource_get_user_data(positioner) : nullptr;
    if (pdata) {
        pop_x = pdata->anchor_x + pdata->offset_x;
        pop_y = pdata->anchor_y + pdata->offset_y;
        pop_w = pdata->width > 0 ? pdata->width : 200;
        pop_h = pdata->height > 0 ? pdata->height : 200;
    }

    SurfaceData* parent_sdata = parent ? (SurfaceData*) wl_resource_get_user_data(parent) : nullptr;
    int parent_x = parent_sdata ? parent_sdata->x : 0;
    int parent_y = parent_sdata ? parent_sdata->y : 0;

    if (sdata) {
        sdata->role = ROLE_POPUP;
        sdata->role_resource = popup;
        sdata->has_parent = true;
        sdata->x = parent_x + pop_x;
        sdata->y = parent_y + pop_y;
        sdata->width  = pop_w;
        sdata->height = pop_h;
    }

    xdg_popup_send_configure(popup, pop_x, pop_y, pop_w, pop_h);
    uint32_t serial = wl_display_next_serial(g_display);
    xdg_surface_send_configure(resource, serial);
    wl_display_flush_clients(g_display);
}

static const struct xdg_surface_interface xdg_surface_implementation = {
        .destroy             = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
        .get_toplevel        = xdg_surface_get_toplevel,
        .get_popup           = xdg_surface_get_popup, // Hooked here!
        .set_window_geometry = [](struct wl_client*, struct wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
        .ack_configure       = [](struct wl_client*, struct wl_resource*, uint32_t) {},
};

static void xdg_wm_base_get_xdg_surface(struct wl_client* client, struct wl_resource* wm_base, uint32_t id, struct wl_resource* surface) {
    SurfaceData* sdata = (SurfaceData*) wl_resource_get_user_data(surface);
    uint32_t version = wl_resource_get_version(wm_base);
    struct wl_resource* xdg_surf = wl_resource_create(client, &xdg_surface_interface, version, id);
    wl_resource_set_implementation(xdg_surf, &xdg_surface_implementation, sdata, nullptr);

    if (sdata) {
        sdata->xdg_surface = xdg_surf;
    }
}

static const struct xdg_wm_base_interface xdg_wm_base_implementation = {
        .destroy           = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
        .create_positioner = [](struct wl_client* c, struct wl_resource* wm_base, uint32_t id) {
            uint32_t version = wl_resource_get_version(wm_base);
            struct wl_resource* pos = wl_resource_create(c, &xdg_positioner_interface, version, id);
            PositionerData* pdata = new PositionerData();
            wl_resource_set_implementation(pos, &positioner_implementation, pdata, [](struct wl_resource* r) {
                delete (PositionerData*) wl_resource_get_user_data(r);
            });
        },
        .get_xdg_surface   = xdg_wm_base_get_xdg_surface,
        .pong              = [](struct wl_client*, struct wl_resource*, uint32_t) {},
};

static void bind_xdg_wm_base(struct wl_client *client, void*, uint32_t version, uint32_t id) {
    struct wl_resource* resource = wl_resource_create(client, &xdg_wm_base_interface, version < 3 ? version : 3, id);
    wl_resource_set_implementation(resource, &xdg_wm_base_implementation, nullptr, nullptr);
}

// ------------------- INPUT & SEAT -------------------
static int create_shm_file(size_t size) {
    int fd = -1;

#if defined(__NR_memfd_create)
    fd = (int)syscall(__NR_memfd_create, "xkb-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd >= 0) {
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }
#endif

    // Android SELinux fallback
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir) runtime_dir = "/data/local/tmp";

    char path[512];
    snprintf(path, sizeof(path), "%s/wayland-xkb-XXXXXX", runtime_dir);
    fd = mkstemp(path);
    if (fd >= 0) {
        unlink(path);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }

    return -1;
}

static void send_keymap(struct wl_resource *keyboard) {
    size_t size = strlen(xkb_keymap) + 1;
    int fd = create_shm_file(size);
    if (fd < 0) {
        LOGE("Failed to create keymap shm file");
        return;
    }

    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        LOGE("Failed to mmap keymap shm file: %s", strerror(errno));
        return;
    }

    memcpy(map, xkb_keymap, size);
    munmap(map, size);

#if defined(F_ADD_SEALS)
    // Seal the file AFTER it has been sized and written
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
#endif

    wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    wl_display_flush_clients(g_display);
    close(fd);
}

static void send_keyboard_focus_to(struct wl_resource* surface) {
    if (!surface || !g_display) return;

    struct wl_client* target_client = wl_resource_get_client(surface);
    uint32_t serial = wl_display_next_serial(g_display);

    for (KeyboardData* kd : g_keyboards) {
        struct wl_client* kd_client = wl_resource_get_client(kd->resource);
        if (kd_client == target_client) {
            // Already entered on this surface
            if (kd->focused_surface == surface) continue;

            // Leave old surface ONLY if we actually entered it before
            if (kd->focused_surface != nullptr) {
                wl_keyboard_send_leave(kd->resource, serial, kd->focused_surface);
                kd->focused_surface = nullptr;
            }

            // Send enter
            struct wl_array keys;
            wl_array_init(&keys);
            wl_keyboard_send_enter(kd->resource, serial, surface, &keys);
            wl_array_release(&keys);

            wl_keyboard_send_modifiers(kd->resource, serial, g_modsDepressed, 0, 0, 0);
            kd->focused_surface = surface;
        } else {
            if (kd->focused_surface != nullptr) {
                wl_keyboard_send_leave(kd->resource, serial, kd->focused_surface);
                kd->focused_surface = nullptr;
            }
        }
    }
    wl_display_flush_clients(g_display);
}


static const struct wl_keyboard_interface keyboard_implementation = {
        .release = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

static const struct wl_pointer_interface pointer_implementation = {
        .set_cursor = [](struct wl_client*, struct wl_resource*, uint32_t, struct wl_resource* surface, int32_t, int32_t) {
            if (surface) {
                SurfaceData* sdata = (SurfaceData*) wl_resource_get_user_data(surface);
                if (sdata) sdata->role = ROLE_CURSOR;
            }
        },
        .release    = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

static void seat_get_keyboard(struct wl_client *client, struct wl_resource* seat, uint32_t id) {
    uint32_t version = wl_resource_get_version(seat);
    struct wl_resource* kb = wl_resource_create(client, &wl_keyboard_interface, version < 4 ? version : 4, id);

    KeyboardData* kd = new KeyboardData();
    kd->resource = kb;
    kd->focused_surface = nullptr;
    g_keyboards.push_back(kd);

    wl_resource_set_implementation(kb, &keyboard_implementation, kd, [](struct wl_resource* r) {
        KeyboardData* data = (KeyboardData*) wl_resource_get_user_data(r);
        auto it = std::find(g_keyboards.begin(), g_keyboards.end(), data);
        if (it != g_keyboards.end()) g_keyboards.erase(it);
        delete data;
    });

    send_keymap(kb);
    if (version >= 4) {
        wl_keyboard_send_repeat_info(kb, 25, 600);
    }

    // Immediately focus Sway's active toplevel surface if already created
    for (auto it = g_activeSurfaces.rbegin(); it != g_activeSurfaces.rend(); ++it) {
        if ((*it)->surface_resource && wl_resource_get_client((*it)->surface_resource) == client) {
            send_keyboard_focus_to((*it)->surface_resource);
            break;
        }
    }
}

static void seat_get_pointer(struct wl_client *client, struct wl_resource* seat, uint32_t id) {
    uint32_t version = wl_resource_get_version(seat);
    struct wl_resource* ptr = wl_resource_create(client, &wl_pointer_interface, version < 5 ? version : 5, id);

    PointerData* pd = new PointerData();
    pd->resource = ptr;
    pd->focused_surface = nullptr;
    pd->button_pressed = false;
    g_pointers.push_back(pd);

    wl_resource_set_implementation(ptr, &pointer_implementation, pd, [](struct wl_resource* r) {
        PointerData* data = (PointerData*) wl_resource_get_user_data(r);
        auto it = std::find(g_pointers.begin(), g_pointers.end(), data);
        if (it != g_pointers.end()) g_pointers.erase(it);
        delete data;
    });
}

static const struct wl_touch_interface touch_implementation = {
        .release = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

static void seat_get_touch(struct wl_client *client, struct wl_resource* seat, uint32_t id) {
    uint32_t version = wl_resource_get_version(seat);
    struct wl_resource* tch = wl_resource_create(client, &wl_touch_interface, version < 4 ? version : 4, id);

    TouchData* td = new TouchData();
    td->resource = tch;
    td->surface = nullptr;
    g_touches.push_back(td);

    wl_resource_set_implementation(tch, &touch_implementation, td, [](struct wl_resource* r) {
        TouchData* data = (TouchData*) wl_resource_get_user_data(r);
        auto it = std::find(g_touches.begin(), g_touches.end(), data);
        if (it != g_touches.end()) g_touches.erase(it);
        delete data;
    });
}

static const struct wl_seat_interface seat_implementation = {
        .get_pointer  = seat_get_pointer,
        .get_keyboard = seat_get_keyboard,
        .get_touch    = seat_get_touch,
        .release      = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

static void bind_seat(struct wl_client *client, void*, uint32_t version, uint32_t id) {
    g_seatResource = wl_resource_create(client, &wl_seat_interface, version < 5 ? version : 5, id);
    wl_resource_set_implementation(g_seatResource, &seat_implementation, nullptr, nullptr);

    // Advertise Keyboard + Pointer + Touchscreen
    wl_seat_send_capabilities(g_seatResource,
                              WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_TOUCH);

    if (version >= 2) wl_seat_send_name(g_seatResource, "seat0");
}

// ------------------- OUTPUT & SUBCOMPOSITOR -------------------
static const struct wl_output_interface output_implementation = {
        .release = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

static void bind_output(struct wl_client *client, void*, uint32_t version, uint32_t id) {
    struct wl_resource *output = wl_resource_create(client, &wl_output_interface, version < 3 ? version : 3, id);
    wl_resource_set_implementation(output, &output_implementation, nullptr, nullptr);

    wl_output_send_geometry(output, 0, 0, 70, 150, WL_OUTPUT_SUBPIXEL_UNKNOWN, "Android", "Display", WL_OUTPUT_TRANSFORM_NORMAL);

    int logicalW = (int)(g_screenWidth / g_scaleFactor);
    int logicalH = (int)(g_screenHeight / g_scaleFactor);
    wl_output_send_mode(output, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, logicalW, logicalH, 60000);

    if (version >= 2) wl_output_send_scale(output, 1);
    if (version >= 2) wl_output_send_done(output);
}

static const struct wl_subsurface_interface subsurface_implementation = {
        .destroy      = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
        .set_position = [](struct wl_client*, struct wl_resource*, int32_t, int32_t) {},
        .place_above  = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
        .place_below  = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
        .set_sync     = [](struct wl_client*, struct wl_resource*) {},
        .set_desync   = [](struct wl_client*, struct wl_resource*) {},
};

static const struct wl_subcompositor_interface subcompositor_implementation = {
        .destroy        = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
        .get_subsurface = [](struct wl_client* c, struct wl_resource*, uint32_t id, struct wl_resource*, struct wl_resource*) {
            struct wl_resource* sub = wl_resource_create(c, &wl_subsurface_interface, 1, id);
            wl_resource_set_implementation(sub, &subsurface_implementation, nullptr, nullptr);
        },
};

static void bind_subcompositor(struct wl_client *client, void*, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &wl_subcompositor_interface, version < 1 ? version : 1, id);
    wl_resource_set_implementation(resource, &subcompositor_implementation, nullptr, nullptr);
}

// ------------------- EVENT LOOP THREAD -------------------
static void* wayland_thread(void*) {
    LOGI("Wayland event loop starting...");
    struct wl_event_loop* loop = wl_display_get_event_loop(g_display);
    while (g_display) {
        pthread_mutex_lock(&g_waylandMutex);

        // Dispatch pending I/O events while holding the mutex
        wl_event_loop_dispatch(loop, 0);

        // Flush responses to all clients
        wl_display_flush_clients(g_display);

        pthread_mutex_unlock(&g_waylandMutex);

        // Sleep briefly to yield the lock to Android UI events (sendTouch / sendKey)
        usleep(4000); // 4ms (~250Hz polling loop)
    }
    return nullptr;
}

// ------------------- JNI EXPORTS -------------------

extern "C" JNIEXPORT void JNICALL
Java_com_termux_app_wayland_WaylandBridge_initServer(JNIEnv* env, jclass , jstring jRuntimeDir) {
    if (g_display) return;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_waylandMutex, &attr);
    pthread_mutexattr_destroy(&attr);

    const char* runtimeDir = env->GetStringUTFChars(jRuntimeDir, nullptr);
    setenv("XDG_RUNTIME_DIR", runtimeDir, 1);
    setenv("WAYLAND_DEBUG", "server", 1);
    wl_log_set_handler_server(android_wayland_log_handler);

    g_display = wl_display_create();
    g_eventLoop = wl_display_get_event_loop(g_display);

    char socketPath[512];
    snprintf(socketPath, sizeof(socketPath), "%s/wayland-0", runtimeDir);
    unlink(socketPath);

    int sock_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock_fd >= 0) {
        int bufsize = 1024 * 1024;
        setsockopt(sock_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socketPath, sizeof(addr.sun_path) - 1);

        if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            chmod(socketPath, 0666);
            listen(sock_fd, 128);
            wl_display_add_socket_fd(g_display, sock_fd);
            LOGI("Wayland socket listening on: %s", socketPath);
        } else {
            close(sock_fd);
        }
    }

    wl_display_init_shm(g_display);
    wl_global_create(g_display, &wl_compositor_interface, 4, nullptr, bind_compositor);
    wl_global_create(g_display, &xdg_wm_base_interface, 3, nullptr, bind_xdg_wm_base);
    wl_global_create(g_display, &wl_seat_interface, 5, nullptr, bind_seat);
    wl_global_create(g_display, &wl_subcompositor_interface, 1, nullptr, bind_subcompositor);
    wl_global_create(g_display, &wl_output_interface, 3, nullptr, bind_output);

    pthread_t t;
    pthread_create(&t, nullptr, wayland_thread, nullptr);

    env->ReleaseStringUTFChars(jRuntimeDir, runtimeDir);
}

extern "C" JNIEXPORT void JNICALL
Java_com_termux_app_wayland_WaylandBridge_setSurface(JNIEnv* env, jobject, jobject surface) {
    pthread_mutex_lock(&g_windowMutex);

    if (g_eglDisplay != EGL_NO_DISPLAY && g_eglSurface != EGL_NO_SURFACE) {
        eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(g_eglDisplay, g_eglSurface);
        g_eglSurface = EGL_NO_SURFACE;
    }

    if (g_nativeWindow) {
        ANativeWindow_release(g_nativeWindow);
        g_nativeWindow = nullptr;
    }

    if (surface) {
        g_nativeWindow = ANativeWindow_fromSurface(env, surface);
        init_gles_locked();
    }

    pthread_mutex_unlock(&g_windowMutex);
}

extern "C" JNIEXPORT void JNICALL
Java_com_termux_app_wayland_WaylandBridge_resize(JNIEnv*, jobject, jint width, jint height) {
    if (width < 200 || height < 200) return;

    pthread_mutex_lock(&g_waylandMutex);

    g_screenWidth  = width;
    g_screenHeight = height;

    int logicalWidth  = (int)(width / g_scaleFactor);
    int logicalHeight = (int)(height / g_scaleFactor);

    for (SurfaceData* sdata : g_activeSurfaces) {
        if (sdata && sdata->role == ROLE_TOPLEVEL && sdata->role_resource) {
            struct wl_array states;
            wl_array_init(&states);
            uint32_t *state1 = (uint32_t*) wl_array_add(&states, sizeof(uint32_t));
            *state1 = XDG_TOPLEVEL_STATE_ACTIVATED;
            uint32_t *state2 = (uint32_t*) wl_array_add(&states, sizeof(uint32_t));
            *state2 = XDG_TOPLEVEL_STATE_MAXIMIZED;

            xdg_toplevel_send_configure(sdata->role_resource, logicalWidth, logicalHeight, &states);
            wl_array_release(&states);

            uint32_t serial = wl_display_next_serial(g_display);
            xdg_surface_send_configure(sdata->xdg_surface, serial);
        }
    }
    wl_display_flush_clients(g_display);

    pthread_mutex_unlock(&g_waylandMutex);
}

extern "C" JNIEXPORT void JNICALL
Java_com_termux_app_wayland_WaylandBridge_setScale(JNIEnv* env, jobject thiz, jfloat scale) {
    if (scale < 0.5f) scale = 0.5f;

    pthread_mutex_lock(&g_waylandMutex);
    g_scaleFactor = scale;
    pthread_mutex_unlock(&g_waylandMutex);

    Java_com_termux_app_wayland_WaylandBridge_resize(env, thiz, g_screenWidth, g_screenHeight);
}

extern "C" JNIEXPORT void JNICALL
Java_com_termux_app_wayland_WaylandBridge_focusKeyboard(JNIEnv*, jobject) {
    pthread_mutex_lock(&g_waylandMutex);
    if (!g_activeSurfaces.empty()) {
        send_keyboard_focus_to(g_activeSurfaces.back()->surface_resource);
    }
    pthread_mutex_unlock(&g_waylandMutex);
}

extern "C" JNIEXPORT void JNICALL
Java_com_termux_app_wayland_WaylandBridge_sendKey(JNIEnv*, jobject, jint evdevScanCode, jboolean down) {
    pthread_mutex_lock(&g_waylandMutex);

    if (g_keyboards.empty() || !g_display) {
        pthread_mutex_unlock(&g_waylandMutex);
        return;
    }

    uint32_t time   = get_current_time_ms();
    uint32_t serial = wl_display_next_serial(g_display);

    if (evdevScanCode == KEY_LEFTSHIFT || evdevScanCode == KEY_RIGHTSHIFT) {
        if (down) g_modsDepressed |= 1; else g_modsDepressed &= ~1;
    } else if (evdevScanCode == KEY_LEFTCTRL || evdevScanCode == KEY_RIGHTCTRL) {
        if (down) g_modsDepressed |= 4; else g_modsDepressed &= ~4;
    } else if (evdevScanCode == KEY_LEFTALT || evdevScanCode == KEY_RIGHTALT) {
        if (down) g_modsDepressed |= 8; else g_modsDepressed &= ~8;
    }

    for (KeyboardData* kd : g_keyboards) {
        if (kd->focused_surface != nullptr) {
            wl_keyboard_send_key(kd->resource, serial, time, (uint32_t)evdevScanCode, down ? 1 : 0);
            wl_keyboard_send_modifiers(kd->resource, serial, g_modsDepressed, 0, 0, 0);
        }
    }
    wl_display_flush_clients(g_display);

    pthread_mutex_unlock(&g_waylandMutex);
}

extern "C" JNIEXPORT void JNICALL
Java_com_termux_app_wayland_WaylandBridge_sendTouch(JNIEnv*, jobject, jint id, jfloat x, jfloat y, jint action) {
    pthread_mutex_lock(&g_waylandMutex);

    if (g_activeSurfaces.empty() || !g_display) {
        pthread_mutex_unlock(&g_waylandMutex);
        return;
    }

    float logicalX = x / g_scaleFactor;
    float logicalY = y / g_scaleFactor;

    // 1. Popup dismissal on touch down outside
    if (action == 0 || action == 5) { // ACTION_DOWN or ACTION_POINTER_DOWN
        for (auto it = g_activeSurfaces.rbegin(); it != g_activeSurfaces.rend(); ++it) {
            SurfaceData* s = *it;
            if (s && s->role == ROLE_POPUP && s->role_resource) {
                if (logicalX < s->x || logicalX >= s->x + s->width ||
                    logicalY < s->y || logicalY >= s->y + s->height) {
                    xdg_popup_send_popup_done(s->role_resource);
                }
            }
        }
    }

    // 2. Find target surface
    SurfaceData* hit = nullptr;
    for (auto it = g_activeSurfaces.rbegin(); it != g_activeSurfaces.rend(); ++it) {
        SurfaceData* s = *it;
        if (s->role == ROLE_POPUP) {
            if (logicalX >= s->x && logicalX < s->x + s->width &&
                logicalY >= s->y && logicalY < s->y + s->height) {
                hit = s;
                break;
            }
        } else if (s->role == ROLE_TOPLEVEL) {
            if (!hit) hit = s;
        }
    }
    if (!hit) hit = g_activeSurfaces.front();
    if (!hit || !hit->surface_resource) {
        pthread_mutex_unlock(&g_waylandMutex);
        return;
    }

    struct wl_client* client = wl_resource_get_client(hit->surface_resource);
    TouchData* td = find_touch_for_client(client);

    float surfaceLocalX = logicalX - hit->x;
    float surfaceLocalY = logicalY - hit->y;
    uint32_t time   = get_current_time_ms();
    uint32_t serial = wl_display_next_serial(g_display);

    // ========================================================
    // CASE A: Client supports Native Touchscreen (wl_touch)
    // ========================================================
    if (td) {
        if (action == 0 || action == 5) { // ACTION_DOWN or ACTION_POINTER_DOWN
            td->surface = hit->surface_resource;
            td->active_touch_ids.push_back(id);

            wl_touch_send_down(td->resource, serial, time, hit->surface_resource, id,
                               wl_fixed_from_double(surfaceLocalX), wl_fixed_from_double(surfaceLocalY));
            wl_touch_send_frame(td->resource);

            // Give keyboard focus to the tapped window
            send_keyboard_focus_to(hit->surface_resource);

        } else if (action == 1) { // ACTION_MOVE
            wl_touch_send_motion(td->resource, time, id,
                                 wl_fixed_from_double(surfaceLocalX), wl_fixed_from_double(surfaceLocalY));
            wl_touch_send_frame(td->resource);

        } else if (action == 2 || action == 6) { // ACTION_UP or ACTION_POINTER_UP
            wl_touch_send_up(td->resource, serial, time, id);
            wl_touch_send_frame(td->resource);

            auto it = std::find(td->active_touch_ids.begin(), td->active_touch_ids.end(), id);
            if (it != td->active_touch_ids.end()) td->active_touch_ids.erase(it);
            if (td->active_touch_ids.empty()) td->surface = nullptr;

        } else if (action == 3) { // ACTION_CANCEL
            wl_touch_send_cancel(td->resource);
            td->active_touch_ids.clear();
            td->surface = nullptr;
        }

        wl_display_flush_clients(g_display);
        pthread_mutex_unlock(&g_waylandMutex);
        return;
    }

    // ========================================================
    // CASE B: Fallback to Pointer for clients without wl_touch
    // ========================================================
    PointerData* pd = find_pointer_for_client(client);
    if (pd) {
        uint32_t p_ver = wl_resource_get_version(pd->resource);

        if (pd->focused_surface != hit->surface_resource) {
            if (pd->focused_surface != nullptr) {
                wl_pointer_send_leave(pd->resource, serial, pd->focused_surface);
                if (p_ver >= 5) wl_pointer_send_frame(pd->resource);
            }
            wl_pointer_send_enter(pd->resource, serial, hit->surface_resource,
                                  wl_fixed_from_double(surfaceLocalX), wl_fixed_from_double(surfaceLocalY));
            if (p_ver >= 5) wl_pointer_send_frame(pd->resource);
            pd->focused_surface = hit->surface_resource;
        }

        if (action == 0 || action == 5) {
            wl_pointer_send_motion(pd->resource, time,
                                   wl_fixed_from_double(surfaceLocalX), wl_fixed_from_double(surfaceLocalY));
            wl_pointer_send_button(pd->resource, serial, time, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
            if (p_ver >= 5) wl_pointer_send_frame(pd->resource);
            pd->button_pressed = true;
            send_keyboard_focus_to(hit->surface_resource);

        } else if (action == 1) {
            wl_pointer_send_motion(pd->resource, time,
                                   wl_fixed_from_double(surfaceLocalX), wl_fixed_from_double(surfaceLocalY));
            if (p_ver >= 5) wl_pointer_send_frame(pd->resource);

        } else if (action == 2 || action == 6) {
            if (pd->button_pressed) {
                wl_pointer_send_button(pd->resource, serial, time, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
                if (p_ver >= 5) wl_pointer_send_frame(pd->resource);
                pd->button_pressed = false;
            }
        }
    }

    wl_display_flush_clients(g_display);
    pthread_mutex_unlock(&g_waylandMutex);
}

extern "C" JNIEXPORT void JNICALL
Java_com_termux_app_wayland_WaylandBridge_refresh(JNIEnv*, jobject) {
    pthread_mutex_lock(&g_windowMutex);
    composite_and_post_locked();
    pthread_mutex_unlock(&g_windowMutex);
}

extern "C" JNIEXPORT void JNICALL
Java_com_termux_app_wayland_WaylandBridge_registerActivityListener(JNIEnv* env, jclass , jobject listener) {
    if (g_bridgeListenerObject) {
        env->DeleteGlobalRef(g_bridgeListenerObject);
        g_bridgeListenerObject = nullptr;
    }

    if (listener) {
        g_bridgeListenerObject = env->NewGlobalRef(listener);
        jclass clazz = env->GetObjectClass(listener);
        g_onClientLaunchMethod = env->GetMethodID(clazz, "onClientLaunched", "()V");
        g_onClientExitMethod   = env->GetMethodID(clazz, "onClientExited", "()V");
    }
}
