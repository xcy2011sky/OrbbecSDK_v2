// Copyright (c) Orbbec Inc. All Rights Reserved.
// Licensed under the MIT License.

#include "utils_c.h"
#include "utils_types.h"
#include <stdbool.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>

#if defined(__linux__) || defined(__APPLE__)
#include <strings.h>

static int ob_smpl_stricmp(const char *lhs, const char *rhs) {
    while(*lhs != '\0' && *rhs != '\0') {
        int diff = tolower((unsigned char)*lhs) - tolower((unsigned char)*rhs);
        if(diff != 0) {
            return diff;
        }
        lhs++;
        rhs++;
    }
    return tolower((unsigned char)*lhs) - tolower((unsigned char)*rhs);
}

#undef _stricmp
#undef _strnicmp
#define _stricmp ob_smpl_stricmp
#define _strnicmp strncasecmp
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ob_smpl_test_automation_t {
    int      initialized;
    int      enabled;
    uint64_t start_time_ms;
    uint32_t initial_delay_ms;
    uint32_t interval_ms;
    uint32_t key_count;
    uint32_t next_key_index;
    char     keys[64];
} ob_smpl_test_automation_t;

static ob_smpl_test_automation_t g_test_automation = { 0, 0, 0, 1500, 1000, 0, 0, { 0 } };

static int ob_smpl_parse_bool_env(const char *name) {
    const char *value = getenv(name);
    if(value == NULL) {
        return 0;
    }
    if(_stricmp(value, "1") == 0 || _stricmp(value, "true") == 0 || _stricmp(value, "yes") == 0 || _stricmp(value, "on") == 0) {
        return 1;
    }
    return 0;
}

static uint32_t ob_smpl_parse_u32_env(const char *name, uint32_t default_value) {
    const char *value = getenv(name);
    char       *end   = NULL;
    unsigned long parsed = 0;

    if(value == NULL || value[0] == '\0') {
        return default_value;
    }

    parsed = strtoul(value, &end, 10);
    if(end == value) {
        return default_value;
    }
    return (uint32_t)parsed;
}

static char ob_smpl_parse_key_token(const char *token) {
    size_t len = 0;
    if(token == NULL) {
        return 0;
    }

    while(*token != '\0' && isspace((unsigned char)*token)) {
        token++;
    }

    len = strlen(token);
    while(len > 0 && isspace((unsigned char)token[len - 1])) {
        len--;
    }

    if(len == 0) {
        return 0;
    }
    if(len == 1) {
        return token[0];
    }
    if(len == 3 && _strnicmp(token, "ESC", 3) == 0) {
        return 27;
    }
    if(len == 5 && _strnicmp(token, "ENTER", 5) == 0) {
        return '\n';
    }
    if(len == 5 && _strnicmp(token, "SPACE", 5) == 0) {
        return ' ';
    }
    if(len == 3 && _strnicmp(token, "TAB", 3) == 0) {
        return '\t';
    }
    return token[0];
}

static void ob_smpl_init_test_automation(void) {
    const char *rawKeys = NULL;
    char        buffer[256];
    char       *cursor = NULL;

    if(g_test_automation.initialized) {
        return;
    }

    g_test_automation.initialized      = 1;
    g_test_automation.enabled          = ob_smpl_parse_bool_env("OB_EXAMPLE_TEST_MODE");
    g_test_automation.initial_delay_ms = ob_smpl_parse_u32_env("OB_EXAMPLE_AUTO_KEY_INITIAL_DELAY_MS", 1500);
    g_test_automation.interval_ms      = ob_smpl_parse_u32_env("OB_EXAMPLE_AUTO_KEY_INTERVAL_MS", 1000);
    g_test_automation.start_time_ms    = ob_smpl_get_current_timestamp_ms();

    if(!g_test_automation.enabled) {
        return;
    }

    rawKeys = getenv("OB_EXAMPLE_AUTO_KEYS");
    if(rawKeys == NULL || rawKeys[0] == '\0') {
        g_test_automation.keys[0] = 'q';
        g_test_automation.key_count = 1;
        return;
    }

    strncpy(buffer, rawKeys, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    cursor = strtok(buffer, ", ");
    while(cursor != NULL && g_test_automation.key_count < (sizeof(g_test_automation.keys) / sizeof(g_test_automation.keys[0]))) {
        char key = ob_smpl_parse_key_token(cursor);
        if(key != 0) {
            g_test_automation.keys[g_test_automation.key_count++] = key;
        }
        cursor = strtok(NULL, ", ");
    }

    if(g_test_automation.key_count == 0) {
        g_test_automation.keys[0] = 'q';
        g_test_automation.key_count = 1;
    }
}

int ob_smpl_test_mode_enabled(void) {
    ob_smpl_init_test_automation();
    return g_test_automation.enabled;
}

char ob_smpl_test_poll_auto_key(void) {
    uint64_t now = 0;
    uint64_t due = 0;

    ob_smpl_init_test_automation();
    if(!g_test_automation.enabled || g_test_automation.next_key_index >= g_test_automation.key_count) {
        return 0;
    }

    now = ob_smpl_get_current_timestamp_ms();
    due = (uint64_t)g_test_automation.initial_delay_ms +
          (uint64_t)g_test_automation.next_key_index * (uint64_t)g_test_automation.interval_ms;
    if(now - g_test_automation.start_time_ms < due) {
        return 0;
    }

    return g_test_automation.keys[g_test_automation.next_key_index++];
}

#if defined(__linux__) || defined(__APPLE__)
#ifdef __linux__
#include <termio.h>
#else
#include <termios.h>
#endif
#include <unistd.h>
#include <fcntl.h>

#define gets_s gets

int getch(void) {
    struct termios tm, tm_old;
    int            fd = 0, ch;

    if(tcgetattr(fd, &tm) < 0) {  // Save the current terminal settings
        return -1;
    }

    tm_old = tm;
    cfmakeraw(&tm);                        // Change the terminal settings to raw mode, in which all input data is processed in bytes
    if(tcsetattr(fd, TCSANOW, &tm) < 0) {  // Settings after changes on settings
        return -1;
    }

    ch = getchar();
    if(tcsetattr(fd, TCSANOW, &tm_old) < 0) {  // Change the settings to what they were originally
        return -1;
    }

    return ch;
}

int kbhit(void) {
    struct termios oldt, newt;
    int            ch;
    int            oldf;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);
    ch = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);
    if(ch != EOF) {
        ungetc(ch, stdin);
        return 1;
    }
    return 0;
}

#include <sys/time.h>
uint64_t ob_smpl_get_current_timestamp_ms(void) {
    struct timeval te;
    long long      milliseconds;
    gettimeofday(&te, NULL);                                // Get the current time
    milliseconds = te.tv_sec * 1000LL + te.tv_usec / 1000;  // Calculate milliseconds
    return milliseconds;
}

char ob_smpl_wait_for_key_press(uint32_t timeout_ms) {  // Get the current time
    struct timeval te;
    long long      start_time;
    gettimeofday(&te, NULL);
    start_time = te.tv_sec * 1000LL + te.tv_usec / 1000;

    while(true) {
        long long current_time;
        if(kbhit()) {
            return getch();
        }
        {
            char auto_key = ob_smpl_test_poll_auto_key();
            if(auto_key != 0) {
                return auto_key;
            }
        }
        gettimeofday(&te, NULL);
        current_time = te.tv_sec * 1000LL + te.tv_usec / 1000;
        if(timeout_ms > 0 && current_time - start_time > timeout_ms) {
            return 0;
        }
        usleep(100);
    }
}

int ob_smpl_support_ansi_escape(void) {
    if(isatty(fileno(stdout)) == 0) {
        // unsupport
        return 0;
    }
    return 1;
}

#else  // Windows
#include <conio.h>
#include <windows.h>
#include <io.h>
#include <stdio.h>

uint64_t ob_smpl_get_current_timestamp_ms() {
    FILETIME      ft;
    LARGE_INTEGER li;
    GetSystemTimeAsFileTime(&ft);
    li.LowPart             = ft.dwLowDateTime;
    li.HighPart            = ft.dwHighDateTime;
    long long milliseconds = li.QuadPart / 10000LL;
    return milliseconds;
}

char ob_smpl_wait_for_key_press(uint32_t timeout_ms) {
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  originalMode = 0;
    int    modeChanged  = 0;
    int    hasConsoleInput = 0;

    if(hStdin != INVALID_HANDLE_VALUE && GetConsoleMode(hStdin, &originalMode)) {
        hasConsoleInput = 1;
        DWORD rawMode = originalMode & ~ENABLE_ECHO_INPUT;
        if(SetConsoleMode(hStdin, rawMode)) {
            modeChanged = 1;
        }
    }

    DWORD mode = 0;
    if(hStdin != INVALID_HANDLE_VALUE) {
        GetConsoleMode(hStdin, &mode);
    }
    DWORD start_time = GetTickCount();
    while(true) {
        {
            char auto_key = ob_smpl_test_poll_auto_key();
            if(auto_key != 0) {
                if(modeChanged) {
                    SetConsoleMode(hStdin, originalMode);
                }
                return auto_key;
            }
        }
        if(hasConsoleInput && _kbhit()) {
            char ch = (char)_getch();
            if(modeChanged) {
                SetConsoleMode(hStdin, originalMode);
            }
            return ch;
        }
        if(timeout_ms > 0 && GetTickCount() - start_time > timeout_ms) {
            if(modeChanged) {
                SetConsoleMode(hStdin, originalMode);
            }
            return 0;
        }
        Sleep(1);
    }
}

int ob_smpl_support_ansi_escape(void) {
    if(_isatty(_fileno(stdout)) == 0) {
        // unsupport
        return 0;
    }

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if(hOut == INVALID_HANDLE_VALUE) {
        return 0;
    }

    DWORD mode = 0;
    if(!GetConsoleMode(hOut, &mode)) {
        return 0;
    }
    if((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
        return 0;
    }
    return 1;
}

#endif

bool ob_smpl_is_lidar_device(ob_device *device) {
    ob_error       *error       = NULL;
    ob_sensor_list *sensorList  = NULL;
    uint32_t        sensorCount = 0;

    if(device == NULL) {
        return false;
    }

    sensorList = ob_device_get_sensor_list(device, &error);
    CHECK_OB_ERROR_EXIT(&error);

    sensorCount = ob_sensor_list_get_count(sensorList, &error);
    CHECK_OB_ERROR_EXIT(&error);

    for(uint32_t index = 0; index < sensorCount; index++) {
        OBSensorType sensorType = ob_sensor_list_get_sensor_type(sensorList, index, &error);
        CHECK_OB_ERROR_EXIT(&error);

        if(sensorType == OB_SENSOR_LIDAR) {
            ob_delete_sensor_list(sensorList, &error);
            CHECK_OB_ERROR_EXIT(&error);
            return true;
        }
    }

    ob_delete_sensor_list(sensorList, &error);
    CHECK_OB_ERROR_EXIT(&error);

    return false;
}

bool ob_smpl_is_gemini305_device(int vid, int pid) {
    return (vid == OB_DEVICE_VID && (pid == 0x0840 || pid == 0x0841 || pid == 0x0842 || pid == 0x0843));
}

bool ob_smpl_is_astra_mini_device(int vid, int pid) {
    return (vid == OB_DEVICE_VID && (pid == 0x069d || pid == 0x065b || pid == 0x065e));
}

#ifdef __cplusplus
}
#endif
