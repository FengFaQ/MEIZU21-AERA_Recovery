// SPDX-License-Identifier: Apache-2.0
// WPE side of the pixel/input bridge. The privileged recovery process never
// loads WebKit; this executable runs only behind the audited platform jail.
#include "protocol.hpp"
#include "platform_sandbox.hpp"
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/headless/wpe-headless.h>
#include <epoxy/egl.h>
#include <glib-unix.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace aeraui::web;
struct Session;
static WPEInputMethodContext *CreateInputMethodContext(WPEDisplay *, WPEView *);
struct AERADisplay { WPEDisplay parent; Session *session = nullptr; };
struct AERADisplayClass { WPEDisplayClass parent; };
G_DEFINE_TYPE(AERADisplay, aera_display, WPE_TYPE_DISPLAY)
static void aera_display_init(AERADisplay *display) { display->session = nullptr; }
static gpointer GetEGLDisplay(WPEDisplay *, GError **error) {
  if (!epoxy_has_egl_extension(nullptr, "EGL_MESA_platform_surfaceless")) {
    g_set_error_literal(error, WPE_EGL_ERROR, WPE_EGL_ERROR_NOT_AVAILABLE,
                        "Mesa surfaceless EGL is unavailable");
    return nullptr;
  }
  EGLDisplay display = EGL_NO_DISPLAY;
  if (epoxy_has_egl_extension(nullptr, "EGL_EXT_platform_base"))
    display = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
                                       EGL_DEFAULT_DISPLAY, nullptr);
  else if (epoxy_has_egl_extension(nullptr, "EGL_KHR_platform_base"))
    display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                    EGL_DEFAULT_DISPLAY, nullptr);
  if (display != EGL_NO_DISPLAY) return display;
  g_set_error_literal(error, WPE_EGL_ERROR, WPE_EGL_ERROR_NOT_AVAILABLE,
                      "Failed to create the surfaceless EGL display");
  return nullptr;
}
static void aera_display_class_init(AERADisplayClass *klass) {
  auto *display = WPE_DISPLAY_CLASS(klass);
  display->connect = [](WPEDisplay *, GError **) -> gboolean { return TRUE; };
  display->create_view = [](WPEDisplay *d) -> WPEView * {
    return WPE_VIEW(g_object_new(WPE_TYPE_VIEW_HEADLESS, "display", d, nullptr));
  };
  display->create_toplevel = [](WPEDisplay *d, guint) -> WPEToplevel * {
    return WPE_TOPLEVEL(g_object_new(WPE_TYPE_TOPLEVEL_HEADLESS, "display", d, nullptr));
  };
  display->create_input_method_context = CreateInputMethodContext;
  // Mesa's surfaceless EGL renders WebKit's coordinated layer tree through
  // Zink and Turnip. WPE then publishes a read-back SHM buffer to the trusted
  // recovery UI, preserving the existing isolated pixel bridge.
  display->get_egl_display = GetEGLDisplay;
}
struct Session {
  GMainLoop *loop = nullptr;
  WebKitWebView *web = nullptr;
  WebKitNetworkSession *network = nullptr;
  WPEView *view = nullptr;
  uint8_t *shared = nullptr;
  WPEBuffer *latest = nullptr;
  uint32_t sequence = 0;
  bool pending = false;
  bool snapshot_pending = false;
  bool staged_ready = false;
  bool has_frame = false;
  gint64 last_native_frame_us = 0;
  gint64 last_snapshot_request_us = 0;
  gint64 last_input_us = 0;
  gint64 visual_activity_until_us = 0;
  bool media_playing = false;
  std::vector<uint8_t> staged;
  struct DownloadRecord;
  std::vector<std::unique_ptr<DownloadRecord>> downloads;
  uint32_t next_download_id = 1;
};
static bool Send(Session *s, const Message &m);
static WebKitCookieAcceptPolicy CookiePolicy(uint32_t value) {
  return value == 0 ? WEBKIT_COOKIE_POLICY_ACCEPT_NEVER :
      value == 2 ? WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS :
                   WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY;
}
static void BrowsingDataCleared(GObject *object, GAsyncResult *result,
                                gpointer data) {
  auto *s = static_cast<Session *>(data);
  GError *error = nullptr;
  Message message;
  if (webkit_website_data_manager_clear_finish(
          WEBKIT_WEBSITE_DATA_MANAGER(object), result, &error)) {
    message.kind = Kind::kBrowsingDataCleared;
    g_strlcpy(message.text, "Cookies and site data cleared.",
              sizeof(message.text));
  } else {
    message.kind = Kind::kBrowsingDataFailed;
    g_strlcpy(message.text, error ? error->message :
        "Could not clear cookies and site data.", sizeof(message.text));
  }
  g_clear_error(&error);
  Send(s, message);
}
static bool Send(Session *s, const Message &m) {
  if (send(4, &m, sizeof(m), MSG_DONTWAIT | MSG_NOSIGNAL) == sizeof(m)) return true;
  // Never stall WebKit or accumulate an unbounded queue behind recovery.
  g_main_loop_quit(s->loop); return false;
}
struct AERAInputMethodContext { WPEInputMethodContext parent; Session *session = nullptr; };
struct AERAInputMethodContextClass { WPEInputMethodContextClass parent; };
G_DEFINE_TYPE(AERAInputMethodContext, aera_input_method_context,
              WPE_TYPE_INPUT_METHOD_CONTEXT)
static void SendKeyboardFocus(WPEInputMethodContext *context, bool focused) {
  auto *input = reinterpret_cast<AERAInputMethodContext *>(context);
  if (!input->session) return;
  Message message;
  message.kind = focused ? Kind::kKeyboardShow : Kind::kKeyboardHide;
  if (focused)
    message.value = static_cast<uint32_t>(wpe_input_method_context_get_input_purpose(context));
  Send(input->session, message);
}
static void aera_input_method_context_init(AERAInputMethodContext *context) {
  context->session = nullptr;
}
static void aera_input_method_context_class_init(AERAInputMethodContextClass *klass) {
  auto *input = WPE_INPUT_METHOD_CONTEXT_CLASS(klass);
  input->get_preedit_string = [](WPEInputMethodContext *, char **text,
                                 GList **underlines, guint *cursor) {
    if (text) *text = g_strdup("");
    if (underlines) *underlines = nullptr;
    if (cursor) *cursor = 0;
  };
  input->focus_in = [](WPEInputMethodContext *context) {
    SendKeyboardFocus(context, true);
  };
  input->focus_out = [](WPEInputMethodContext *context) {
    SendKeyboardFocus(context, false);
  };
}
static WPEInputMethodContext *CreateInputMethodContext(WPEDisplay *display,
                                                       WPEView *view) {
  auto *context = reinterpret_cast<AERAInputMethodContext *>(
      g_object_new(aera_input_method_context_get_type(), "view", view, nullptr));
  context->session = reinterpret_cast<AERADisplay *>(display)->session;
  return WPE_INPUT_METHOD_CONTEXT(context);
}
static void Status(Session *s, const char *error = nullptr) {
  Message m;
  m.kind = error ? Kind::kError : Kind::kStatus;
  m.value = std::clamp<int>(webkit_web_view_get_estimated_load_progress(s->web) * 100, 0, 100);
  m.x = webkit_web_view_can_go_back(s->web); m.y = webkit_web_view_can_go_forward(s->web);
  const char *text = error ? error : webkit_web_view_get_uri(s->web);
  g_strlcpy(m.text, text ? text : "", sizeof(m.text));
  Send(s, m);
}
struct Session::DownloadRecord {
  Session *session = nullptr;
  WebKitDownload *download = nullptr;
  uint32_t id = 0;
  std::string name = "Download";
  gint64 last_progress_us = 0;
  bool announced = false;
  bool terminal = false;
  ~DownloadRecord() { g_clear_object(&download); }
};
static std::string SafeDownloadName(const char *suggested) {
  std::string name = suggested && *suggested ? suggested : "download";
  if (name.size() > 120) name.resize(120);
  for (char &value : name) {
    const unsigned char c = static_cast<unsigned char>(value);
    if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == ' '))
      value = '_';
  }
  while (!name.empty() && (name.front() == '.' || name.front() == ' '))
    name.erase(name.begin());
  if (name.empty()) name = "download";
  return name;
}
static std::string UniqueDownloadName(const std::string &requested) {
  if (access(("/downloads/" + requested).c_str(), F_OK)) return requested;
  const auto dot = requested.find_last_of('.');
  const std::string stem = dot == std::string::npos ? requested : requested.substr(0, dot);
  const std::string extension = dot == std::string::npos ? "" : requested.substr(dot);
  for (unsigned suffix = 2; suffix < 1000; ++suffix) {
    const std::string candidate = stem + " (" + std::to_string(suffix) + ")" + extension;
    if (access(("/downloads/" + candidate).c_str(), F_OK)) return candidate;
  }
  return std::to_string(g_get_real_time()) + "-" + requested;
}
static void SendDownload(Session::DownloadRecord *record, Kind kind,
                         const char *error = nullptr) {
  if (!record || !record->session) return;
  const uint64_t received = webkit_download_get_received_data_length(record->download);
  uint64_t total = 0;
  if (auto *response = webkit_download_get_response(record->download))
    total = webkit_uri_response_get_content_length(response);
  const auto progress = static_cast<int>(std::clamp(
      webkit_download_get_estimated_progress(record->download) * 100.0, 0.0, 100.0));
  Message message;
  message.kind = kind;
  message.sequence = record->id;
  message.x = progress;
  message.y = static_cast<int32_t>(std::min<uint64_t>(total / 1024, INT32_MAX));
  message.value = static_cast<uint32_t>(std::min<uint64_t>(received / 1024, UINT32_MAX));
  g_strlcpy(message.text, error ? error : record->name.c_str(), sizeof(message.text));
  Send(record->session, message);
}
static gboolean DownloadDestination(WebKitDownload *download,
                                    const gchar *suggested, gpointer data) {
  auto *record = static_cast<Session::DownloadRecord *>(data);
  record->name = UniqueDownloadName(SafeDownloadName(suggested));
  const std::string path = "/downloads/" + record->name;
  webkit_download_set_allow_overwrite(download, FALSE);
  // WPE WebKit takes an absolute native path here. Passing the file:// URI
  // used by older GTK WebKit builds fails its g_path_is_absolute assertion
  // and leaves the download announced but unable to create a destination.
  webkit_download_set_destination(download, path.c_str());
  record->announced = true;
  SendDownload(record, Kind::kDownloadStarted);
  return TRUE;
}
static void DownloadData(WebKitDownload *, guint64, gpointer data) {
  auto *record = static_cast<Session::DownloadRecord *>(data);
  const gint64 now = g_get_monotonic_time();
  if (record->last_progress_us && now - record->last_progress_us < 100000) return;
  record->last_progress_us = now;
  SendDownload(record, Kind::kDownloadProgress);
}
static void DownloadFailed(WebKitDownload *, GError *error, gpointer data) {
  auto *record = static_cast<Session::DownloadRecord *>(data);
  if (record->terminal) return;
  record->terminal = true;
  const bool cancelled = error && error->domain == WEBKIT_DOWNLOAD_ERROR &&
      error->code == WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER;
  SendDownload(record, cancelled ? Kind::kDownloadCancelled : Kind::kDownloadFailed,
               cancelled ? "Cancelled" : error ? error->message : "Download failed");
}
static void DownloadFinished(WebKitDownload *, gpointer data) {
  auto *record = static_cast<Session::DownloadRecord *>(data);
  if (record->terminal) return;
  record->terminal = true;
  SendDownload(record, Kind::kDownloadFinished);
}
static void DownloadStarted(WebKitNetworkSession *, WebKitDownload *download,
                            gpointer data) {
  auto *session = static_cast<Session *>(data);
  auto record = std::make_unique<Session::DownloadRecord>();
  record->session = session;
  record->download = WEBKIT_DOWNLOAD(g_object_ref(download));
  record->id = session->next_download_id++;
  auto *raw = record.get();
  g_signal_connect(download, "decide-destination",
                   G_CALLBACK(DownloadDestination), raw);
  g_signal_connect(download, "received-data", G_CALLBACK(DownloadData), raw);
  g_signal_connect(download, "failed", G_CALLBACK(DownloadFailed), raw);
  g_signal_connect(download, "finished", G_CALLBACK(DownloadFinished), raw);
  session->downloads.push_back(std::move(record));
}
static bool SamePixels(const uint8_t *packed, const uint8_t *data, guint stride) {
  for (int y = 0; y < kHeight; ++y) {
    if (memcmp(packed + y * kWidth * 4, data + y * stride, kWidth * 4)) return false;
  }
  return true;
}
static void CopyPixels(uint8_t *packed, const uint8_t *data, guint stride) {
  for (int y = 0; y < kHeight; ++y)
    memcpy(packed + y * kWidth * 4, data + y * stride, kWidth * 4);
}
static uint8_t *FramePixels(Session *s, uint32_t sequence) {
  return s->shared + FrameSlot(sequence) * kFrameBytes;
}
static void Publish(Session *s) {
  if (s->pending || !s->latest) return;
  auto *buffer = s->latest;
  if (!WPE_IS_BUFFER_SHM(buffer) || wpe_buffer_get_width(buffer) != kWidth ||
      wpe_buffer_get_height(buffer) != kHeight) {
    Status(s, "Unsupported browser frame."); g_main_loop_quit(s->loop); return;
  }
  auto *shm = WPE_BUFFER_SHM(buffer);
  const auto stride = wpe_buffer_shm_get_stride(shm);
  gsize bytes = 0;
  auto *data = static_cast<const uint8_t *>(g_bytes_get_data(wpe_buffer_shm_get_data(shm), &bytes));
  if (wpe_buffer_shm_get_format(shm) != WPE_PIXEL_FORMAT_ARGB8888 || stride < kWidth * 4 ||
      stride > kWidth * 4 + 4096 || bytes < uint64_t(stride) * kHeight) {
    Status(s, "Invalid browser frame layout."); g_main_loop_quit(s->loop); return;
  }
  // buffer-rendered is WebKit's commit notification. Comparing the complete
  // 8.3 MiB surface before every copy only duplicates memory traffic while
  // the compositor is actively scrolling.
  const uint32_t next_sequence = s->sequence + 1;
  CopyPixels(FramePixels(s, next_sequence), data, stride);
  Message m; m.kind = Kind::kFrame; m.sequence = next_sequence;
  m.x = kWidth; m.y = kHeight; m.value = kFrameBytes;
  s->sequence = next_sequence;
  s->pending = true; s->has_frame = true;
  Send(s, m);
  g_clear_object(&s->latest);
}
static void PublishPixels(Session *s, const uint8_t *data, guint stride) {
  if (s->has_frame && SamePixels(FramePixels(s, s->sequence), data, stride)) return;
  // Pixel motion is a more reliable signal than DOM media events. Sites such
  // as YouTube move video between documents/players and can lose a play event,
  // while every genuinely advancing frame necessarily changes this buffer.
  s->visual_activity_until_us = g_get_monotonic_time() + 1000000;
  const uint32_t next_sequence = s->sequence + 1;
  CopyPixels(FramePixels(s, next_sequence), data, stride);
  Message m; m.kind = Kind::kFrame; m.sequence = next_sequence;
  m.x = kWidth; m.y = kHeight; m.value = kFrameBytes;
  s->sequence = next_sequence;
  s->pending = true; s->has_frame = true; Send(s, m);
}
static void SnapshotDone(GObject *source, GAsyncResult *result, gpointer data) {
  auto *s = static_cast<Session *>(data);
  s->snapshot_pending = false;
  GError *error = nullptr;
  WebKitImage *image = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), result, &error);
  if (!image) {
    if (error) { Status(s, error->message); g_error_free(error); }
    return;
  }
  const guint stride = webkit_image_get_stride(image);
  gsize bytes = 0;
  const auto *data_bytes = static_cast<const uint8_t *>(g_bytes_get_data(webkit_image_as_bytes(image), &bytes));
  if (webkit_image_get_width(image) == kWidth && webkit_image_get_height(image) == kHeight &&
      stride >= kWidth * 4 && stride <= kWidth * 4 + 4096 && bytes >= uint64_t(stride) * kHeight) {
    if (s->pending) {
      if (s->staged.empty()) s->staged.resize(kFrameBytes);
      const uint8_t *baseline = s->staged_ready ? s->staged.data() :
          FramePixels(s, s->sequence);
      if (!SamePixels(baseline, data_bytes, stride)) {
        CopyPixels(s->staged.data(), data_bytes, stride);
        s->staged_ready = true;
        s->visual_activity_until_us = g_get_monotonic_time() + 1000000;
      }
    } else PublishPixels(s, data_bytes, stride);
  } else Status(s, "Invalid software browser frame.");
  g_object_unref(image);
}
static gboolean SnapshotTick(gpointer data) {
  auto *s = static_cast<Session *>(data);
  // WPE's GPU-composited SHM buffers are cheaper and preserve compositor
  // cadence. A full visible-page snapshot is only a compatibility fallback,
  // so it must not run at this timer's 125 Hz polling frequency.
  // While loading or touching, cap fallback requests at 30 Hz; once idle, a
  // sparse refresh is enough for pages that never submit a native SHM buffer.
  const gint64 now = g_get_monotonic_time();
  // Accelerated WPE commits every visible change through buffer-rendered.
  // Snapshots are only a startup compatibility fallback; running them beside
  // native commits forces a second full-page paint and stalls complex pages.
  if (s->last_native_frame_us) return G_SOURCE_CONTINUE;
  const bool interacting = s->last_input_us && now - s->last_input_us < 500000;
  const bool loading = webkit_web_view_get_estimated_load_progress(s->web) < 0.999;
  const bool visual_motion = s->media_playing || now < s->visual_activity_until_us;
  const gint64 native_quiet_us = interacting ? 50000 : 500000;
  if (s->last_native_frame_us && now - s->last_native_frame_us < native_quiet_us)
    return G_SOURCE_CONTINUE;
  // Some media paths do not always submit a new WPE buffer for video frames.
  // While media is playing, drive the non-overlapping snapshot path as fast as
  // 60 Hz; snapshot_pending naturally applies back-pressure.
  const gint64 interval_us = visual_motion ? 16667
      : (interacting || loading) ? 33333 : 500000;
  if (s->snapshot_pending ||
      (s->last_snapshot_request_us &&
       now - s->last_snapshot_request_us < interval_us))
    return G_SOURCE_CONTINUE;
  s->snapshot_pending = true;
  s->last_snapshot_request_us = now;
  webkit_web_view_get_snapshot(s->web, WEBKIT_SNAPSHOT_REGION_VISIBLE,
      WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr, SnapshotDone, s);
  return G_SOURCE_CONTINUE;
}
static gboolean Input(gint fd, GIOCondition condition, gpointer data) {
  auto *s = static_cast<Session *>(data);
  if (condition & (G_IO_HUP | G_IO_ERR)) { g_main_loop_quit(s->loop); return G_SOURCE_REMOVE; }
  Message m;
  const auto count = recv(fd, &m, sizeof(m), MSG_DONTWAIT | MSG_TRUNC);
  if (count != sizeof(m) || !Valid(m, false)) { g_main_loop_quit(s->loop); return G_SOURCE_REMOVE; }
  if (m.kind == Kind::kOpen || m.kind == Kind::kBack ||
      m.kind == Kind::kForward || m.kind == Kind::kReload ||
      m.kind == Kind::kTouchDown || m.kind == Kind::kTouchMove ||
      m.kind == Kind::kTouchUp)
    s->last_input_us = g_get_monotonic_time();
  switch (m.kind) {
    case Kind::kOpen: {
      const auto address = Address(m.text);
      if (!address.empty()) webkit_web_view_load_uri(s->web, address.c_str());
      else Status(s, "Only HTTP and HTTPS addresses are supported.");
      break;
    }
    case Kind::kBack: webkit_web_view_go_back(s->web); break;
    case Kind::kForward: webkit_web_view_go_forward(s->web); break;
    case Kind::kReload: webkit_web_view_reload(s->web); break;
    case Kind::kStop: webkit_web_view_stop_loading(s->web); break;
    case Kind::kSetZoom:
      webkit_web_view_set_zoom_level(s->web, m.value / 100.0);
      break;
    case Kind::kDownloadCancel:
      for (const auto &record : s->downloads)
        if (record->id == m.sequence && !record->terminal) {
          webkit_download_cancel(record->download);
          break;
        }
      break;
    case Kind::kSetCookiePolicy:
      webkit_cookie_manager_set_accept_policy(
          webkit_network_session_get_cookie_manager(s->network),
          CookiePolicy(m.value));
      break;
    case Kind::kClearBrowsingData:
      webkit_website_data_manager_clear(
          webkit_network_session_get_website_data_manager(s->network),
          WEBKIT_WEBSITE_DATA_ALL, 0, nullptr, BrowsingDataCleared, s);
      break;
    case Kind::kClose: g_main_loop_quit(s->loop); break;
    case Kind::kAck:
      if (!s->pending || m.sequence != s->sequence) { g_main_loop_quit(s->loop); break; }
      s->pending = false;
      if (s->staged_ready) {
        s->staged_ready = false;
        PublishPixels(s, s->staged.data(), kWidth * 4);
      } else Publish(s);
      break;
    case Kind::kTouchDown: case Kind::kTouchMove: case Kind::kTouchUp: {
      const auto kind = m.kind == Kind::kTouchDown ? WPE_EVENT_TOUCH_DOWN :
                        m.kind == Kind::kTouchUp ? WPE_EVENT_TOUCH_UP : WPE_EVENT_TOUCH_MOVE;
      auto *event = wpe_event_touch_new(kind, s->view, WPE_INPUT_SOURCE_TOUCHSCREEN,
          g_get_monotonic_time() / 1000, static_cast<WPEModifiers>(0),
          m.value + 1, m.x, m.y);
      wpe_view_event(s->view, event); wpe_event_unref(event); break;
    }
    case Kind::kKey: {
      if (m.value > 0x10ffff) break;
      guint key = m.value == 8 ? 0xff08 : m.value == 13 ? 0xff0d :
                  m.value < 256 ? m.value : (0x01000000 | m.value);
      for (auto kind : {WPE_EVENT_KEYBOARD_KEY_DOWN, WPE_EVENT_KEYBOARD_KEY_UP}) {
        auto *event = wpe_event_keyboard_new(kind, s->view, WPE_INPUT_SOURCE_KEYBOARD,
            g_get_monotonic_time() / 1000, static_cast<WPEModifiers>(0), 0, key);
        wpe_view_event(s->view, event); wpe_event_unref(event);
      }
      break;
    }
    default: return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}
int main(int argc, char **argv) {
  // Defense in depth only, not a replacement for the missing launcher. A
  // command-line switch or namespace existence never proves isolation.
  if (argc != 2 || strcmp(argv[1], "--isolated-ipc-v1") || !aera_sandbox::Validate()) {
    fprintf(stderr, "AERA browser requires an isolated, unprivileged launcher.\n"); return 78;
  }
  struct stat frame{}; int socket_type = 0; socklen_t type_size = sizeof(socket_type);
  if (fstat(3, &frame) || !S_ISREG(frame.st_mode) || frame.st_size != kSharedBytes ||
      getsockopt(4, SOL_SOCKET, SO_TYPE, &socket_type, &type_size) || socket_type != SOCK_SEQPACKET) {
    fprintf(stderr, "Invalid browser bridge: size=%lld type=%d errno=%d\n", (long long)frame.st_size, socket_type, errno); return 78;
  }
  Session s;
  s.shared = static_cast<uint8_t *>(mmap(nullptr, kSharedBytes, PROT_READ | PROT_WRITE, MAP_SHARED, 3, 0));
  if (s.shared == MAP_FAILED) { perror("Map browser bridge"); return 78; }
  close(3);
  // Only the browser shell owns recovery's pixel/control bridge. WebKit
  // auxiliary execs inherit their own IPC descriptors, never these channels.
  if (fcntl(4, F_SETFD, FD_CLOEXEC)) return 78;
  s.loop = g_main_loop_new(nullptr, FALSE);
  auto *aera_display = reinterpret_cast<AERADisplay *>(
      g_object_new(aera_display_get_type(), nullptr));
  aera_display->session = &s;
  auto *display = WPE_DISPLAY(aera_display);
  GError *error = nullptr;
  if (!wpe_display_connect(display, &error)) { fprintf(stderr, "Browser display: %s\n", error ? error->message : "failed"); return 78; }
  auto *settings = webkit_settings_new_with_settings("enable-developer-extras", FALSE,
      "enable-javascript", TRUE, "enable-webgl", FALSE,
      "enable-smooth-scrolling", TRUE,
      "media-playback-requires-user-gesture", TRUE, nullptr);
  webkit_settings_set_user_agent(settings,
      "Mozilla/5.0 (Linux; Android 16; CPH2653 Build/BP2A.250605.031.A2) "
      "AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/140.0.0.0 Mobile Safari/537.36");
  auto *context = webkit_web_context_new();
  webkit_web_context_register_uri_scheme(context, "aera",
      +[](WebKitURISchemeRequest *request, gpointer) {
        const char *uri = webkit_uri_scheme_request_get_uri(request);
        const bool test = uri && !strcmp(uri, "aera://test");
        if (!uri || (strcmp(uri, "aera://start") && !test)) {
          GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Unknown internal page");
          webkit_uri_scheme_request_finish_error(request, error); g_error_free(error); return;
        }
        static const char start[] = R"HTML(<!doctype html><html><meta name="viewport" content="width=device-width,initial-scale=1"><title>AERA Browser</title>
<style>body{background:#202329;color:#f4f4f5;font:24px sans-serif;margin:44px}h1{font-size:54px}p{line-height:1.6;color:#bcbfc8}a,button,input{font:24px sans-serif;border-radius:12px;padding:18px}a{color:#7dd3fc}button{background:#7dd3fc;color:#101828;border:0}input{width:85%;margin:28px 0;background:#353b46;color:white;border:1px solid #64748b}.space{height:500px}</style>
<h1>AERA Browser</h1><p>This page is built into recovery. No network connection is required.</p><p id="js">JavaScript pending</p>
<script>document.getElementById('js').textContent='JavaScript is running';</script>
<button onclick="this.textContent='Touch works';this.style.background='#86efac'">Test touch</button><br><input placeholder="Type here" aria-label="Typing test"><p><a href="aera://test">Open a second page</a></p><div class="space"></div><h2>Scroll works</h2><p>The browser remains isolated from recovery data.</p></html>)HTML";
        static const char second[] = "<!doctype html><html><meta name='viewport' content='width=device-width,initial-scale=1'><body style='background:#202329;color:white;font:28px sans-serif;padding:44px'><h1>Second page</h1><p>Navigation works.</p><a style='color:#7dd3fc' href='aera://start'>Back to start</a></body></html>";
        const char *html = test ? second : start;
        GInputStream *stream = g_memory_input_stream_new_from_data(html, strlen(html), nullptr);
        webkit_uri_scheme_request_finish(request, stream, strlen(html), "text/html");
        g_object_unref(stream);
      }, nullptr, nullptr);
  const bool persistent_profile = !access("/profile", R_OK | W_OK);
  auto *network = persistent_profile
      ? webkit_network_session_new("/profile/data", "/tmp/cache")
      : webkit_network_session_new_ephemeral();
  s.network = network;
  webkit_network_session_set_itp_enabled(network, TRUE);
  auto *cookie_manager = webkit_network_session_get_cookie_manager(network);
  if (persistent_profile)
    webkit_cookie_manager_set_persistent_storage(
        cookie_manager, "/profile/cookies.sqlite",
        WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
  webkit_cookie_manager_set_accept_policy(
      cookie_manager, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
  webkit_network_session_set_tls_errors_policy(network, WEBKIT_TLS_ERRORS_POLICY_FAIL);
  auto *content = webkit_user_content_manager_new();
  g_signal_connect(content, "script-message-received::aeraMedia",
      G_CALLBACK(+[](WebKitUserContentManager *, JSCValue *value, gpointer data) {
        if (jsc_value_is_boolean(value))
          static_cast<Session *>(data)->media_playing = jsc_value_to_boolean(value);
      }), &s);
  webkit_user_content_manager_register_script_message_handler(
      content, "aeraMedia", nullptr);
  static const char media_observer[] = R"JS((()=>{
    let active=false;
    const report=()=>{
      const next=[...document.querySelectorAll('video')].some(
        video=>!video.paused&&!video.ended&&video.readyState>=2);
      if(next===active)return;
      active=next;
      window.webkit.messageHandlers.aeraMedia.postMessage(active);
    };
    for(const event of ['playing','play','pause','ended','emptied','abort'])
      document.addEventListener(event,report,true);
  })();)JS";
  auto *media_script = webkit_user_script_new(media_observer,
      WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
  webkit_user_content_manager_add_script(content, media_script);
  webkit_user_script_unref(media_script);
  // Many old or desktop-first pages omit a viewport declaration. Mobile
  // browsers lay those pages out at device width instead of shrinking a
  // roughly 980 CSS-pixel canvas into an unreadable strip.
  static const char mobile_viewport[] = R"JS((()=>{
    if(document.querySelector('meta[name="viewport"]'))return;
    const meta=document.createElement('meta');
    meta.name='viewport';
    meta.content='width=device-width,initial-scale=1';
    (document.head||document.documentElement).appendChild(meta);
  })();)JS";
  auto *viewport_script = webkit_user_script_new(mobile_viewport,
      WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, nullptr, nullptr);
  webkit_user_content_manager_add_script(content, viewport_script);
  webkit_user_script_unref(viewport_script);
  s.web = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display,
      "settings", settings, "web-context", context, "network-session", network,
      "user-content-manager", content, nullptr));
  g_signal_connect(network, "download-started", G_CALLBACK(DownloadStarted), &s);
  g_signal_connect(s.web, "permission-request", G_CALLBACK(+[](WebKitWebView *, WebKitPermissionRequest *r, gpointer) -> gboolean {
    webkit_permission_request_deny(r); return TRUE;
  }), nullptr);
  g_signal_connect(s.web, "run-file-chooser", G_CALLBACK(+[](WebKitWebView *, WebKitFileChooserRequest *r, gpointer) -> gboolean {
    webkit_file_chooser_request_cancel(r); return TRUE;
  }), nullptr);
  g_signal_connect(s.web, "decide-policy", G_CALLBACK(+[](WebKitWebView *, WebKitPolicyDecision *d, WebKitPolicyDecisionType t, gpointer) -> gboolean {
    if (t == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) { webkit_policy_decision_ignore(d); return TRUE; }
    if (t != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return FALSE;
    auto *action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
    const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    if (uri && !Address(uri).empty()) return FALSE;
    webkit_policy_decision_ignore(d); return TRUE;
  }), nullptr);
  g_signal_connect(s.web, "notify::estimated-load-progress", G_CALLBACK(+[](GObject *, GParamSpec *, gpointer data) {
    Status(static_cast<Session *>(data));
  }), &s);
  g_signal_connect(s.web, "notify::uri", G_CALLBACK(+[](GObject *, GParamSpec *, gpointer data) {
    Status(static_cast<Session *>(data));
  }), &s);
  g_signal_connect(s.web, "load-failed", G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent, const char *, GError *e, gpointer data) -> gboolean {
    Status(static_cast<Session *>(data), e->message); return TRUE;
  }), &s);
  g_signal_connect(s.web, "web-process-terminated", G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason, gpointer data) {
    auto *s = static_cast<Session *>(data); Status(s, "Browser process stopped."); g_main_loop_quit(s->loop);
  }), &s);
  s.view = webkit_web_view_get_wpe_view(s.web);
  g_signal_connect(s.view, "buffer-rendered", G_CALLBACK(+[](WPEView *, WPEBuffer *buffer, gpointer data) {
    auto *s = static_cast<Session *>(data);
    s->last_native_frame_us = g_get_monotonic_time();
    g_set_object(&s->latest, buffer); Publish(s);
  }), &s);
  auto *toplevel = wpe_view_get_toplevel(s.view);
  // Render a sharp 1080x1920 backing store while exposing a 360x640 logical
  // Android handset viewport. The Mobile UA controls server-side selection;
  // the logical width controls responsive CSS and media queries.
  wpe_toplevel_scale_changed(toplevel, kDeviceScale);
  wpe_toplevel_resize(toplevel, kViewWidth, kViewHeight);
  wpe_view_focus_in(s.view);
  const auto input = g_unix_fd_add(4, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR), Input, &s);
  const auto snapshots = g_timeout_add(8, SnapshotTick, &s);
  Status(&s);
  webkit_web_view_load_uri(s.web, "aera://start");
  g_main_loop_run(s.loop);
  if (g_main_context_find_source_by_id(nullptr, input)) g_source_remove(input);
  if (g_main_context_find_source_by_id(nullptr, snapshots)) g_source_remove(snapshots);
  g_clear_object(&s.latest);
  g_object_unref(s.web);
  webkit_user_content_manager_unregister_script_message_handler(
      content, "aeraMedia", nullptr);
  g_object_unref(content); s.network = nullptr; g_object_unref(network); g_object_unref(context);
  g_object_unref(settings); g_object_unref(display); g_main_loop_unref(s.loop);
  munmap(s.shared, kSharedBytes); close(4);
  return 0;
}
