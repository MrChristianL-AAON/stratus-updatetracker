/*
* Copyright 2024 NXP
* NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
* accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
* activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
* terms, then you may not retain, install, activate or otherwise use the software.
*/


/*********************
 *      INCLUDES
 *********************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "lvgl.h"
#include "../generated/update_tracker.h"
#include "custom.h"

/* Check if we're compiling for simulator mode */
#if defined(SIMULATOR) || defined(BUILD_SIMULATOR) || (!defined(__linux__) && !defined(CONFIG_ZEPHYR_LVGL) && !defined(CONFIG_ZEPHYR_KERNEL))
    /* Standard C library inclusions for simulator */
    #include <sys/stat.h>
    #include <sys/types.h>
    #if defined(_WIN32) || defined(_WIN64)
        #include <direct.h>
        #define mkdir(dir, mode) _mkdir(dir)
    #endif
    #define IS_SIMULATOR 1
#else
    /* Non-simulator target */
    #if defined(CONFIG_ZEPHYR_LVGL) || defined(CONFIG_ZEPHYR_KERNEL)
        #include <zephyr/fs/fs.h>
        #include <zephyr/kernel.h>
    #else
        #include <sys/stat.h>
        #include <sys/types.h>
        #include <unistd.h>
    #endif
    #define IS_SIMULATOR 0
#endif

/*********************
 *      DEFINES
 *********************/
#if IS_SIMULATOR
    /* For simulator, use a path in the current directory */
    #define UPDATE_JSON_PATH "current_update_step.json"
#else
    /* For real targets (Zephyr, Yocto, etc.), use appropriate path */
    #if defined(CONFIG_ZEPHYR_LVGL) || defined(CONFIG_ZEPHYR_KERNEL)
        #define UPDATE_JSON_PATH "/tmp/current_update_step.json"
    #else
        #define UPDATE_JSON_PATH "/var/lib/update_tracker/current_update_step.json"
    #endif
#endif

#define JSON_BUFFER_SIZE 512
#define DEFAULT_UPDATE_INTERVAL 2000  // Default polling interval in milliseconds

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    int progress;
    char status[64];
    char step[64];
} update_status_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static bool parse_json_value(const char* json, const char* key, char* value, size_t max_len);
static void check_update_status(lv_ui *ui);
static void ensure_update_json_exists(void);

#if IS_SIMULATOR
static void simulate_update_cycle(lv_timer_t *timer);
#endif

/* File operations compatibility layer */
static int64_t get_file_timestamp(const char* filepath);
static bool read_file_contents(const char* filepath, char* buffer, size_t max_size);
static bool write_file_contents(const char* filepath, const char* content);

/**********************
 *  STATIC VARIABLES
 **********************/
static int64_t last_modified_time = 0;  // Stores the last modified time of the JSON file
static lv_timer_t *update_timer = NULL; // Handle to the update timer
static update_status_t current_status = {0, "System Ready", "Waiting for update"}; // Current status

/**
 * Get file timestamp (last modification time)
 */
static int64_t get_file_timestamp(const char* filepath)
{
#if defined(CONFIG_ZEPHYR_LVGL) || defined(CONFIG_ZEPHYR_KERNEL)
    struct fs_dirent entry;
    if (fs_stat(filepath, &entry) != 0) {
        return 0;
    }
    return entry.mtime;
#else
    struct stat file_stat;
    if (stat(filepath, &file_stat) != 0) {
        return 0;
    }
    return file_stat.st_mtime;
#endif
}

/**
 * Read file contents into buffer
 */
static bool read_file_contents(const char* filepath, char* buffer, size_t max_size)
{
#if defined(CONFIG_ZEPHYR_LVGL) || defined(CONFIG_ZEPHYR_KERNEL)
    struct fs_file_t file;
    int ret;
    
    ret = fs_open(&file, filepath, FS_O_READ);
    if (ret != 0) {
        return false;
    }
    
    memset(buffer, 0, max_size);
    ret = fs_read(&file, buffer, max_size - 1);
    fs_close(&file);
    
    if (ret < 0) {
        return false;
    }
    
    return true;
#else
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        return false;
    }
    
    memset(buffer, 0, max_size);
    size_t bytes_read = fread(buffer, 1, max_size - 1, f);
    fclose(f);
    
    return bytes_read > 0;
#endif
}

/**
 * Write content to file
 */
static bool write_file_contents(const char* filepath, const char* content)
{
#if defined(CONFIG_ZEPHYR_LVGL) || defined(CONFIG_ZEPHYR_KERNEL)
    struct fs_file_t file;
    int ret;
    
    ret = fs_open(&file, filepath, FS_O_CREATE | FS_O_WRITE);
    if (ret != 0) {
        return false;
    }
    
    fs_write(&file, content, strlen(content));
    fs_close(&file);
    
    return true;
#else
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        // Try to create directory if file open fails
        #if defined(_WIN32) || defined(_WIN64)
            mkdir("tmp", 0777);
        #else
            mkdir("tmp", 0777);
        #endif
        
        // Try again
        f = fopen(filepath, "wb");
        if (!f) {
            return false;
        }
    }
    
    size_t bytes_written = fwrite(content, 1, strlen(content), f);
    fclose(f);
    
    return bytes_written == strlen(content);
#endif
}

/**
 * Parse a JSON string and extract a specific value
 */
static bool parse_json_value(const char* json, const char* key, char* value, size_t max_len)
{
    char search_key[64];
    char *key_pos, *value_start, *value_end;
    
    // Format the key to look for in JSON
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    
    // Find the key in the JSON string
    key_pos = strstr(json, search_key);
    if (!key_pos) {
        return false;
    }
    
    // Find the beginning of the value (after the colon and any whitespace)
    value_start = strchr(key_pos + strlen(search_key), ':');
    if (!value_start) {
        return false;
    }
    
    // Skip whitespace after colon
    value_start++;
    while (*value_start == ' ' || *value_start == '\t' || *value_start == '\n' || *value_start == '\r') {
        value_start++;
    }
    
    // If the value is a string (starts with quote)
    if (*value_start == '"') {
        value_start++; // Skip the opening quote
        value_end = strchr(value_start, '"');
        if (!value_end) {
            return false;
        }
        
        // Copy the string value (limited by max_len)
        size_t len = value_end - value_start;
        if (len >= max_len) {
            len = max_len - 1;
        }
        memcpy(value, value_start, len);
        value[len] = '\0';
    } else {
        // If the value is a number or boolean
        value_end = value_start;
        while (*value_end && *value_end != ',' && *value_end != '}' && *value_end != ' ' && 
               *value_end != '\t' && *value_end != '\n' && *value_end != '\r') {
            value_end++;
        }
        
        size_t len = value_end - value_start;
        if (len >= max_len) {
            len = max_len - 1;
        }
        memcpy(value, value_start, len);
        value[len] = '\0';
    }
    
    return true;
}

/**
 * Check for updates in the JSON file and update the UI elements
 */
static void check_update_status(lv_ui *ui)
{
    char buffer[JSON_BUFFER_SIZE];
    update_status_t status = current_status;  // Start with current values
    char value_str[32];
    bool file_changed = false;
    int64_t file_time;
    static int64_t last_log_time = 0;
    
    // First, check if the file has been modified since last read
    file_time = get_file_timestamp(UPDATE_JSON_PATH);
    if (file_time == 0) {
        // File doesn't exist yet - don't show an error, as this might be normal during startup
        // Only log a message if the interval between logs is substantial (to avoid flooding logs)
#if defined(CONFIG_ZEPHYR_LVGL) || defined(CONFIG_ZEPHYR_KERNEL)
    int64_t current_time = k_uptime_get();
#else
    int64_t current_time = (int64_t)time(NULL) * 1000;  // Convert to milliseconds
#endif
        if (current_time - last_log_time > 10000) {  // Log at most every 10 seconds
            printf("Waiting for update status file: %s\n", UPDATE_JSON_PATH);
            last_log_time = current_time;
        }
        return;
    }
    
    // Check if the file was modified since we last read it
    if (file_time != last_modified_time) {
        file_changed = true;
        last_modified_time = file_time;
    } else {
        // File hasn't changed since last read, no need to process it again
        return;
    }
    
    // Read the JSON file
    if (!read_file_contents(UPDATE_JSON_PATH, buffer, sizeof(buffer))) {
        printf("Error: Could not read file %s\n", UPDATE_JSON_PATH);
        return;
    }
    
    // Parse the JSON content
    if (parse_json_value(buffer, "progress", value_str, sizeof(value_str))) {
        status.progress = atoi(value_str);
    }
    
    if (parse_json_value(buffer, "status", status.status, sizeof(status.status))) {
        // Status successfully parsed
    }
    
    if (parse_json_value(buffer, "step", status.step, sizeof(status.step))) {
        // Step successfully parsed
    }
    
    // Save the current status
    current_status = status;
    
    // Update the UI elements
    if (ui->screen_status != NULL) {
        lv_label_set_text(ui->screen_status, status.status);
    }
    
    if (ui->screen_step != NULL) {
        lv_label_set_text(ui->screen_step, status.step);
    }
    
    if (ui->screen_progress != NULL) {
        char percentage[8];
        snprintf(percentage, sizeof(percentage), "%d%%", status.progress);
        lv_label_set_text(ui->screen_progress, percentage);
    }
    
    if (ui->screen_loading_bar != NULL) {
        lv_bar_set_value(ui->screen_loading_bar, status.progress, LV_ANIM_ON);
    }
    
    // Log update for debugging
    printf("Update status: %d%% - %s - %s\n", 
           status.progress, status.status, status.step);
}

/**
 * Poll for updates from the JSON file at a regular interval
 * Only reads and processes the JSON if the file has changed
 */
void update_tracker_task(lv_timer_t *timer)
{
    lv_ui *ui = (lv_ui *)timer->user_data;
    check_update_status(ui);
}

/**
 * Set the update polling interval
 * @param interval_ms New polling interval in milliseconds
 */
void set_update_polling_interval(uint32_t interval_ms)
{
    if (update_timer != NULL) {
        lv_timer_set_period(update_timer, interval_ms);
        printf("Update polling interval set to %d ms\n", interval_ms);
    }
}

/**
 * Ensures that the update status JSON file exists
 * Creates it with default values if it doesn't exist
 */
static void ensure_update_json_exists(void)
{
    // Check if file already exists
    if (get_file_timestamp(UPDATE_JSON_PATH) != 0) {
        // File exists, nothing to do
        return;
    }
    
    // File doesn't exist, create it with default values
    const char *default_json = "{\n"
                              "    \"progress\": 0,\n"
                              "    \"status\": \"System Ready\",\n"
                              "    \"step\": \"Waiting for update\"\n"
                              "}\n";
    
    if (!write_file_contents(UPDATE_JSON_PATH, default_json)) {
        printf("Error: Could not create %s\n", UPDATE_JSON_PATH);
        return;
    }
    
    printf("Created default update status file at %s\n", UPDATE_JSON_PATH);
}

#if IS_SIMULATOR
/**
 * For simulator only: Simulate update process automatically
 */
static void simulate_update_cycle(lv_timer_t *timer)
{
    lv_ui *ui = (lv_ui *)timer->user_data;
    static int update_phase = 0;
    static const char *update_steps[] = {
        "Preparing for update",
        "Downloading packages",
        "Verifying download",
        "Installing updates",
        "Configuring system", 
        "Finalizing installation",
        "Cleaning up",
        "Update complete."
    };
    static const int num_phases = sizeof(update_steps)/sizeof(update_steps[0]);
    
    // Calculate progress based on phase (0-100)
    int progress = (update_phase * 100) / num_phases;
    if (progress > 100) progress = 100;
    
    // Update the JSON file with new values
    char json_content[512];
    snprintf(json_content, sizeof(json_content), 
             "{\n"
             "    \"progress\": %d,\n"
             "    \"status\": \"System Updating...\",\n"
             "    \"step\": \"%s\"\n"
             "}\n", 
             progress, 
             update_steps[update_phase]);
    
    write_file_contents(UPDATE_JSON_PATH, json_content);
    
    // Move to next phase or restart
    update_phase++;
    if (update_phase >= num_phases) {
        // Cycle completed - reset or stop
        update_phase = 0;
    }
}
#endif

/**
 * Create a demo application
 */
void custom_init(lv_ui *ui)
{
    /* Initialize update tracking */
    update_timer = lv_timer_create(update_tracker_task, DEFAULT_UPDATE_INTERVAL, ui);
    
    /* Log the path where we're looking for the JSON file */
    printf("Update tracker: Monitoring %s for update status changes\n", UPDATE_JSON_PATH);
    
    /* Create the JSON file if it doesn't exist yet */
    ensure_update_json_exists();
    
    /* Force the initial check to always run by setting last_modified_time to 0 */
    last_modified_time = 0;
    check_update_status(ui);
    
#if IS_SIMULATOR
    /* For simulator only: start automatic update simulation */
    lv_timer_create(simulate_update_cycle, 3000, ui);  // Update every 3 seconds
    printf("SIMULATOR MODE: Auto-generating update status changes every 3 seconds\n");
#endif
}
