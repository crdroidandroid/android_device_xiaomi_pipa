/*
 * Copyright (C) 2023-2025 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <android/log.h>
#include <android/looper.h>
#include <android/sensor.h>
#include <arm_neon.h>  // for fast square root + vector math
#include <ctype.h>     // Add for isspace() function
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>  // Add for signal handling
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>  // Add for time functions
#include <unistd.h>
const char kPackageName[] = "xiaomi-keyboard";

/********************************************
 * Configuration Constants
 ********************************************/
#define BUFFER_SIZE 256
#define NANODEV_PATH "/dev/nanodev0"
#define DEBOUNCE_COUNT 3

/********************************************
 * Message Protocol Definitions
 ********************************************/
#define MSG_TYPE_SLEEP 37
#define MSG_TYPE_WAKE 40
#define MSG_HEADER_1 0x31
#define MSG_HEADER_2 0x38
#define MSG_TYPE_MOVEMENT 0x64

// Lock state message types
#define MSG_TYPE_LOCK 41
#define MSG_TYPE_UNLOCK 42

// Device path
// We'll find this dynamically
char* EVENT_PATH = NULL;

// Simplify by keeping only essential logging macros
#define TAG "xiaomi-keyboard"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

// Keep just one enhanced logging macro for important events
#define LOG_IMPORTANT(fmt, ...)                                         \
  do {                                                                  \
    time_t now = time(NULL);                                            \
    struct tm* tm_info = localtime(&now);                               \
    char time_str[20];                                                  \
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info); \
    __android_log_print(ANDROID_LOG_INFO, TAG, "[%s] " fmt, time_str,   \
                        ##__VA_ARGS__);                                 \
  } while (0)

// Nanodev file
int fd;

// Current kb enabled/disabled state
bool kb_status = true;

// Sensor variables
const ASensor* sensor;
ASensorEventQueue* queue;

float padX = 0;
float padY = 0;
float padZ = 0;
float kbX = 0;
float kbY = 0;
float kbZ = 0;

// Add signal handler for graceful termination - MOVED HERE
volatile sig_atomic_t terminate = 0;

// Condition variable for pausing and resuming the kb, sensor threads
pthread_mutex_t kb_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t kb_cond = PTHREAD_COND_INITIALIZER;
bool kb_thread_paused = false;

// Condition variable for the sensor thread
pthread_mutex_t sensor_mutex = PTHREAD_MUTEX_INITIALIZER;

// For the preference thread
pthread_mutex_t angle_detection_mutex = PTHREAD_MUTEX_INITIALIZER;

// Add these global variables
time_t last_monitor_activity = time(NULL);
pthread_t watchdog_thread;
bool watchdog_enabled = true;

// Angle detection enable flag
bool angle_detection_enabled = false;  // Default value

void load_angle_detection_preference() {
  FILE* f = fopen("/data/misc/xiaomi_keyboard.conf", "r");
  if (f) {
    int c = fgetc(f);
    pthread_mutex_lock(&angle_detection_mutex);
    angle_detection_enabled = (c == '1');
    pthread_mutex_lock(&angle_detection_mutex);
    fclose(f);
    LOGI("Angle detection preference loaded: %s",
         angle_detection_enabled ? "enabled" : "disabled");
  } else {
    LOGW(
        "Could not open /data/misc/xiaomi_keyboard.conf, using default "
        "(enabled)");
    // angle_detection_enabled = true;
  }
}

void* preference_watcher_thread(void*) {
  while (!terminate) {
    load_angle_detection_preference();
    sleep(10);  // Reload every 10 seconds
  }
  return NULL;
}

// Globals
static ASensorManager* sensorManager = NULL;
static const ASensor* accelerometer = NULL;
static ASensorEventQueue* sensorQueue = NULL;
static ALooper* looper = NULL;

void* accelerometer_thread(void* args) {
  pthread_mutex_lock(&kb_mutex);
  while (kb_thread_paused && !terminate) {
    pthread_cond_wait(&kb_cond, &kb_mutex);
  }
  pthread_mutex_unlock(&kb_mutex);

  sensorManager = ASensorManager_getInstanceForPackage(
      "org.lineageos.xiaomiperipheralmanager");
  accelerometer = ASensorManager_getDefaultSensor(sensorManager,
                                                  ASENSOR_TYPE_ACCELEROMETER);

  if (!accelerometer) {
    LOGI("Accelerometer not available");
    return NULL;
  }

  looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
  sensorQueue =
      ASensorManager_createEventQueue(sensorManager, looper, 0, NULL, NULL);
  ASensorEventQueue_enableSensor(sensorQueue, accelerometer);
  ASensorEventQueue_setEventRate(sensorQueue, accelerometer,
                                 ASensor_getMinDelay(accelerometer));

  while (!terminate) {
    ALooper_pollOnce(500, NULL, NULL, NULL);
    if (terminate) break;

    ASensorEvent event;
    while (ASensorEventQueue_getEvents(sensorQueue, &event, 1) > 0) {
      if (event.type == ASENSOR_TYPE_ACCELEROMETER) {
        pthread_mutex_lock(&sensor_mutex);
        padX = event.acceleration.x;
        padY = event.acceleration.y;
        padZ = event.acceleration.z;
        pthread_mutex_unlock(&sensor_mutex);
      }
    }
  }

  // Unreachable, but good practice
  ASensorEventQueue_disableSensor(sensorQueue, accelerometer);
  ASensorManager_destroyEventQueue(sensorManager, sensorQueue);
  return NULL;
}

static inline float neon_sqrtf(float x) {
  float32x2_t val = vdup_n_f32(x);
  float32x2_t res = vsqrt_f32(val);
  return vget_lane_f32(res, 0);
}

static inline float fast_acosf(float x) {
  if (x < -1.0f) x = -1.0f;
  if (x > 1.0f) x = 1.0f;

  float negate = (x < 0.0f);
  x = fabsf(x);

  float ret = -0.0187293f;
  ret = ret * x + 0.0742610f;
  ret = ret * x - 0.2121144f;
  ret = ret * x + 1.5707288f;
  ret = ret * neon_sqrtf(1.0f - x);

  return negate ? (M_PI - ret) : ret;
}

/**
 * Find the keyboard event input device path
 * This replaces the hardcoded path with dynamic detection
 */
char* find_keyboard_input_path() {
  static char path_buffer[128] = "/dev/input/event12";
  const char* input_dir = "/dev/input";
  DIR* dir = opendir(input_dir);

  if (!dir) {
    LOGE("Failed to open input directory");
    return path_buffer;
  }

  FILE* device_file;
  char name_path[128];
  char device_name[256];
  struct dirent* entry;

  // Simplified detection criteria with key terms
  const char* keyboard_identifiers[] = {"xiaomi", "keyboard", "pipa", "XKBD"};
  const int num_identifiers = 4;

  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "event", 5) == 0) {
      snprintf(name_path, sizeof(name_path), "/sys/class/input/%s/device/name",
               entry->d_name);

      device_file = fopen(name_path, "r");
      if (device_file && fgets(device_name, sizeof(device_name), device_file)) {
        // Convert to lowercase for case-insensitive matching
        for (char* p = device_name; *p; p++) {
          *p = tolower(*p);
        }

        for (int i = 0; i < num_identifiers; i++) {
          if (strstr(device_name, keyboard_identifiers[i])) {
            snprintf(path_buffer, sizeof(path_buffer), "/dev/input/%s",
                     entry->d_name);
            LOGI("Found keyboard at: %s", path_buffer);
            fclose(device_file);
            closedir(dir);
            return path_buffer;
          }
        }
        fclose(device_file);
      }
    }
  }

  closedir(dir);
  LOGW("Could not find keyboard device, using default path");
  return path_buffer;
}

/**
 * Set keyboard state directly
 */
void set_kb_state(bool value, bool force) {
  if (kb_status != value || force) {
    kb_status = value;
    LOGI("Setting keyboard state to: %d", value);

    // Add fd validation before attempting write
    if (fd < 0) {
      LOGE("Invalid file descriptor (fd=%d) when setting keyboard state", fd);
      return;
    }

    unsigned char buf[3] = {0x32, 0xFF, (unsigned char)value};
    ssize_t bytes_written = write(fd, &buf, 3);

    if (bytes_written != 3) {
      // Enhanced error logging with errno details
      LOGE("Failed to write keyboard state: %s (errno=%d, written=%zd/3)",
           strerror(errno), errno, bytes_written);

      // Log device status
      struct stat st;
      if (fstat(fd, &st) == 0) {
        LOGI("Device status: mode=%o, size=%lld, uid=%d, gid=%d", st.st_mode,
             (long long)st.st_size, st.st_uid, st.st_gid);
      } else {
        LOGE("Unable to stat device: %s", strerror(errno));
      }
    } else {
      LOGI("Successfully wrote keyboard state: %d", value);
    }
  }
}

// Improved keyboard status monitoring with debouncing

// Add this global variable to track device lock state
bool device_is_locked = false;

void* keyboard_monitor_thread(void* arg) {
  (void)arg;

  int connection_state_count = 0;
  bool last_state = access(EVENT_PATH, F_OK) != -1;

  struct timespec timeout;

  while (!terminate) {
    // Check whether the watchdog thread should be paused
    pthread_mutex_lock(&kb_mutex);
    while (kb_thread_paused && !terminate) {
      pthread_cond_wait(&kb_cond, &kb_mutex);
    }
    pthread_mutex_unlock(&kb_mutex);

    if (terminate) break;

    // Check keyboard connection state
    bool current_state = (access(EVENT_PATH, F_OK) != -1);

    if (current_state != last_state) {
      connection_state_count++;
      LOGD("Potential keyboard connection change detected (%d/%d)",
           connection_state_count, DEBOUNCE_COUNT);
    } else {
      connection_state_count = 0;
    }

    if (connection_state_count >= DEBOUNCE_COUNT) {
      last_state = current_state;
      connection_state_count = 0;

      pthread_mutex_lock(&kb_mutex);
      if (!kb_thread_paused) {
        if (current_state && !device_is_locked && !kb_status) {
          LOGI("Keyboard connected and device unlocked - enabling");
          set_kb_state(true, false);
        } else if ((!current_state || device_is_locked) && kb_status) {
          LOGI("Keyboard %s - disabling",
               !current_state ? "disconnected" : "disabled due to device lock");
          set_kb_state(false, false);
        }
      }
      pthread_mutex_unlock(&kb_mutex);
    }

    // Always update watchdog activity if not paused
    pthread_mutex_lock(&kb_mutex);
    if (!kb_thread_paused) last_monitor_activity = time(NULL);
    pthread_mutex_unlock(&kb_mutex);

    // Sleep in a responsive pattern (1s total)
    for (int i = 0; i < 5 && !terminate; i++) {
      usleep(200000);
    }
  }
  LOGI("Keyboard monitor thread exiting");
  return NULL;
}

// Add this watchdog thread function
void* watchdog_thread_func(void* arg) {
  (void)arg;  // Suppress unused parameter warning

  const int WATCHDOG_INTERVAL = 30;  // 30 seconds

  LOGI("Watchdog thread started");

  while (!terminate) {
    sleep(10);  // Check every 10 seconds

    time_t now = time(NULL);
    pthread_mutex_lock(&kb_mutex);
    bool is_paused = kb_thread_paused;
    time_t last_activity = last_monitor_activity;
    pthread_mutex_unlock(&kb_mutex);

    // If monitor thread hasn't updated in WATCHDOG_INTERVAL, it might be stuck
    if (!is_paused && watchdog_enabled &&
        now - last_activity > WATCHDOG_INTERVAL) {
      LOGW("Watchdog: Monitor thread appears stuck for %d seconds",
           (int)(now - last_activity));

      // Signal the condition to try to wake up the thread
      pthread_mutex_lock(&kb_mutex);
      pthread_cond_signal(&kb_cond);
      pthread_mutex_unlock(&kb_mutex);
    }
  }

  LOGI("Watchdog thread exiting");
  return NULL;
}

// Define message types for better readability

/**
 * Event handler for wake/sleep messages
 */
// Consider using a simpler mutex lock/unlock pattern
void handle_power_event(char* buffer) {
  bool is_wake = (buffer[6] == 1);

  pthread_mutex_lock(&kb_mutex);
  if (is_wake) {
    kb_thread_paused = false;
    last_monitor_activity = time(NULL);
    pthread_cond_signal(&kb_cond);
  } else {
    kb_thread_paused = true;
  }
  pthread_mutex_unlock(&kb_mutex);

  // Log and handle status after mutex is released
  if (is_wake) {
    LOGI("Received wake event - enabling keyboard monitoring");
    bool keyboard_connected = (access(EVENT_PATH, F_OK) != -1);
    LOGI("Wake: Keyboard %s",
         keyboard_connected ? "connected" : "disconnected");

    // Only enable if the device is not locked and keyboard is connected
    if (keyboard_connected && !device_is_locked) {
      set_kb_state(true, true);
    } else {
      kb_status = false;
      LOGI("Not enabling keyboard on wake: %s",
           device_is_locked ? "device is locked" : "keyboard not connected");
    }
  } else {
    LOGI("Received sleep event - pausing keyboard monitoring");
  }
}

void handle_lock_event(char* buffer) {
  bool is_locked = (buffer[4] == MSG_TYPE_LOCK);

  // Add message validation logging
  LOGI("Received lock event: %s (msg_type=%d)", is_locked ? "LOCK" : "UNLOCK",
       buffer[4]);

  // Log buffer contents for debugging
  char hex_buffer[64] = {0};
  for (int i = 0; i < 7 && i < 20; i++) {
    sprintf(hex_buffer + (i * 3), "%02X ", (unsigned char)buffer[i]);
  }
  LOGD("Lock message buffer: %s", hex_buffer);

  pthread_mutex_lock(&kb_mutex);
  // Update global lock state
  device_is_locked = is_locked;

  if (is_locked) {
    LOGI("Lock event with current kb_status=%d", kb_status);

    if (kb_status) {
      // Check device status before attempting to change state
      if (fd >= 0) {
        // Check if device is writable
        int flags = fcntl(fd, F_GETFL);
        if (flags != -1 && (flags & O_RDWR)) {
          LOGI(
              "Device is opened with read-write access, attempting to disable "
              "keyboard");
          set_kb_state(false, true);
        } else {
          LOGW("Device may not have write permissions (flags=%d)", flags);
          set_kb_state(false, true);  // Try anyway
        }
      } else {
        LOGE("Invalid file descriptor when handling lock event (fd=%d)", fd);
      }

      LOGI("Device locked - disabling keyboard");
    } else {
      LOGI("Device locked but keyboard already disabled");
    }
  } else {
    // Restore previous state if keyboard is connected
    LOGI("Unlock event, checking keyboard presence");
    bool keyboard_present = (access(EVENT_PATH, F_OK) != -1);
    LOGI("Keyboard %s on unlock", keyboard_present ? "present" : "not present");

    if (keyboard_present) {
      // Same device check as above
      if (fd >= 0) {
        LOGI("Attempting to enable keyboard on unlock");
        set_kb_state(true, true);
      } else {
        LOGE("Invalid file descriptor when handling unlock event (fd=%d)", fd);

        // Try to recover the file descriptor
        fd = open(NANODEV_PATH, O_RDWR);
        if (fd != -1) {
          LOGI("Reopened device file on unlock, attempting to enable keyboard");
          set_kb_state(true, true);
        }
      }

      LOGI("Device unlocked - re-enabling keyboard");
    } else {
      LOGW("Not enabling keyboard on unlock - device not present");
    }
  }
  pthread_mutex_unlock(&kb_mutex);
}

float calculateAngle(float kX, float kY, float kZ, float padX, float padY,
                     float padZ) {
  float32x4_t a = {kX, kY, kZ, 0.0f};
  float32x4_t b = {padX, padY, padZ, 0.0f};

  // Dot product
  float32x4_t prod = vmulq_f32(a, b);
  float dot = vgetq_lane_f32(prod, 0) + vgetq_lane_f32(prod, 1) +
              vgetq_lane_f32(prod, 2);

  // Norm of a
  float32x4_t a2 = vmulq_f32(a, a);
  float norm_a_sq =
      vgetq_lane_f32(a2, 0) + vgetq_lane_f32(a2, 1) + vgetq_lane_f32(a2, 2);
  float norm_a = neon_sqrtf(norm_a_sq);

  // Norm of b
  float32x4_t b2 = vmulq_f32(b, b);
  float norm_b_sq =
      vgetq_lane_f32(b2, 0) + vgetq_lane_f32(b2, 1) + vgetq_lane_f32(b2, 2);
  float norm_b = neon_sqrtf(norm_b_sq);

  if (norm_a == 0.0f || norm_b == 0.0f) return 0.0f;

  float cos_theta = dot / (norm_a * norm_b);
  float angle = fast_acosf(cos_theta) * (180.0f / M_PI);

  return angle;
}

void get_kb_accel(char* buffer) {
  int x = ((buffer[7] << 4) & 4080) | ((buffer[6] >> 4) & 15);
  int y = ((buffer[9] << 4) & 4080) | ((buffer[8] >> 4) & 15);
  int z = ((buffer[11] << 4) & 4080) | ((buffer[10] >> 4) & 15);

  if ((x & 2048) == 2048) x = -(4096 - x);
  if ((y & 2048) == 2048) y = -(4096 - y);
  if ((z & 2048) == 2048) z = -(4096 - z);

  float x_normal = (x * 9.8f) / 256.0f;
  float y_normal = ((-y) * 9.8f) / 256.0f;
  float z_normal = ((-z) * 9.8f) / 256.0f;

  float scale = 9.8f / neon_sqrtf(x_normal * x_normal + y_normal * y_normal +
                                  z_normal * z_normal);

  pthread_mutex_lock(&sensor_mutex);
  kbX = x_normal * scale;
  kbY = y_normal * scale;
  kbZ = z_normal * scale;
  pthread_mutex_unlock(&sensor_mutex);
}

void handle_accel_event(char* buffer) {
  float local_padX, local_padY, local_padZ;
  float local_kbX, local_kbY, local_kbZ;
  float last_kbX = 0.0f, last_kbY = 0.0f, last_kbZ = 0.0f;
  const float vector_threshold = 0.04f;

  get_kb_accel(buffer);

  pthread_mutex_lock(&sensor_mutex);
  local_padX = padX;
  local_padY = padY;
  local_padZ = padZ;
  local_kbX = kbX;
  local_kbY = kbY;
  local_kbZ = kbZ;
  pthread_mutex_unlock(&sensor_mutex);

  float dx = local_kbX - last_kbX;
  float dy = local_kbY - last_kbY;
  float dz = local_kbZ - last_kbZ;

  float delta = dx * dx + dy * dy + dz * dz;

  if (delta > vector_threshold) {
    float angle = calculateAngle(local_kbX, local_kbY, local_kbZ, local_padX,
                                 local_padY, local_padZ);
    set_kb_state(!(angle >= 120), false);

    last_kbX = local_kbX;
    last_kbY = local_kbY;
    last_kbZ = local_kbZ;
  }
}

/**
 * Main event handler - dispatches to appropriate handler based on message type
 */
void handle_event(char* buffer, ssize_t bytes_read) {
  pthread_mutex_lock(&angle_detection_mutex);
  bool angle_detection_enabled_local = angle_detection_enabled;
  pthread_mutex_unlock(&angle_detection_mutex);

  // Basic validation
  if (bytes_read < 7 || buffer[1] != MSG_HEADER_1 ||
      buffer[2] != MSG_HEADER_2) {
    return;
  }

  // Handle message based on type
  if (buffer[4] == MSG_TYPE_SLEEP || buffer[4] == MSG_TYPE_WAKE) {
    if (buffer[5] == 1) {
      handle_power_event(buffer);
    }
  } else if (buffer[4] == MSG_TYPE_LOCK || buffer[4] == MSG_TYPE_UNLOCK) {
    handle_lock_event(buffer);
  } else if (buffer[4] == MSG_TYPE_MOVEMENT && angle_detection_enabled_local) {
    LOGI("angle_detection_enabled: %d", angle_detection_enabled_local);
    handle_accel_event(buffer);
  }
}

/**
 * Attempt to reconnect to the device with exponential backoff
 * Returns: file descriptor on success, -1 on failure
 */
int reconnect_device() {
  int attempts = 0;
  const int max_attempts = 5;  // Reduced from 10
  int new_fd = -1;

  LOGI("Starting device reconnection procedure to %s", NANODEV_PATH);

  // Check if device exists
  if (access(NANODEV_PATH, F_OK) != 0) {
    LOGE("Device file %s does not exist: %s", NANODEV_PATH, strerror(errno));
  } else {
    LOGI("Device file exists, checking permissions");
    // Check permissions
    if (access(NANODEV_PATH, R_OK | W_OK) != 0) {
      LOGE("Insufficient permissions for device: %s", strerror(errno));
    } else {
      LOGI("Device has read/write permissions");
    }
  }

  while (attempts < max_attempts && new_fd == -1 && !terminate) {
    LOGI("Reconnect attempt %d/%d", attempts + 1, max_attempts);

    // Log current process permissions
    uid_t uid = getuid();
    gid_t gid = getgid();
    LOGI("Current process: uid=%d, gid=%d, euid=%d, egid=%d", uid, gid,
         geteuid(), getegid());

    new_fd = open(NANODEV_PATH, O_RDWR);

    if (new_fd != -1) {
      LOGI("Successfully reconnected to device (fd=%d)", new_fd);
      return new_fd;
    } else {
      LOGE("Failed to open device: %s (errno=%d)", strerror(errno), errno);
    }

    // Simplified backoff: 1s, 2s, 4s, 4s, 4s
    int sleep_time = (attempts < 3) ? (1 << attempts) : 4;
    LOGI("Sleeping for %d seconds before next attempt", sleep_time);
    sleep(sleep_time);
    attempts++;
  }

  LOGE("Failed to reconnect after %d attempts", attempts);
  return -1;
}

// Add signal handler for graceful termination
void signal_handler(int signum) {
  LOGI("Caught signal %d, terminating...", signum);
  terminate = 1;
}

// Use a cleanup function for consistent resource release
void cleanup_resources(pthread_t monitor_thread, pthread_t watchdog_thread_id /*, pthread_t tab_sensor_thread, pthread_t kb_sensor_thread*/) {
  LOGI("Performing cleanup...");

  pthread_mutex_lock(&kb_mutex);
  terminate = 1;
  pthread_cond_signal(&kb_cond);
  pthread_mutex_unlock(&kb_mutex);

  pthread_join(monitor_thread, NULL);
  if (watchdog_enabled && watchdog_thread_id != 0) {
    pthread_join(watchdog_thread_id, NULL);
  }

  if (fd != -1) {
    close(fd);
    fd = -1;
  }
  ASensorEventQueue_disableSensor(sensorQueue, accelerometer);
  ASensorManager_destroyEventQueue(sensorManager, sensorQueue);
}

#define VERSION_STRING "1.1.0"

/**
 * Main function
 */
int main() {
  // Add program start timestamp
  time_t start_time = time(NULL);
  struct tm* tm_info = localtime(&start_time);
  char time_str[64];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

  LOGI("Xiaomi keyboard service v%s starting at %s", VERSION_STRING, time_str);

  // Load angle detection preference
  load_angle_detection_preference();

  ssize_t bytes_read;
  char buffer[BUFFER_SIZE];

  // Initialize log
  LOG_IMPORTANT("Xiaomi keyboard service starting...");

  // Dynamic path detection
  EVENT_PATH = find_keyboard_input_path();
  LOGI("Using keyboard input path: %s", EVENT_PATH);

  // Open the nanodev device file
  fd = open(NANODEV_PATH, O_RDWR);
  if (fd == -1) {
    LOGE("Error opening nanodev device: %s (errno=%d)", strerror(errno), errno);

    // Add more diagnostic information
    if (access(NANODEV_PATH, F_OK) != 0) {
      LOGE("Device file %s does not exist!", NANODEV_PATH);
    } else {
      LOGE("Device exists but cannot be opened. Checking permissions...");
      if (access(NANODEV_PATH, R_OK | W_OK) != 0) {
        LOGE("Insufficient permissions for device %s", NANODEV_PATH);
      }
    }

    return errno;
  }

  LOGI("Successfully opened device file (fd=%d)", fd);

  // Get and log file status
  struct stat st;
  if (fstat(fd, &st) == 0) {
    LOGI("Device file info: mode=%o, size=%lld, uid=%d, gid=%d", st.st_mode,
         (long long)st.st_size, st.st_uid, st.st_gid);
  }

  // Check current keyboard status
  if (access(EVENT_PATH, F_OK) == -1) {
    kb_status = false;
    LOGW("Keyboard input device not found, starting disabled");
  } else {
    // Only enable if the device is not locked
    if (!device_is_locked) {
      LOGI("Keyboard input device found and device unlocked, starting enabled");
      set_kb_state(true, true);
    } else {
      LOGI("Keyboard input device found but device locked, starting disabled");
      kb_status = false;
    }
  }

  // Create the keyboard monitor thread
  pthread_t monitor_thread;
  if (pthread_create(&monitor_thread, NULL, keyboard_monitor_thread, NULL) !=
      0) {
    LOGE("Failed to create keyboard monitor thread");
    close(fd);
    return EXIT_FAILURE;
  }

  // At the top of main():
  pthread_t watchdog_thread_id = 0;

  // Replace watchdog thread creation with:
  if (watchdog_enabled) {
    if (pthread_create(&watchdog_thread_id, NULL, watchdog_thread_func, NULL) !=
        0) {
      LOGW("Failed to create watchdog thread - continuing without watchdog");
      watchdog_enabled = false;
    }
  } else {
    LOGI("Watchdog disabled by configuration");
  }

  // Create sensor thread
  pthread_t sensor_thread;
  pthread_create(&sensor_thread, NULL, accelerometer_thread, NULL);
  pthread_detach(sensor_thread);

  // Create the preference watching thread
  pthread_t preference_thread;
  pthread_create(&preference_thread, NULL, preference_watcher_thread, NULL);
  pthread_detach(preference_thread);

  // Set up signal handling
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  // Consider adding a maximum number of recoveries
  int recoveries = 0;
  const int MAX_RECOVERIES = 3;

  // Main loop for keyboard events
  LOGI("Main loop starting, ready to receive keyboard events");
  while (!terminate) {
    // Read data from the device
    bytes_read = read(fd, buffer, BUFFER_SIZE);

    if (bytes_read > 0) {
      // Reset recovery counter after successful read
      recoveries = 0;
      // Process the message
      handle_event(buffer, bytes_read);
    } else if (bytes_read == 0) {
      // No data available, sleep before trying again
      usleep(500000);  // 500ms
    } else {
      // Read error occurred
      LOGE("Error reading device: %s", strerror(errno));

      // Check if we've exceeded recovery limit
      if (++recoveries > MAX_RECOVERIES) {
        LOGE("Exceeded maximum recovery attempts, exiting");
        break;
      }

      // Close the current file descriptor
      close(fd);

      // Try to reconnect with backoff
      fd = reconnect_device();

      // If reconnection failed, exit the loop
      if (fd == -1) {
        LOGE("Could not recover device connection, exiting");
        break;
      }
    }
  }

  // Final status report before exit
  time_t end_time = time(NULL);
  double runtime = difftime(end_time, start_time);
  LOGI("Service exiting after running for %.1f seconds", runtime);

  // Cleanup
  cleanup_resources(monitor_thread, watchdog_thread_id);

  return 0;
}