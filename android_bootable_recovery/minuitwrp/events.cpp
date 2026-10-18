/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/poll.h>
#include <limits.h>
#include <linux/input.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fstream>
#ifdef USE_INPUT_FF_HAPTICS
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#endif
#ifdef USE_QTI_AIDL_HAPTICS_FIX_OFF
#include <thread>
#endif

#ifdef USE_QTI_HAPTICS
#include <android/hardware/vibrator/1.2/IVibrator.h>
#endif

#ifdef USE_QTI_AIDL_HAPTICS
#include <aidl/android/hardware/vibrator/IVibrator.h>
#include <android/binder_manager.h>
using ::aidl::android::hardware::vibrator::IVibrator;
#ifdef USE_QTI_AIDL_HAPTICS_FQNAME
static const std::string kVibratorInstance = std::string("android.hardware.vibrator.") + USE_QTI_AIDL_HAPTICS_FQNAME;
#else
static const std::string kVibratorInstance = std::string(IVibrator::descriptor) + "/default";
#endif
#ifdef USE_QTI_AIDL_HAPTICS_FIX_OFF
static std::atomic_int vib_on_count = 0;
#endif
#endif

#include "common.h"
#include "twcommon.h"

#include "minuitwrp/minui.h"

//#define _EVENT_LOGGING

#define MAX_DEVICES         32

#define VIBRATOR_TIMEOUT_FILE	"/sys/class/timed_output/vibrator/enable"
#define VIBRATOR_TIME_MS    50

#define LEDS_HAPTICS_DURATION_FILE	"/sys/class/leds/vibrator/duration"
#define LEDS_HAPTICS_ACTIVATE_FILE	"/sys/class/leds/vibrator/activate"

#ifndef SYN_REPORT
#define SYN_REPORT          0x00
#endif
#ifndef SYN_CONFIG
#define SYN_CONFIG          0x01
#endif
#ifndef SYN_MT_REPORT
#define SYN_MT_REPORT       0x02
#endif

#define ABS_MT_POSITION     0x2a /* Group a set of X and Y */
#define ABS_MT_AMPLITUDE    0x2b /* Group a set of Z and W */
#define ABS_MT_SLOT         0x2f
#define ABS_MT_TOUCH_MAJOR  0x30
#define ABS_MT_TOUCH_MINOR  0x31
#define ABS_MT_WIDTH_MAJOR  0x32
#define ABS_MT_WIDTH_MINOR  0x33
#define ABS_MT_ORIENTATION  0x34
#define ABS_MT_POSITION_X   0x35
#define ABS_MT_POSITION_Y   0x36
#define ABS_MT_TOOL_TYPE    0x37
#define ABS_MT_BLOB_ID      0x38
#define ABS_MT_TRACKING_ID  0x39
#define ABS_MT_PRESSURE     0x3a
#define ABS_MT_DISTANCE     0x3b

enum {
    DOWN_NOT,
    DOWN_SENT,
    DOWN_RELEASED,
};

struct virtualkey {
    int scancode;
    int centerx, centery;
    int width, height;
};

struct position {
    int x, y;
    int synced;
    int valid;
    struct input_absinfo xi, yi;
};

struct ev {
    struct pollfd *fd;

    struct virtualkey *vks;
    int vk_count;

    char deviceName[64];

    int ignored;
    bool mouse;

    struct position p, mt_p;
    struct position mt_slots[2];
    int mt_slot;
    int mt_secondary_slot;
    bool mt_active[2];
    bool mt_dirty[2];
    int down;
};

static struct pollfd ev_fds[MAX_DEVICES];
static struct ev evs[MAX_DEVICES];
static unsigned ev_count = 0;
static struct timeval lastInputStat;
static time_t lastInputMTime;
static int has_mouse = 0;
static struct input_event secondary_touch_event;
static bool secondary_touch_pending = false;

static inline int ABS(int x) {
    return x<0?-x:x;
}

int write_to_file(const std::string& fn, const std::string& line) {
	FILE *file;
	file = fopen(fn.c_str(), "w");
	if (file != NULL) {
		fwrite(line.c_str(), line.size(), 1, file);
		fclose(file);
		return 0;
	}
	LOGI("Cannot find file %s\n", fn.c_str());
	return -1;
}

#ifndef TW_NO_HAPTICS
#ifndef TW_HAPTICS_TSPDRV
#ifdef USE_INPUT_FF_HAPTICS
namespace {

constexpr size_t kBitsPerWord = sizeof(unsigned long) * 8;

bool InputBitSet(const unsigned long* bits, size_t bit) {
    return (bits[bit / kBitsPerWord] & (1UL << (bit % kBitsPerWord))) != 0;
}

class InputForceFeedbackHaptics {
  public:
    static InputForceFeedbackHaptics& Instance() {
        // Recovery owns this object for its complete process lifetime. Keeping
        // it alive avoids teardown races with the detached worker during a
        // userspace handoff.
        static auto* instance = new InputForceFeedbackHaptics();
        return *instance;
    }

    void Pulse(int duration_ms) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            duration_ms_ = duration_ms;
            ++request_sequence_;
        }
        condition_.notify_one();
    }

  private:
    InputForceFeedbackHaptics() {
        std::thread([this] { WorkerLoop(); }).detach();
    }

    bool OpenDevice() {
        if (fd_ >= 0) return true;

        DIR* directory = opendir("/dev/input");
        if (directory == nullptr) return false;

        int fallback_fd = -1;
        bool fallback_rumble = false;
        bool fallback_constant = false;
        dirent* entry = nullptr;
        while ((entry = readdir(directory)) != nullptr) {
            if (strncmp(entry->d_name, "event", 5) != 0) continue;

            const std::string path = std::string("/dev/input/") + entry->d_name;
            const int candidate = open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (candidate < 0) continue;

            unsigned long event_bits[(EV_MAX / kBitsPerWord) + 1] = {};
            unsigned long effect_bits[(FF_MAX / kBitsPerWord) + 1] = {};
            if (ioctl(candidate, EVIOCGBIT(0, sizeof(event_bits)), event_bits) < 0 ||
                !InputBitSet(event_bits, EV_FF) ||
                ioctl(candidate, EVIOCGBIT(EV_FF, sizeof(effect_bits)), effect_bits) < 0) {
                close(candidate);
                continue;
            }

            const bool rumble = InputBitSet(effect_bits, FF_RUMBLE);
            const bool constant = InputBitSet(effect_bits, FF_CONSTANT);
            if (!rumble && !constant) {
                close(candidate);
                continue;
            }

            char device_name[128] = {};
            ioctl(candidate, EVIOCGNAME(sizeof(device_name)), device_name);
            if (strcmp(device_name, "qcom-hv-haptics") == 0) {
                if (fallback_fd >= 0) close(fallback_fd);
                fd_ = candidate;
                // Some Qualcomm HV haptics revisions expose FF_RUMBLE but
                // reject its translated periodic waveform at upload time.
                // Their native FF_CONSTANT path is the reliable direct-play
                // interface, so prefer it whenever the driver advertises it.
                supports_rumble_ = rumble && !constant;
                supports_constant_ = constant;
                break;
            }

            if (fallback_fd < 0) {
                fallback_fd = candidate;
                fallback_rumble = rumble;
                fallback_constant = constant;
            } else {
                close(candidate);
            }
        }
        closedir(directory);

        if (fd_ < 0 && fallback_fd >= 0) {
            fd_ = fallback_fd;
            supports_rumble_ = fallback_rumble;
            supports_constant_ = fallback_constant;
        }

        if (fd_ >= 0) {
            LOGI("AERA input force-feedback haptics ready\n");
            missing_device_logged_ = false;
            return true;
        }
        if (!missing_device_logged_) {
            LOGI("AERA input force-feedback haptics device was not found\n");
            missing_device_logged_ = true;
        }
        return false;
    }

    bool StartEffect(int duration_ms) {
        if (!OpenDevice()) return false;

        ff_effect effect = {};
        effect.id = -1;
        effect.replay.length = static_cast<uint16_t>(duration_ms);
        if (supports_rumble_) {
            effect.type = FF_RUMBLE;
            effect.u.rumble.strong_magnitude = 0x6000;
            effect.u.rumble.weak_magnitude = 0x3000;
        } else if (supports_constant_) {
            effect.type = FF_CONSTANT;
            effect.u.constant.level = 0x5fff;
        } else {
            return false;
        }

        if (ioctl(fd_, EVIOCSFF, &effect) < 0) {
            ResetDevice();
            return false;
        }
        effect_id_ = effect.id;

        input_event play = {};
        play.type = EV_FF;
        play.code = effect_id_;
        play.value = 1;
        if (write(fd_, &play, sizeof(play)) != sizeof(play)) {
            StopEffect();
            ResetDevice();
            return false;
        }
        return true;
    }

    void StopEffect() {
        if (fd_ < 0 || effect_id_ < 0) return;
        input_event stop = {};
        stop.type = EV_FF;
        stop.code = effect_id_;
        stop.value = 0;
        write(fd_, &stop, sizeof(stop));
        ioctl(fd_, EVIOCRMFF, effect_id_);
        effect_id_ = -1;
    }

    void ResetDevice() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        effect_id_ = -1;
        supports_rumble_ = false;
        supports_constant_ = false;
    }

    void WorkerLoop() {
        uint64_t handled_sequence = 0;
        for (;;) {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [&] { return request_sequence_ != handled_sequence; });
            handled_sequence = request_sequence_;
            const int duration_ms = duration_ms_;
            lock.unlock();

            StopEffect();
            if (!StartEffect(duration_ms)) continue;

            lock.lock();
            condition_.wait_for(lock, std::chrono::milliseconds(duration_ms),
                                [&] { return request_sequence_ != handled_sequence; });
            lock.unlock();
            StopEffect();
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    uint64_t request_sequence_ = 0;
    int duration_ms_ = VIBRATOR_TIME_MS;
    int fd_ = -1;
    int effect_id_ = -1;
    bool supports_rumble_ = false;
    bool supports_constant_ = false;
    bool missing_device_logged_ = false;
};

}  // namespace
#endif

int vibrate(int timeout_ms)
{
    if (timeout_ms > 10000) timeout_ms = 1000;
    if (timeout_ms < 1) return 0;
    char tout[6];
    sprintf(tout, "%i", timeout_ms);

#ifdef USE_INPUT_FF_HAPTICS
    InputForceFeedbackHaptics::Instance().Pulse(timeout_ms);
#elif defined(USE_QTI_HAPTICS)
    android::sp<android::hardware::vibrator::V1_2::IVibrator> vib = android::hardware::vibrator::V1_2::IVibrator::getService();
    if (vib != nullptr) {
        vib->on((uint32_t)timeout_ms);
    }
#elif defined(USE_QTI_AIDL_HAPTICS)
    // getService() waits for several seconds when live fastbootd has
    // intentionally stopped a vendor/odm-backed vibrator HAL. Haptics run on
    // the UI event path, so that wait stalls every touch and makes the whole
    // interface appear frozen. A missing vibrator is optional: probe without
    // waiting and simply skip this pulse until recovery restores the service.
    std::shared_ptr<IVibrator> vib = IVibrator::fromBinder(
        ndk::SpAIBinder(AServiceManager_checkService(kVibratorInstance.c_str())));
    if (vib != nullptr) {
#ifdef USE_QTI_AIDL_HAPTICS_FIX_OFF
        std::thread([vib, timeout_ms] {
            if (vib->on((uint32_t)timeout_ms, nullptr).isOk()) {
                vib_on_count++;
                usleep(timeout_ms * 1000);
                vib_on_count--;
                if (!vib_on_count) vib->off();
            }
        }).detach();
#else
        vib->on((uint32_t)timeout_ms, nullptr);
#endif
    }
#elif defined(USE_SAMSUNG_HAPTICS)
    /* Newer Samsung devices have duration file only
     0 in VIBRATOR_TIMEOUT_FILE means no vibration
     Anything else is the vibration running for X milliseconds */
    if (std::ifstream(VIBRATOR_TIMEOUT_FILE).good()) {
        write_to_file(VIBRATOR_TIMEOUT_FILE, tout);
    }
#else
    if (std::ifstream(LEDS_HAPTICS_ACTIVATE_FILE).good()) {
        write_to_file(LEDS_HAPTICS_DURATION_FILE, tout);
        write_to_file(LEDS_HAPTICS_ACTIVATE_FILE, "1");
    } else
        write_to_file(VIBRATOR_TIMEOUT_FILE, tout);
#endif
    return 0;
}
#endif
#endif

/* Returns empty tokens */
static char *vk_strtok_r(char *str, const char *delim, char **save_str)
{
    if(!str)
    {
        if(!*save_str)
            return NULL;

        str = (*save_str) + 1;
    }
    *save_str = strpbrk(str, delim);

    if (*save_str)
        **save_str = '\0';

    return str;
}

static int vk_init(struct ev *e)
{
    char vk_path[PATH_MAX] = "/sys/board_properties/virtualkeys.";
    char vks[2048], *ts = NULL;
    ssize_t len;
    int vk_fd;
    int i;

    e->vk_count = 0;

    len = strlen(vk_path);
    len = ioctl(e->fd->fd, EVIOCGNAME(sizeof(e->deviceName)), e->deviceName);
    if (len <= 0)
    {
        LOGE("Unable to query event object.\n");
        return -1;
    }
#ifdef _EVENT_LOGGING
    printf("Event object: %s\n", e->deviceName);
#endif

#ifdef WHITELIST_INPUT
    if (strcmp(e->deviceName, EXPAND(WHITELIST_INPUT)) != 0)
    {
        e->ignored = 1;
    }
#else
#ifndef TW_INPUT_BLACKLIST
    // Blacklist these "input" devices, use TW_INPUT_BLACKLIST := "accelerometer\x0atest1\x0atest2" using the \x0a as a separator between input devices
    if (strcmp(e->deviceName, "bma250") == 0 || strcmp(e->deviceName, "bma150") == 0)
    {
        LOGI("Blacklisting input device: %s\n", e->deviceName);
        e->ignored = 1;
    }
#else
    char* bl = strdup(EXPAND(TW_INPUT_BLACKLIST));
    char* blacklist = strtok(bl, "\n");

    while (blacklist != NULL) {
        if (strcmp(e->deviceName, blacklist) == 0) {
            LOGI("Blacklisting input device: %s\n", blacklist);
            e->ignored = 1;
        }
        blacklist = strtok(NULL, "\n");
    }
    free(bl);
#endif
#endif

    strcat(vk_path, e->deviceName);

    // Some devices split the keys from the touchscreen
    e->vk_count = 0;
    vk_fd = open(vk_path, O_RDONLY);
    if (vk_fd >= 0)
    {
        len = read(vk_fd, vks, sizeof(vks)-1);
        close(vk_fd);
        if (len <= 0)
            return -1;

        vks[len] = '\0';

        /* Parse a line like:
            keytype:keycode:centerx:centery:width:height:keytype2:keycode2:centerx2:...
        */
        for (ts = vks, e->vk_count = 1; *ts; ++ts) {
            if (*ts == ':')
                ++e->vk_count;
        }

        if (e->vk_count % 6) {
            LOGI("minui: %s is %d %% 6\n", vk_path, e->vk_count % 6);
        }
        e->vk_count /= 6;
        if (e->vk_count <= 0)
            return -1;

        e->down = DOWN_NOT;
    }

    ioctl(e->fd->fd, EVIOCGABS(ABS_X), &e->p.xi);
    ioctl(e->fd->fd, EVIOCGABS(ABS_Y), &e->p.yi);
    e->p.synced = 0;
    e->p.valid = 0;
#ifdef _EVENT_LOGGING
    printf("EV: ST minX: %d  maxX: %d  minY: %d  maxY: %d\n", e->p.xi.minimum, e->p.xi.maximum, e->p.yi.minimum, e->p.yi.maximum);
#endif

    ioctl(e->fd->fd, EVIOCGABS(ABS_MT_POSITION_X), &e->mt_p.xi);
    ioctl(e->fd->fd, EVIOCGABS(ABS_MT_POSITION_Y), &e->mt_p.yi);
    e->mt_p.synced = 0;
    e->mt_p.valid = 0;
    e->mt_slots[0].xi = e->mt_slots[1].xi = e->mt_p.xi;
    e->mt_slots[0].yi = e->mt_slots[1].yi = e->mt_p.yi;
    e->mt_slots[0].synced = e->mt_slots[1].synced = 0;
    e->mt_slot = 0;
    e->mt_secondary_slot = -1;
    e->mt_active[0] = e->mt_active[1] = false;
    e->mt_dirty[0] = e->mt_dirty[1] = false;
#ifdef _EVENT_LOGGING
    printf("EV: MT minX: %d  maxX: %d  minY: %d  maxY: %d\n", e->mt_p.xi.minimum, e->mt_p.xi.maximum, e->mt_p.yi.minimum, e->mt_p.yi.maximum);
#endif

    e->vks = (virtualkey *)malloc(sizeof(*e->vks) * e->vk_count);

    for (i = 0; i < e->vk_count; ++i) {
        char *token[6];
        int j;

        for (j = 0; j < 6; ++j) {
            token[j] = vk_strtok_r((i||j)?NULL:vks, ":", &ts);
        }

        if (strcmp(token[0], "0x01") != 0) {
            /* Java does string compare, so we do too. */
            LOGI("minui: %s: ignoring unknown virtual key type %s\n", vk_path, token[0]);
            continue;
        }

        e->vks[i].scancode = strtol(token[1], NULL, 0);
        e->vks[i].centerx = strtol(token[2], NULL, 0);
        e->vks[i].centery = strtol(token[3], NULL, 0);
        e->vks[i].width = strtol(token[4], NULL, 0);
        e->vks[i].height = strtol(token[5], NULL, 0);
    }

    return 0;
}

#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array)	((array[LONG(bit)] >> OFF(bit)) & 1)

// Check for EV_REL (REL_X and REL_Y) and, because touchscreens can have those too,
// check also for EV_KEY (BTN_LEFT and BTN_RIGHT)
static bool check_mouse(int fd, const char* deviceName)
{

	unsigned long bit[EV_MAX][NBITS(KEY_MAX)];
	memset(bit, 0, sizeof(bit));
	ioctl(fd, EVIOCGBIT(0, EV_MAX), bit[0]);

	if(!test_bit(EV_REL, bit[0]) || !test_bit(EV_KEY, bit[0]))
		return false;

	ioctl(fd, EVIOCGBIT(EV_REL, KEY_MAX), bit[EV_REL]);
	if(!test_bit(REL_X, bit[EV_REL]) || !test_bit(REL_Y, bit[EV_REL]))
		return false;

	ioctl(fd, EVIOCGBIT(EV_KEY, KEY_MAX), bit[EV_KEY]);
	if(!test_bit(BTN_LEFT, bit[EV_KEY]))
		return false;

	LOGI("Found mouse '%s'\n", deviceName);
	has_mouse = 1;
	return true;
}

int ev_has_mouse(void)
{
	return has_mouse;
}

static unsigned input_generation;
unsigned ev_input_generation(void) { return input_generation; }

int ev_init(void)
{
    ++input_generation;
    DIR *dir;
    struct dirent *de;
    int fd;

    has_mouse = 0;

	dir = opendir("/dev/input");
    if(dir != 0) {
        while((de = readdir(dir))) {
#ifdef _EVENT_LOGGING
            fprintf(stderr,"/dev/input/%s\n", de->d_name);
#endif
            if(strncmp(de->d_name,"event",5)) continue;
            fd = openat(dirfd(dir), de->d_name, O_RDONLY);
            if(fd < 0) continue;

			ev_fds[ev_count].fd = fd;
            ev_fds[ev_count].events = POLLIN;
            evs[ev_count].fd = &ev_fds[ev_count];

            /* Load virtualkeys if there are any */
            vk_init(&evs[ev_count]);

            evs[ev_count].mouse = !evs[ev_count].ignored &&
                check_mouse(fd, evs[ev_count].deviceName);

            ev_count++;
            if(ev_count == MAX_DEVICES) break;
        }
        closedir(dir);
    }

    struct stat st;
    if(stat("/dev/input", &st) >= 0)
        lastInputMTime = st.st_mtime;
    gettimeofday(&lastInputStat, NULL);

    return 0;
}

void ev_exit(void)
{
	while (ev_count-- > 0) {
		if (evs[ev_count].vk_count) {
			free(evs[ev_count].vks);
			evs[ev_count].vk_count = 0;
		}
		close(ev_fds[ev_count].fd);
	}
	ev_count = 0;
}

/*static int vk_inside_display(__s32 value, struct input_absinfo *info, int screen_size)
{
    int screen_pos;

    if (info->minimum == info->maximum)
        return 0;

    screen_pos = (value - info->minimum) * (screen_size - 1) / (info->maximum - info->minimum);
    return (screen_pos >= 0 && screen_pos < screen_size);
}*/

static int vk_tp_to_screen(struct position *p, int *x, int *y)
{
    if (p->xi.minimum == p->xi.maximum || p->yi.minimum == p->yi.maximum)
    {
        // In this case, we assume the screen dimensions are the same.
        *x = p->x;
        *y = p->y;
        return 0;
    }

#ifdef _EVENT_LOGGING
    printf("EV: p->x=%d  x-range=%d,%d  fb-width=%d\n", p->x, p->xi.minimum, p->xi.maximum, gr_fb_width());
#endif

#ifndef RECOVERY_TOUCHSCREEN_SWAP_XY
    int fb_width = gr_fb_width();
    int fb_height = gr_fb_height();
#else
    // We need to swap the scaling sizes, too
    int fb_width = gr_fb_height();
    int fb_height = gr_fb_width();
#endif

    *x = (p->x - p->xi.minimum) * (fb_width - 1) / (p->xi.maximum - p->xi.minimum);
    *y = (p->y - p->yi.minimum) * (fb_height - 1) / (p->yi.maximum - p->yi.minimum);

    if (*x >= 0 && *x < fb_width &&
        *y >= 0 && *y < fb_height)
    {
        return 0;
    }

    return 1;
}

/* Translate a virtual key in to a real key event, if needed */
/* Returns non-zero when the event should be consumed */
static int vk_modify(struct ev *e, struct input_event *ev)
{
    static int downX = -1, downY = -1;
    static int discard = 0;
    static int last_virt_key = 0;
    static int lastWasSynReport = 0;
    static int touchReleaseOnNextSynReport = 0;
	static int use_tracking_id_negative_as_touch_release = 0; // On some devices, type: 3  code: 39  value: -1, aka EV_ABS ABS_MT_TRACKING_ID -1 indicates a true touch release
    int i;
    int x, y;

    // This is used to ditch useless event handlers, like an accelerometer
    if (e->ignored)     return 1;
    // Mouse SYN packets must never enter touchscreen contact synthesis.
    if (e->mouse) {
        return !((ev->type == EV_REL &&
                  (ev->code == REL_X || ev->code == REL_Y || ev->code == REL_WHEEL)) ||
                 (ev->type == EV_KEY && (ev->code < BTN_MISC ||
                    (ev->code >= BTN_MOUSE && ev->code < BTN_JOYSTICK))));
    }

    if (ev->type == EV_REL && ev->code == REL_Z)
    {
        // This appears to be an accelerometer or another strange input device. It's not the touchscreen.
#ifdef _EVENT_LOGGING
        printf("EV: Device disabled due to non-touchscreen messages.\n");
#endif
        e->ignored = 1;
        return 1;
    }

#ifdef _EVENT_LOGGING
    printf("EV: %s => type: %x  code: %x  value: %d\n", e->deviceName, ev->type, ev->code, ev->value);
#endif

	// Handle keyboard events, value of 1 indicates key down, 0 indicates key up
    if (ev->type == EV_KEY) {
		return 0;
	}

    // Preserve one non-primary type-B contact for AERA multitouch. Hardware
    // is free to assign slot 1, 2, or higher, so claim the first active
    // nonzero slot instead of assuming that the second finger is always 1.
    if (ev->type == EV_ABS && ev->code == ABS_MT_SLOT) {
        e->mt_slot = ev->value;
        return 1;
    }
    if (ev->type == EV_ABS && e->mt_slot != 0) {
        if (e->mt_secondary_slot < 0 &&
            (ev->code != ABS_MT_TRACKING_ID || ev->value >= 0))
            e->mt_secondary_slot = e->mt_slot;
        if (e->mt_slot != e->mt_secondary_slot)
            return 1;
        switch (ev->code) {
        case ABS_MT_POSITION_X:
            e->mt_slots[1].x = ev->value;
            e->mt_slots[1].synced |= 0x01;
            e->mt_dirty[1] = true;
            break;
        case ABS_MT_POSITION_Y:
            e->mt_slots[1].y = ev->value;
            e->mt_slots[1].synced |= 0x02;
            e->mt_dirty[1] = true;
            break;
        case ABS_MT_TRACKING_ID:
            e->mt_active[1] = ev->value >= 0;
            e->mt_dirty[1] = true;
            break;
        default:
            break;
        }
        return 1;
    }

    if (ev->type == EV_SYN && ev->code == SYN_REPORT && e->mt_dirty[1] &&
        (e->mt_slots[1].synced & 0x03) == 0x03) {
        int second_x = -1;
        int second_y = -1;
        if (!vk_tp_to_screen(&e->mt_slots[1], &second_x, &second_y)) {
#ifdef RECOVERY_TOUCHSCREEN_SWAP_XY
            second_x ^= second_y;
            second_y ^= second_x;
            second_x ^= second_y;
#endif
#ifdef RECOVERY_TOUCHSCREEN_FLIP_X
            second_x = gr_fb_width() - second_x;
#endif
#ifdef RECOVERY_TOUCHSCREEN_FLIP_Y
            second_y = gr_fb_height() - second_y;
#endif
            secondary_touch_event = {};
            secondary_touch_event.type = EV_ABS;
            secondary_touch_event.code = e->mt_active[1] ? 3 : 2;
            secondary_touch_event.value = (second_x << 16) | second_y;
            secondary_touch_pending = true;
        }
        e->mt_dirty[1] = false;
        if (!e->mt_active[1])
            e->mt_secondary_slot = -1;
    }

    if (ev->type == EV_ABS) {
        switch (ev->code) {

        case ABS_X: //00
            e->p.synced |= 0x01;
            e->p.valid |= 0x01;
            e->p.x = ev->value;
#ifdef _EVENT_LOGGING
            printf("EV: %s => EV_ABS  ABS_X  %d\n", e->deviceName, ev->value);
#endif
            break;

        case ABS_Y: //01
            e->p.synced |= 0x02;
            e->p.valid |= 0x02;
            e->p.y = ev->value;
#ifdef _EVENT_LOGGING
            printf("EV: %s => EV_ABS  ABS_Y  %d\n", e->deviceName, ev->value);
#endif
            break;

        case ABS_MT_POSITION: //2a
            e->mt_p.synced = 0x03;
            if (ev->value == (1 << 31))
            {
#ifndef TW_IGNORE_MT_POSITION_0
                lastWasSynReport = 1;
#endif
#ifdef _EVENT_LOGGING
#ifndef TW_IGNORE_MT_POSITION_0
                printf("EV: %s => EV_ABS  ABS_MT_POSITION  %d, touch released\n", e->deviceName, ev->value);
#else
                printf("Ignoring ABS_MT_POSITION 0\n", e->deviceName, ev->value);
#endif
#endif
            }
            else
            {
                lastWasSynReport = 0;
                e->mt_p.valid = 0x03;
                e->mt_p.x = (ev->value & 0x7FFF0000) >> 16;
                e->mt_p.y = (ev->value & 0xFFFF);
#ifdef _EVENT_LOGGING
                printf("EV: %s => EV_ABS  ABS_MT_POSITION  %d, set x: %d and y: %d and lastWasSynReport to 0\n", e->deviceName, ev->value, (ev->value & 0x7FFF0000) >> 16, (ev->value & 0xFFFF));
#endif
            }
            break;

        case ABS_MT_TOUCH_MAJOR: //30
            if (ev->value == 0)
            {
#ifndef TW_IGNORE_MAJOR_AXIS_0
                // We're in a touch release, although some devices will still send positions as well
                touchReleaseOnNextSynReport = 1;
#endif
            }
#ifdef _EVENT_LOGGING
            printf("EV: %s => EV_ABS  ABS_MT_TOUCH_MAJOR  %d\n", e->deviceName, ev->value);
#endif
            break;

		case ABS_MT_PRESSURE: //3a
                    if (ev->value == 0)
            {
                // We're in a touch release, although some devices will still send positions as well
                touchReleaseOnNextSynReport = 1;
            }
#ifdef _EVENT_LOGGING
            printf("EV: %s => EV_ABS  ABS_MT_PRESSURE  %d\n", e->deviceName, ev->value);
#endif
            break;

        case ABS_MT_POSITION_X: //35
            e->mt_p.synced |= 0x01;
            e->mt_p.valid |= 0x01;
            e->mt_p.x = ev->value;
#ifdef _EVENT_LOGGING
            printf("EV: %s => EV_ABS  ABS_MT_POSITION_X  %d\n", e->deviceName, ev->value);
#endif
            break;

        case ABS_MT_POSITION_Y: //36
            e->mt_p.synced |= 0x02;
            e->mt_p.valid |= 0x02;
            e->mt_p.y = ev->value;
#ifdef _EVENT_LOGGING
            printf("EV: %s => EV_ABS  ABS_MT_POSITION_Y  %d\n", e->deviceName, ev->value);
#endif
            break;

        case ABS_MT_TOUCH_MINOR: //31
#ifdef _EVENT_LOGGING
            printf("EV: %s => EV_ABS ABS_MT_TOUCH_MINOR %d\n", e->deviceName, ev->value);
#endif
            break;

        case ABS_MT_WIDTH_MAJOR: //32
#ifdef _EVENT_LOGGING
            printf("EV: %s => EV_ABS ABS_MT_WIDTH_MAJOR %d\n", e->deviceName, ev->value);
#endif
            break;

        case ABS_MT_WIDTH_MINOR: //33
#ifdef _EVENT_LOGGING
            printf("EV: %s => EV_ABS ABS_MT_WIDTH_MINOR %d\n", e->deviceName, ev->value);
#endif
            break;

        case ABS_MT_TRACKING_ID: //39
#ifdef TW_IGNORE_ABS_MT_TRACKING_ID
#ifdef _EVENT_LOGGING
            printf("EV: %s => EV_ABS ABS_MT_TRACKING_ID %d ignored\n", e->deviceName, ev->value);
#endif
            return 1;
#endif
            if (ev->value < 0) {
                // ABS axes retain their last value when an unchanged axis is
                // omitted from a later report. A release is not a (0,0) sample.
                touchReleaseOnNextSynReport = 2;
                use_tracking_id_negative_as_touch_release = 1;
#ifdef _EVENT_LOGGING
                if (use_tracking_id_negative_as_touch_release)
                    printf("using ABS_MT_TRACKING_ID value -1 to indicate touch releases\n");
#endif
            }
#ifdef _EVENT_LOGGING
            printf("EV: %s => EV_ABS ABS_MT_TRACKING_ID %d\n", e->deviceName, ev->value);
#endif
            break;

#ifdef _EVENT_LOGGING
        // These are for touch logging purposes only
        case ABS_MT_ORIENTATION: //34
            printf("EV: %s => EV_ABS ABS_MT_ORIENTATION %d\n", e->deviceName, ev->value);
			return 1;
            break;

		case ABS_MT_TOOL_TYPE: //37
            LOGI("EV: %s => EV_ABS ABS_MT_TOOL_TYPE %d\n", e->deviceName, ev->value);
			return 1;
            break;

        case ABS_MT_BLOB_ID: //38
            printf("EV: %s => EV_ABS ABS_MT_BLOB_ID %d\n", e->deviceName, ev->value);
			return 1;
            break;

		case ABS_MT_DISTANCE: //3b
            printf("EV: %s => EV_ABS ABS_MT_DISTANCE %d\n", e->deviceName, ev->value);
			return 1;
            break;
        case ABS_MT_SLOT:
            printf("EV: %s => ABS_MT_SLOT %d\n", e->deviceName, ev->value);
			return 1;
			break;
#endif

        default:
            // This is an unhandled message, just skip it
            return 1;
        }

        if (ev->code != ABS_MT_POSITION)
        {
            lastWasSynReport = 0;
            return 1;
        }
    }

    // Check if we should ignore the message
    if (ev->code != ABS_MT_POSITION && (ev->type != EV_SYN || (ev->code != SYN_REPORT && ev->code != SYN_MT_REPORT)))
    {
        lastWasSynReport = 0;
        return 0;
    }

#ifdef _EVENT_LOGGING
    if (ev->type == EV_SYN && ev->code == SYN_REPORT)
        printf("EV: %s => EV_SYN  SYN_REPORT\n", e->deviceName);
    if (ev->type == EV_SYN && ev->code == SYN_MT_REPORT)
        printf("EV: %s => EV_SYN  SYN_MT_REPORT\n", e->deviceName);
#endif

    // Discard the MT versions
    if (ev->code == SYN_MT_REPORT)      return 0;

    if (((lastWasSynReport == 1 || touchReleaseOnNextSynReport == 1) && !use_tracking_id_negative_as_touch_release) || (use_tracking_id_negative_as_touch_release && touchReleaseOnNextSynReport == 2))
    {
        // Reset the value
        touchReleaseOnNextSynReport = 0;
        e->p.synced = e->mt_p.synced = 0;
        if (downX < 0 && !discard)
            return 1;

        // We are a finger-up state
        if (!discard)
        {
            // Report the key up
            ev->type = EV_ABS;
            ev->code = 0;
            ev->value = (downX << 16) | downY;
        }
        downX = -1;
        downY = -1;
        if (discard)
        {
            discard = 0;

            // Send the keyUp event
            ev->type = EV_KEY;
            ev->code = last_virt_key;
            ev->value = 0;
        }
        return 0;
    }
    lastWasSynReport = 1;

    // Retrieve where the x,y position is
    if ((e->p.synced & 0x03) && e->p.valid == 0x03)
    {
        vk_tp_to_screen(&e->p, &x, &y);
    }
    else if ((e->mt_p.synced & 0x03) && e->mt_p.valid == 0x03)
    {
        vk_tp_to_screen(&e->mt_p, &x, &y);
    }
    else
    {
        // We don't have useful information to convey
        return 1;
    }

#ifdef RECOVERY_TOUCHSCREEN_SWAP_XY
    x ^= y;
    y ^= x;
    x ^= y;
#endif
#ifdef RECOVERY_TOUCHSCREEN_FLIP_X
    x = gr_fb_width() - x;
#endif
#ifdef RECOVERY_TOUCHSCREEN_FLIP_Y
    y = gr_fb_height() - y;
#endif

#ifdef _EVENT_LOGGING
    printf("EV: x: %d  y: %d\n", x, y);
#endif

    // Clear the current sync states
    e->p.synced = e->mt_p.synced = 0;

    // If we have nothing useful to report, skip it
    if (x == -1 || y == -1)     return 1;

    // Special case, we'll ignore touches on 0,0 because it usually means
    // that we received extra data after our last sync and x and y were
    // reset to 0. We should not be using 0,0 anyway.
    if (x == 0 && y == 0)
        return 1;

    // On first touch, see if we're at a virtual key
    if (downX == -1)
    {
        // Attempt mapping to virtual key
        for (i = 0; i < e->vk_count; ++i)
        {
            int xd = ABS(e->vks[i].centerx - x);
            int yd = ABS(e->vks[i].centery - y);

            if (xd < e->vks[i].width/2 && yd < e->vks[i].height/2)
            {
                ev->type = EV_KEY;
                ev->code = e->vks[i].scancode;
                ev->value = 1;

                last_virt_key = e->vks[i].scancode;

#ifndef TW_NO_HAPTICS
                vibrate(VIBRATOR_TIME_MS);
#endif

                // Mark that all further movement until lift is discard,
                // and make sure we don't come back into this area
                discard = 1;
                downX = 0;
                return 0;
            }
        }
    }

    // If we were originally a button press, discard this event
    if (discard)
    {
        return 1;
    }

    // Record where we started the touch for deciding if this is a key or a scroll
    downX = x;
    downY = y;

    ev->type = EV_ABS;
    ev->code = 1;
    ev->value = (x << 16) | y;
    return 0;
}

int ev_get(struct input_event *ev, int timeout_ms)
{
    int r;
    unsigned n;
    struct timeval curr;

    if (secondary_touch_pending) {
        *ev = secondary_touch_event;
        secondary_touch_pending = false;
        return 0;
    }

    gettimeofday(&curr, NULL);
    if(curr.tv_sec - lastInputStat.tv_sec >= 2)
    {
        struct stat st;
        stat("/dev/input", &st);
        if (st.st_mtime != lastInputMTime)
        {
            LOGI("Reloading input devices\n");
            ev_exit();
            ev_init();
            lastInputMTime = st.st_mtime;
        }
        lastInputStat = curr;
    }

    r = poll(ev_fds, ev_count, timeout_ms);

    if(r > 0) {
        for(n = 0; n < ev_count; n++) {
            if(ev_fds[n].revents & POLLIN) {
                r = read(ev_fds[n].fd, ev, sizeof(*ev));
                if(r == sizeof(*ev)) {
                    if (!vk_modify(&evs[n], ev))
                        return 0;
                }
            }
        }
        return -1;
    }

    return -2;
}

int ev_wait(int timeout __unused)
{
    return -1;
}

void ev_dispatch(void)
{
    return;
}

int ev_get_input(int fd __unused, short revents __unused, struct input_event *ev __unused)
{
    return -1;
}
