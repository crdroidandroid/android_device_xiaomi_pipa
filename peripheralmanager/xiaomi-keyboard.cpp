/*
 * Copyright (C) 2023-2025 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h> // Add for signal handling
#include <time.h> // Add for time functions
#include <ctype.h> // Add for isspace() function

const char kPackageName[] = "xiaomi-keyboard";

/********************************************
 * Configuration Constants
 ********************************************/
#define BUFFER_SIZE 256
#define NANODEV_PATH "/dev/nanodev0"
#define CONFIG_PATH "/data/local/tmp/xiaomi_keyboard.conf"
#define DEBOUNCE_COUNT 3

/********************************************
 * Message Protocol Definitions
 ********************************************/
#define MSG_TYPE_SLEEP 37
#define MSG_TYPE_WAKE 40
#define MSG_HEADER_1 0x31
#define MSG_HEADER_2 0x38

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
#define LOG_IMPORTANT(fmt, ...) do { \
    time_t now = time(NULL); \
    struct tm* tm_info = localtime(&now); \
    char time_str[20]; \
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info); \
    __android_log_print(ANDROID_LOG_INFO, TAG, "[%s] " fmt, time_str, ##__VA_ARGS__); \
} while(0)

// Nanodev file
int fd;

// Current kb enabled/disabled state
bool kb_status = true;

// Add signal handler for graceful termination - MOVED HERE
volatile sig_atomic_t terminate = 0;

// Condition variable for pausing and resuming the thread
pthread_mutex_t kb_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t kb_cond = PTHREAD_COND_INITIALIZER;
bool kb_thread_paused = false;

// Add these global variables
time_t last_monitor_activity = time(NULL);
pthread_t watchdog_thread;
bool watchdog_enabled = true;

// Add a default config that can be used instead of parsing a file
const bool DEFAULT_WATCHDOG_ENABLED = true;

// Simplify configuration loading
void load_configuration() {
    // Set defaults
    watchdog_enabled = DEFAULT_WATCHDOG_ENABLED;
    
    FILE* config_file = fopen(CONFIG_PATH, "r");
    if (!config_file) {
        LOGI("No configuration file found, using defaults");
        return;
    }
    
    char line[256];
    char key[128], value[128];
    
    while (fgets(line, sizeof(line), config_file) != NULL) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n') continue;
        
        if (sscanf(line, "%127[^=]=%127s", key, value) == 2) {
            // Remove whitespace
            char* p = key + strlen(key) - 1;
            while (p >= key && isspace(*p)) *p-- = '\0';
            
            // Process configuration keys
            if (strcmp(key, "watchdog_enabled") == 0) {
                watchdog_enabled = (strcmp(value, "true") == 0);
                LOGI("Config: watchdog_enabled = %d", watchdog_enabled);
            }
            // Add more configuration options as needed
        }
    }
    
    fclose(config_file);
    LOGI("Configuration loaded from %s", CONFIG_PATH);
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
            snprintf(name_path, sizeof(name_path), 
                     "/sys/class/input/%s/device/name", entry->d_name);
            
            device_file = fopen(name_path, "r");
            if (device_file && fgets(device_name, sizeof(device_name), device_file)) {
                // Convert to lowercase for case-insensitive matching
                for (char* p = device_name; *p; p++) {
                    *p = tolower(*p);
                }
                
                for (int i = 0; i < num_identifiers; i++) {
                    if (strstr(device_name, keyboard_identifiers[i])) {
                        snprintf(path_buffer, sizeof(path_buffer), 
                                 "/dev/input/%s", entry->d_name);
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
                LOGI("Device status: mode=%o, size=%lld, uid=%d, gid=%d", 
                     st.st_mode, (long long)st.st_size, st.st_uid, st.st_gid);
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

void *keyboard_monitor_thread(void *arg) {
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
        if (!kb_thread_paused)
            last_monitor_activity = time(NULL);
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
void *watchdog_thread_func(void *arg) {
    (void)arg; // Suppress unused parameter warning
    
    const int WATCHDOG_INTERVAL = 30; // 30 seconds
    
    LOGI("Watchdog thread started");
    
    while (!terminate) {
        sleep(10); // Check every 10 seconds
        
        time_t now = time(NULL);
        pthread_mutex_lock(&kb_mutex);
        bool is_paused = kb_thread_paused;
        time_t last_activity = last_monitor_activity;
        pthread_mutex_unlock(&kb_mutex);
        
        // If monitor thread hasn't updated in WATCHDOG_INTERVAL, it might be stuck
        if ((is_paused || !watchdog_enabled) && now - last_activity > WATCHDOG_INTERVAL && watchdog_enabled) {
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
void handle_power_event(char *buffer) {
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
        LOGI("Wake: Keyboard %s", keyboard_connected ? "connected" : "disconnected");
        
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

void handle_lock_event(char *buffer) {
    bool is_locked = (buffer[4] == MSG_TYPE_LOCK);
    
    // Add message validation logging
    LOGI("Received lock event: %s (msg_type=%d)", 
         is_locked ? "LOCK" : "UNLOCK", buffer[4]);
    
    // Log buffer contents for debugging
    char hex_buffer[64] = {0};
    for (int i = 0; i < 7 && i < 20; i++) {
        sprintf(hex_buffer + (i*3), "%02X ", (unsigned char)buffer[i]);
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
                    LOGI("Device is opened with read-write access, attempting to disable keyboard");
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

/**
 * Main event handler - dispatches to appropriate handler based on message type
 */
void handle_event(char *buffer, ssize_t bytes_read) {
    // Basic validation
    if (bytes_read < 7 || buffer[1] != MSG_HEADER_1 || buffer[2] != MSG_HEADER_2) {
        return;
    }
    
    // Handle message based on type
    if (buffer[4] == MSG_TYPE_SLEEP || buffer[4] == MSG_TYPE_WAKE) {
        if (buffer[5] == 1) {
            handle_power_event(buffer);
        }
    } else if (buffer[4] == MSG_TYPE_LOCK || buffer[4] == MSG_TYPE_UNLOCK) {
        handle_lock_event(buffer);
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
        LOGI("Current process: uid=%d, gid=%d, euid=%d, egid=%d", 
             uid, gid, geteuid(), getegid());
        
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
void cleanup_resources(pthread_t monitor_thread, pthread_t watchdog_thread_id) {
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
}

#define VERSION_STRING "1.0.0"

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
    
    // Load configuration
    load_configuration();
    
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
        LOGI("Device file info: mode=%o, size=%lld, uid=%d, gid=%d", 
             st.st_mode, (long long)st.st_size, st.st_uid, st.st_gid);
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
    if (pthread_create(&monitor_thread, NULL, keyboard_monitor_thread, NULL) != 0) {
        LOGE("Failed to create keyboard monitor thread");
        close(fd);
        return EXIT_FAILURE;
    }

    // At the top of main():
    pthread_t watchdog_thread_id = 0;

    // Replace watchdog thread creation with:
    if (watchdog_enabled) {
        if (pthread_create(&watchdog_thread_id, NULL, watchdog_thread_func, NULL) != 0) {
            LOGW("Failed to create watchdog thread - continuing without watchdog");
            watchdog_enabled = false;
        }
    } else {
        LOGI("Watchdog disabled by configuration");
    }

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
        } 
        else if (bytes_read == 0) {
            // No data available, sleep before trying again
            usleep(100000); // 100ms
        } 
        else {
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