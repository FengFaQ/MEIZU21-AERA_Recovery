/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Silent fallback for devices without a recovery audio implementation. It
 * preserves the narrow audio bridge protocol so media-capable plugins remain
 * usable without exposing audio devices or treating missing audio as fatal.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

static const uid_t kBrowserUid = 99090;
static const uid_t kMediaUid = 99092;
static const uid_t kDoomUid = 99094;
static const uint32_t kMagic = 0x41525041U;
static const uint32_t kRate = 48000U;
static const uint32_t kChannels = 2U;
static const uint32_t kBits = 16U;
static const char kSocketName[] = "aera-browser-audio-v1";
static volatile sig_atomic_t g_stop = 0;

static void Stop(int signal_number) {
  (void)signal_number;
  g_stop = 1;
}

static int ReadAll(int fd, void* output, size_t size) {
  uint8_t* cursor = output;
  while (size && !g_stop) {
    const ssize_t count = read(fd, cursor, size);
    if (count > 0) {
      cursor += count;
      size -= (size_t)count;
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    return 0;
  }
  return size == 0;
}

static void DrainClient(int client) {
  struct ucred credentials = {0};
  socklen_t credentials_size = sizeof(credentials);
  if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials,
                 &credentials_size) != 0 ||
      credentials_size != sizeof(credentials) ||
      (credentials.uid != kBrowserUid && credentials.uid != kMediaUid &&
       credentials.uid != kDoomUid)) {
    return;
  }

  uint32_t hello[4] = {0};
  if (!ReadAll(client, hello, sizeof(hello)) || hello[0] != kMagic ||
      hello[1] != kRate || hello[2] != kChannels || hello[3] != kBits) {
    return;
  }

  uint8_t buffer[8192];
  while (!g_stop) {
    const ssize_t count = read(client, buffer, sizeof(buffer));
    if (count > 0) continue;
    if (count < 0 && errno == EINTR) continue;
    break;
  }
}

int main(int argc, char** argv) {
  if (argc != 2 || strcmp(argv[1], "--browser-audio") != 0 || getuid() != 0) {
    fprintf(stderr,
            "Usage (root recovery only): aera-audio-null-bridge --browser-audio\n");
    return 64;
  }

  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, Stop);
  signal(SIGINT, Stop);
  prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);

  const int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (server < 0) return 70;
  struct sockaddr_un address = {0};
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path + 1, kSocketName, sizeof(kSocketName) - 1);
  const socklen_t address_size = (socklen_t)(
      offsetof(struct sockaddr_un, sun_path) + 1 + sizeof(kSocketName) - 1);
  if (bind(server, (const struct sockaddr*)&address, address_size) != 0 ||
      listen(server, 1) != 0) {
    close(server);
    return 70;
  }

  fprintf(stderr, "AERA audio: using silent fallback\n");
  while (!g_stop) {
    const int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0) {
      if (errno == EINTR) continue;
      break;
    }
    DrainClient(client);
    close(client);
  }
  close(server);
  return 0;
}
