/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gpu_renderer.hpp"

#include <android/log.h>
#include <android/hardware_buffer.h>
#include <vndk/hardware_buffer.h>
#include <dlfcn.h>
#include <unistd.h>

#include <cstring>

#include "drivers/opengles/glad/include/glad/egl.h"
#include "drivers/opengles/glad/include/glad/gles2.h"
#include "drivers/opengles/lv_opengles_driver.h"
#include "drivers/opengles/lv_opengles_texture.h"
#include "drivers/opengles/lv_opengles_texture_private.h"

namespace aeraui {
namespace {

constexpr char kLogTag[] = "AeraGpu";
constexpr char kEglDriver[] = "/vendor/lib64/egl/libEGL_adreno.so";
constexpr char kGlesDriver[] = "/vendor/lib64/egl/libGLESv2_adreno.so";
constexpr char kNativeWindowLibrary[] = "/vendor/lib64/libnativewindow.so";
constexpr uint32_t Fourcc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
         (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}
constexpr uint32_t kDrmFormatAbgr8888 = Fourcc('A', 'B', '2', '4');
constexpr EGLenum kEglNativeBufferAndroid = 0x3140;

struct SymbolLoader {
  void* library;
  PFNEGLGETPROCADDRESSPROC get_proc_address;
};

GLADapiproc LoadSymbol(void* user_data, const char* name) {
  auto* loader = static_cast<SymbolLoader*>(user_data);
  void* symbol = dlsym(loader->library, name);
  if (symbol != nullptr)
    return reinterpret_cast<GLADapiproc>(symbol);
  if (loader->get_proc_address != nullptr)
    return reinterpret_cast<GLADapiproc>(loader->get_proc_address(name));
  return nullptr;
}

template <typename T>
bool LoadBootstrap(void* library, const char* name, T* function) {
  *function = reinterpret_cast<T>(dlsym(library, name));
  return *function != nullptr;
}

using ImageTargetTexture = void (*)(GLenum target, void* image);
using AllocateHardwareBuffer = int (*)(const AHardwareBuffer_Desc*,
                                       AHardwareBuffer**);
using ReleaseHardwareBuffer = void (*)(AHardwareBuffer*);
using DescribeHardwareBuffer = void (*)(const AHardwareBuffer*,
                                        AHardwareBuffer_Desc*);
using GetHardwareBufferHandle = const native_handle_t* (*)(
    const AHardwareBuffer*);
using ToNativeWindowBuffer = void* (*)(const AHardwareBuffer*);
using ImportScanoutBuffer = int (*)(const GRDrmBufferInfo*, gr_surface*);
using ReleaseScanoutBuffer = int (*)(gr_surface);

bool HasExtension(const char* extensions, const char* wanted) {
  if (extensions == nullptr || wanted == nullptr || *wanted == '\0')
    return false;
  const size_t wanted_length = std::strlen(wanted);
  const char* match = extensions;
  while ((match = std::strstr(match, wanted)) != nullptr) {
    const bool starts_word = match == extensions || match[-1] == ' ';
    const char tail = match[wanted_length];
    if (starts_word && (tail == '\0' || tail == ' '))
      return true;
    match += wanted_length;
  }
  return false;
}

void DisplayResolutionChanged(lv_event_t* event) {
  auto* display = static_cast<lv_display_t*>(lv_event_get_target(event));
  auto* texture = static_cast<lv_opengles_texture_t*>(
      lv_display_get_driver_data(display));
  if (texture == nullptr) return;
  const int32_t width = lv_display_get_horizontal_resolution(display);
  const int32_t height = lv_display_get_vertical_resolution(display);
  if (width <= 0 || height <= 0 ||
      lv_opengles_texture_reshape(texture, display, width, height) !=
          LV_RESULT_OK) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "failed to reshape LVGL texture to %dx%d", width,
                        height);
    return;
  }
  const GLuint texture_id = lv_opengles_texture_get_texture_id(display);
  glBindTexture(GL_TEXTURE_2D, texture_id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  __android_log_print(ANDROID_LOG_INFO, kLogTag,
                      "reshaped LVGL texture to %dx%d", width, height);
}

}  // namespace

GpuRenderer::~GpuRenderer() {
  Shutdown();
}

bool GpuRenderer::Initialize(int32_t width, int32_t height) {
  Shutdown();
  width_ = width;
  height_ = height;

  egl_library_ = dlopen(kEglDriver, RTLD_NOW | RTLD_GLOBAL);
  if (egl_library_ == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "Adreno EGL unavailable: %s", dlerror());
    return false;
  }
  gles_library_ = dlopen(kGlesDriver, RTLD_NOW | RTLD_GLOBAL);
  if (gles_library_ == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "Adreno GLES unavailable: %s", dlerror());
    Shutdown();
    return false;
  }
  nativewindow_library_ = dlopen(kNativeWindowLibrary, RTLD_NOW | RTLD_GLOBAL);
  // Some recovery device trees package the matching native-window runtime in
  // /system as well: inspecting Android's vendor partition temporarily hides
  // the ramdisk copy. Keep the existing vendor runtime as the first choice.
  if (nativewindow_library_ == nullptr) {
    nativewindow_library_ = dlopen("/system/lib64/libnativewindow.so",
                                 RTLD_NOW | RTLD_GLOBAL);
  }
  if (nativewindow_library_ == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "Android native-buffer runtime unavailable: %s",
                        dlerror());
    Shutdown();
    return false;
  }

  PFNEGLGETDISPLAYPROC get_display = nullptr;
  PFNEGLINITIALIZEPROC initialize = nullptr;
  PFNEGLGETPROCADDRESSPROC get_proc_address = nullptr;
  PFNEGLTERMINATEPROC terminate = nullptr;
  if (!LoadBootstrap(egl_library_, "eglGetDisplay", &get_display) ||
      !LoadBootstrap(egl_library_, "eglInitialize", &initialize) ||
      !LoadBootstrap(egl_library_, "eglGetProcAddress", &get_proc_address) ||
      !LoadBootstrap(egl_library_, "eglTerminate", &terminate)) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "Adreno EGL bootstrap is incomplete");
    Shutdown();
    return false;
  }

  EGLDisplay display = get_display(EGL_DEFAULT_DISPLAY);
  EGLint major = 0;
  EGLint minor = 0;
  if (display == EGL_NO_DISPLAY || !initialize(display, &major, &minor)) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "eglInitialize failed");
    Shutdown();
    return false;
  }
  egl_display_ = display;

  SymbolLoader egl_loader{egl_library_, get_proc_address};
  if (!gladLoadEGLUserPtr(display, LoadSymbol, &egl_loader) ||
      eglBindAPI == nullptr || !eglBindAPI(EGL_OPENGL_ES_API)) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "EGL entry point loading failed");
    terminate(display);
    egl_display_ = nullptr;
    Shutdown();
    return false;
  }

  const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_NONE,
  };
  EGLConfig config = nullptr;
  EGLint config_count = 0;
  if (!eglChooseConfig(display, config_attributes, &config, 1, &config_count) ||
      config_count != 1) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "no compatible Adreno pbuffer config");
    Shutdown();
    return false;
  }

  const EGLint surface_attributes[] = {
      EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE,
  };
  const EGLint context_attributes[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE,
  };
  EGLSurface surface =
      eglCreatePbufferSurface(display, config, surface_attributes);
  EGLContext context =
      eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
  if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
      !eglMakeCurrent(display, surface, surface, context)) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "Adreno context creation failed: %#x", eglGetError());
    egl_surface_ = surface;
    egl_context_ = context;
    Shutdown();
    return false;
  }
  egl_surface_ = surface;
  egl_context_ = context;

  SymbolLoader gles_loader{gles_library_, get_proc_address};
  if (!gladLoadGLES2UserPtr(LoadSymbol, &gles_loader)) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "GLES entry point loading failed");
    Shutdown();
    return false;
  }

  const char* egl_extensions = eglQueryString(display, EGL_EXTENSIONS);
  const char* gl_extensions =
      reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  auto image_target = reinterpret_cast<ImageTargetTexture>(
      LoadSymbol(&gles_loader, "glEGLImageTargetTexture2DOES"));
  if (!HasExtension(egl_extensions, "EGL_ANDROID_image_native_buffer") ||
      !HasExtension(gl_extensions, "GL_OES_EGL_image") ||
      eglCreateImageKHR == nullptr || eglDestroyImageKHR == nullptr ||
      image_target == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "Adreno native-buffer image import is unavailable");
    Shutdown();
    return false;
  }

  auto allocate_buffer = reinterpret_cast<AllocateHardwareBuffer>(
      dlsym(nativewindow_library_, "AHardwareBuffer_allocate"));
  auto release_buffer = reinterpret_cast<ReleaseHardwareBuffer>(
      dlsym(nativewindow_library_, "AHardwareBuffer_release"));
  auto describe_buffer = reinterpret_cast<DescribeHardwareBuffer>(
      dlsym(nativewindow_library_, "AHardwareBuffer_describe"));
  auto get_buffer_handle = reinterpret_cast<GetHardwareBufferHandle>(
      dlsym(nativewindow_library_, "AHardwareBuffer_getNativeHandle"));
  auto to_native_window = reinterpret_cast<ToNativeWindowBuffer>(dlsym(
      nativewindow_library_,
      "_ZN7android38AHardwareBuffer_to_ANativeWindowBufferEPK15AHardwareBuffer"));
  auto import_scanout = reinterpret_cast<ImportScanoutBuffer>(
      dlsym(RTLD_DEFAULT, "gr_drm_import_scanout_buffer"));
  if (allocate_buffer == nullptr || release_buffer == nullptr ||
      describe_buffer == nullptr || get_buffer_handle == nullptr ||
      to_native_window == nullptr || import_scanout == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "native-buffer allocation or DRM import API is unavailable");
    Shutdown();
    return false;
  }

  for (unsigned int i = 0; i < 2; ++i) {
    AHardwareBuffer_Desc requested{};
    requested.width = static_cast<uint32_t>(width_);
    requested.height = static_cast<uint32_t>(height_);
    requested.layers = 1;
    requested.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    // CPU access forces a linear allocation. GPU_FRAMEBUFFER makes it
    // renderable and COMPOSER_OVERLAY makes it scanout-capable.
    requested.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                      AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY |
                      AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                      AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    if (allocate_buffer(&requested, &hardware_buffers_[i]) != 0 ||
        hardware_buffers_[i] == nullptr) {
      __android_log_print(ANDROID_LOG_WARN, kLogTag,
                          "could not allocate native scanout %u", i);
      Shutdown();
      return false;
    }

    AHardwareBuffer_Desc actual{};
    describe_buffer(hardware_buffers_[i], &actual);
    const native_handle_t* handle = get_buffer_handle(hardware_buffers_[i]);
    if (handle == nullptr || handle->numFds < 1 || actual.stride == 0) {
      __android_log_print(ANDROID_LOG_WARN, kLogTag,
                          "native scanout %u has no shareable plane", i);
      Shutdown();
      return false;
    }
    const EGLint image_attributes[] = {
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE,
    };
    EGLImageKHR image = eglCreateImageKHR(
        display, EGL_NO_CONTEXT, kEglNativeBufferAndroid,
        reinterpret_cast<EGLClientBuffer>(
            to_native_window(hardware_buffers_[i])),
        image_attributes);
    if (image == EGL_NO_IMAGE_KHR) {
      __android_log_print(ANDROID_LOG_WARN, kLogTag,
                          "native-buffer import %u failed: %#x", i,
                          eglGetError());
      Shutdown();
      return false;
    }
    egl_images_[i] = image;

    // Register the already-imported EGL allocation with KMS. Qualcomm EGL
    // returns EGL_BAD_ACCESS if DRM claims the dma-buf before EGLImage owns it.
    GRDrmBufferInfo info{};
    info.dma_buf_fd = handle->data[0];
    info.width = static_cast<int>(actual.width);
    info.height = static_cast<int>(actual.height);
    info.pitch = static_cast<int>(actual.stride * 4U);
    info.drm_format = kDrmFormatAbgr8888;
    if (import_scanout(&info, &scanouts_[i]) != 0) {
      __android_log_print(ANDROID_LOG_WARN, kLogTag,
                          "DRM rejected native scanout %u (%ux%u stride %u)",
                          i, actual.width, actual.height, actual.stride);
      Shutdown();
      return false;
    }

    glGenTextures(1, &textures_[i]);
    glBindTexture(GL_TEXTURE_2D, textures_[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_target(GL_TEXTURE_2D, image);

    glGenFramebuffers(1, &framebuffers_[i]);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffers_[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, textures_[i], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      __android_log_print(ANDROID_LOG_WARN, kLogTag,
                          "scanout framebuffer %u is not renderable", i);
      Shutdown();
      return false;
    }
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  ready_ = true;
  __android_log_print(
      ANDROID_LOG_INFO, kLogTag,
      "zero-copy native-buffer GPU ready: EGL %d.%d, %s, %s, %dx%d double scanout",
      major, minor, reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
      reinterpret_cast<const char*>(glGetString(GL_RENDERER)), width_, height_);
  return true;
}

lv_display_t* GpuRenderer::CreateDisplay(int32_t logical_width,
                                         int32_t logical_height) {
  if (!ready_ || logical_width <= 0 || logical_height <= 0)
    return nullptr;
  logical_width_ = logical_width;
  logical_height_ = logical_height;
  lv_display_t* display =
      lv_opengles_texture_create(logical_width_, logical_height_);
  driver_initialized_ = display != nullptr;
  if (display != nullptr) {
    const GLuint texture = lv_opengles_texture_get_texture_id(display);
    glBindTexture(GL_TEXTURE_2D, texture);
    // The logical canvas is normally resampled to the native panel. Linear
    // filtering keeps text and curves clean instead of exposing pixel steps.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // The OpenGL texture driver does not resize itself when LVGL swaps the
    // logical dimensions for 90/270-degree rotation. A fixed portrait texture
    // clips and folds a landscape scene, especially mutable video images.
    // Keep the backing texture equal to the current logical viewport and let
    // lv_opengles_render_display_texture rotate it once into native scanout.
    lv_display_add_event_cb(display, DisplayResolutionChanged,
                            LV_EVENT_RESOLUTION_CHANGED, nullptr);
  }
  return display;
}

bool GpuRenderer::Present(lv_display_t* display) {
  if (!ready_ || display == nullptr)
    return false;

  const unsigned int index = next_buffer_;
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffers_[index]);
  glViewport(0, 0, width_, height_);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  // The native scanout and LVGL texture share the panel's portrait orientation.
  // No additional axis mirroring is required for the final composition.
  lv_opengles_render_display_texture(display, false, false);
  glFinish();
  if (glGetError() != GL_NO_ERROR || gr_drm_present(scanouts_[index]) != 0)
    return false;

  next_buffer_ = 1U - index;
  return true;
}

void GpuRenderer::Shutdown() {
  ready_ = false;
  if (egl_display_ != nullptr && egl_context_ != nullptr &&
      egl_surface_ != nullptr && eglMakeCurrent != nullptr) {
    eglMakeCurrent(static_cast<EGLDisplay>(egl_display_),
                   static_cast<EGLSurface>(egl_surface_),
                   static_cast<EGLSurface>(egl_surface_),
                   static_cast<EGLContext>(egl_context_));
    if (driver_initialized_)
      lv_opengles_deinit();
    if (glDeleteFramebuffers != nullptr)
      glDeleteFramebuffers(2, framebuffers_);
    if (glDeleteTextures != nullptr)
      glDeleteTextures(2, textures_);
    for (unsigned int i = 0; i < 2; ++i) {
      if (egl_images_[i] != nullptr && eglDestroyImageKHR != nullptr)
        eglDestroyImageKHR(static_cast<EGLDisplay>(egl_display_),
                           static_cast<EGLImageKHR>(egl_images_[i]));
    }
    eglMakeCurrent(static_cast<EGLDisplay>(egl_display_), EGL_NO_SURFACE,
                   EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }

  if (egl_display_ != nullptr) {
    if (egl_context_ != nullptr && eglDestroyContext != nullptr)
      eglDestroyContext(static_cast<EGLDisplay>(egl_display_),
                        static_cast<EGLContext>(egl_context_));
    if (egl_surface_ != nullptr && eglDestroySurface != nullptr)
      eglDestroySurface(static_cast<EGLDisplay>(egl_display_),
                        static_cast<EGLSurface>(egl_surface_));
    if (eglTerminate != nullptr)
      eglTerminate(static_cast<EGLDisplay>(egl_display_));
  }
  auto release_scanout = reinterpret_cast<ReleaseScanoutBuffer>(
      dlsym(RTLD_DEFAULT, "gr_drm_release_imported_scanout_buffer"));
  auto release_buffer = nativewindow_library_ == nullptr
      ? nullptr
      : reinterpret_cast<ReleaseHardwareBuffer>(
            dlsym(nativewindow_library_, "AHardwareBuffer_release"));
  for (unsigned int i = 0; i < 2; ++i) {
    if (scanouts_[i] != nullptr && release_scanout != nullptr)
      release_scanout(scanouts_[i]);
    if (hardware_buffers_[i] != nullptr && release_buffer != nullptr)
      release_buffer(hardware_buffers_[i]);
    hardware_buffers_[i] = nullptr;
  }
  if (gles_library_ != nullptr)
    dlclose(gles_library_);
  if (egl_library_ != nullptr)
    dlclose(egl_library_);
  if (nativewindow_library_ != nullptr)
    dlclose(nativewindow_library_);

  egl_library_ = nullptr;
  gles_library_ = nullptr;
  nativewindow_library_ = nullptr;
  egl_display_ = nullptr;
  egl_surface_ = nullptr;
  egl_context_ = nullptr;
  for (unsigned int i = 0; i < 2; ++i) {
    egl_images_[i] = nullptr;
    textures_[i] = 0;
    framebuffers_[i] = 0;
    scanouts_[i] = nullptr;
  }
  width_ = 0;
  height_ = 0;
  logical_width_ = 0;
  logical_height_ = 0;
  next_buffer_ = 0;
  driver_initialized_ = false;
}

}  // namespace aeraui
