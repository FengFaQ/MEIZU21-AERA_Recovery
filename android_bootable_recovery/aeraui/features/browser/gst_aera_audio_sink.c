/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * GStreamer output sink for the isolated AERA browser.  The browser is not
 * allowed to open ALSA devices or talk to Android audio services.  This sink
 * accepts one fixed raw format and forwards it to the small, root-owned AERA
 * audio bridge over an abstract AF_UNIX socket.
 */

#define _GNU_SOURCE
#include <gst/base/gstbasesink.h>
#include <gst/gst.h>

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef PACKAGE
#define PACKAGE "aera-recovery"
#endif

#define GST_TYPE_AERA_AUDIO_SINK (gst_aera_audio_sink_get_type())

typedef struct _GstAeraAudioSink {
  GstBaseSink parent;
  gint socket_fd;
  gint flushing;
} GstAeraAudioSink;

typedef struct _GstAeraAudioSinkClass {
  GstBaseSinkClass parent_class;
} GstAeraAudioSinkClass;

G_DEFINE_TYPE(GstAeraAudioSink, gst_aera_audio_sink, GST_TYPE_BASE_SINK)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, "
                    "format=(string)S16LE, "
                    "layout=(string)interleaved, "
                    "rate=(int)48000, "
                    "channels=(int)2"));

static gboolean write_all(GstAeraAudioSink *sink, const guint8 *bytes,
                          gsize size) {
  while (size && !g_atomic_int_get(&sink->flushing)) {
    const int fd = g_atomic_int_get(&sink->socket_fd);
    if (fd < 0) return FALSE;
    const ssize_t count = send(fd, bytes, size, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count > 0) {
      bytes += count;
      size -= (gsize)count;
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd output = {.fd = fd, .events = POLLOUT, .revents = 0};
      const int ready = poll(&output, 1, 50);
      if (ready >= 0) continue;
      if (errno == EINTR) continue;
    }
    return FALSE;
  }
  return size == 0;
}

static gboolean gst_aera_audio_sink_start(GstBaseSink *base) {
  GstAeraAudioSink *sink = (GstAeraAudioSink *)base;
  static const char socket_name[] = "aera-browser-audio-v1";
  static const uint32_t hello[] = {0x41525041U, 48000U, 2U, 16U};

  g_atomic_int_set(&sink->flushing, FALSE);
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    GST_ELEMENT_ERROR(sink, RESOURCE, OPEN_WRITE,
                      ("AERA browser audio socket could not be created"),
                      ("%s", g_strerror(errno)));
    return FALSE;
  }

  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path + 1, socket_name, sizeof(socket_name) - 1);
  const socklen_t address_size = (socklen_t)(
      offsetof(struct sockaddr_un, sun_path) + 1 + sizeof(socket_name) - 1);
  if (connect(fd, (const struct sockaddr *)&address, address_size) != 0) {
    const int saved_errno = errno;
    close(fd);
    GST_ELEMENT_ERROR(sink, RESOURCE, OPEN_WRITE,
                      ("AERA recovery audio bridge is unavailable"),
                      ("%s", g_strerror(saved_errno)));
    return FALSE;
  }

  g_atomic_int_set(&sink->socket_fd, fd);
  struct ucred peer;
  socklen_t peer_size = sizeof(peer);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 ||
      peer_size != sizeof(peer) || peer.uid != 0 ||
      !write_all(sink, (const guint8 *)hello, sizeof(hello))) {
    g_atomic_int_set(&sink->socket_fd, -1);
    close(fd);
    GST_ELEMENT_ERROR(sink, RESOURCE, OPEN_WRITE,
                      ("AERA recovery audio bridge rejected the stream"),
                      ("The trusted root audio endpoint was not available"));
    return FALSE;
  }

  return TRUE;
}

static gboolean gst_aera_audio_sink_stop(GstBaseSink *base) {
  GstAeraAudioSink *sink = (GstAeraAudioSink *)base;
  const int fd = g_atomic_int_get(&sink->socket_fd);
  g_atomic_int_set(&sink->socket_fd, -1);
  if (fd >= 0) close(fd);
  return TRUE;
}

static gboolean gst_aera_audio_sink_unlock(GstBaseSink *base) {
  GstAeraAudioSink *sink = (GstAeraAudioSink *)base;
  g_atomic_int_set(&sink->flushing, TRUE);
  return TRUE;
}

static gboolean gst_aera_audio_sink_unlock_stop(GstBaseSink *base) {
  GstAeraAudioSink *sink = (GstAeraAudioSink *)base;
  g_atomic_int_set(&sink->flushing, FALSE);
  return TRUE;
}

static GstFlowReturn gst_aera_audio_sink_render(GstBaseSink *base,
                                                GstBuffer *buffer) {
  GstAeraAudioSink *sink = (GstAeraAudioSink *)base;
  GstMapInfo map;
  if (g_atomic_int_get(&sink->flushing)) return GST_FLOW_FLUSHING;
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return GST_FLOW_ERROR;
  const gboolean written = write_all(sink, map.data, map.size);
  gst_buffer_unmap(buffer, &map);
  if (g_atomic_int_get(&sink->flushing)) return GST_FLOW_FLUSHING;
  if (!written) {
    GST_ELEMENT_ERROR(sink, RESOURCE, WRITE,
                      ("AERA browser audio stream stopped"),
                      ("The recovery audio bridge closed the connection"));
    return GST_FLOW_ERROR;
  }
  return GST_FLOW_OK;
}

static void gst_aera_audio_sink_init(GstAeraAudioSink *sink) {
  g_atomic_int_set(&sink->socket_fd, -1);
  g_atomic_int_set(&sink->flushing, FALSE);
  gst_base_sink_set_sync(GST_BASE_SINK(sink), TRUE);
  gst_base_sink_set_async_enabled(GST_BASE_SINK(sink), TRUE);
}

static void gst_aera_audio_sink_class_init(GstAeraAudioSinkClass *klass) {
  GstElementClass *element = GST_ELEMENT_CLASS(klass);
  GstBaseSinkClass *base = GST_BASE_SINK_CLASS(klass);
  gst_element_class_set_static_metadata(
      element, "AERA recovery audio output", "Sink/Audio",
      "Streams fixed-format PCM to the protected AERA audio bridge",
      "AERA Recovery Project");
  gst_element_class_add_static_pad_template(element, &sink_template);
  base->start = gst_aera_audio_sink_start;
  base->stop = gst_aera_audio_sink_stop;
  base->unlock = gst_aera_audio_sink_unlock;
  base->unlock_stop = gst_aera_audio_sink_unlock_stop;
  base->render = gst_aera_audio_sink_render;
}

static gboolean plugin_init(GstPlugin *plugin) {
  return gst_element_register(plugin, "autoaudiosink", GST_RANK_PRIMARY + 100,
                              GST_TYPE_AERA_AUDIO_SINK);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, aeraaudio,
                  "AERA recovery browser audio output", plugin_init, "1.0",
                  "Apache-2.0", "AERA Recovery Project",
                  "https://github.com/AERA-Plugins")
