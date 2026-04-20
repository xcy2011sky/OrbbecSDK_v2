// Copyright (c) Orbbec Inc. All Rights Reserved.
// Licensed under the MIT License.

// ob_firmware_update — Unified firmware update tool.
// Merges single-device and multi-device firmware update capabilities.
//
// Usage:
//   ob_firmware_update -l                        List connected devices
//   ob_firmware_update -f firmware.bin            Update single device (auto if only one)
//   ob_firmware_update -f firmware.bin -s SN123   Update device with specified serial number
//   ob_firmware_update -f firmware.bin -a         Update ALL connected devices sequentially

#include <libobsensor/ObSensor.hpp>

#include <iostream>
#include <iomanip>
#include <fstream>
#include <string>
#include <memory>
#include <vector>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <atomic>

#if defined(_WIN32)
#include <windows.h>
#endif

// ============================================================
// Command-line arguments
// ============================================================
struct CmdArgs {
    bool        showHelp  = false;
    bool        listOnly  = false;
    bool        updateAll = false;
    std::string firmwarePath;
    std::string serial;
};

// ============================================================
// Per-update state (reset before each device update)
// ============================================================
struct UpdateState {
    std::atomic<bool> firstCall{ true };
    std::atomic<bool> needReupdate{ false };
    std::atomic<bool> success{ false };
    std::atomic<bool> mismatch{ false };
    std::atomic<bool> failure{ false };
};

static bool g_ansiEscapeSupported = false;

// ============================================================
// Forward declarations
// ============================================================
static void        printUsage(const char *prog, bool brief);
static bool        parseArguments(int argc, char *argv[], CmdArgs &args);
static void        printDeviceList(const std::vector<std::shared_ptr<ob::Device>> &devices);
static void        firmwareUpdateCallback(UpdateState &state, OBFwUpdateState cbState, const char *message, uint8_t percent);
static bool        updateSingleDevice(std::shared_ptr<ob::Device> device, const std::string &firmwarePath, UpdateState &state);
static bool        waitForDeviceReboot(std::shared_ptr<ob::Context> context, const std::string &serial, std::shared_ptr<ob::Device> &outDevice);

// ============================================================
// Detect ANSI escape support (simplified)
// ============================================================
static bool supportAnsiEscape() {
#if defined(_WIN32)
    // Try to enable ANSI on Windows 10+
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if(hOut == INVALID_HANDLE_VALUE)
        return false;
    DWORD mode = 0;
    if(!GetConsoleMode(hOut, &mode))
        return false;
    mode |= 0x0004;  // ENABLE_VIRTUAL_TERMINAL_PROCESSING
    return SetConsoleMode(hOut, mode) != 0;
#else
    const char *term = std::getenv("TERM");
    return term != nullptr && std::string(term) != "dumb";
#endif
}

// ============================================================
// main
// ============================================================
int main(int argc, char *argv[]) try {
    CmdArgs args{};
    if(!parseArguments(argc, argv, args)) {
        return args.showHelp ? 0 : 1;
    }

    g_ansiEscapeSupported = supportAnsiEscape();

    // Create context
    auto context = std::make_shared<ob::Context>();

#if defined(__linux__)
    context->setUvcBackendType(OB_UVC_BACKEND_TYPE_LIBUVC);
#endif

    // Enumerate devices
    auto                                     deviceList = context->queryDeviceList();
    std::vector<std::shared_ptr<ob::Device>> devices;
    for(uint32_t i = 0; i < deviceList->getCount(); ++i) {
        try {
            devices.push_back(deviceList->getDevice(i));
        }
        catch(ob::Error &e) {
            std::cerr << "Warning: failed to open device " << i << ": " << e.what() << std::endl;
        }
    }

    if(devices.empty()) {
        std::cout << "No devices found." << std::endl;
        return 1;
    }

    // List-only mode
    if(args.listOnly) {
        printDeviceList(devices);
        return 0;
    }

    // ────────────────────────────────────────────
    // Mode A: Update ALL devices
    // ────────────────────────────────────────────
    if(args.updateAll) {
        std::cout << "\n=== Batch Firmware Update (" << devices.size() << " device(s)) ===" << std::endl;
        printDeviceList(devices);

        std::vector<std::shared_ptr<ob::Device>> successList, mismatchList, failedList;

        for(uint32_t i = 0; i < devices.size(); ++i) {
            auto info = devices[i]->getDeviceInfo();
            std::cout << "\n[" << (i + 1) << "/" << devices.size() << "] Updating " << info->getName() << " (SN: " << info->getSerialNumber() << ") ..."
                      << std::endl;

            UpdateState state;
            try {
                devices[i]->updateFirmware(
                    args.firmwarePath.c_str(), [&state](OBFwUpdateState s, const char *msg, uint8_t pct) { firmwareUpdateCallback(state, s, msg, pct); },
                    false);
            }
            catch(ob::Error &e) {
                std::cerr << "  Error: " << e.what() << std::endl;
                state.failure = true;
            }

            if(state.success)
                successList.push_back(devices[i]);
            else if(state.mismatch)
                mismatchList.push_back(devices[i]);
            else
                failedList.push_back(devices[i]);
        }

        // Summary
        std::cout << "\n========== Upgrade Summary ==========" << std::endl;
        std::cout << "Success   (" << successList.size() << "):" << std::endl;
        for(auto &d: successList)
            std::cout << "  - " << d->getDeviceInfo()->getName() << "  SN: " << d->getDeviceInfo()->getSerialNumber() << std::endl;

        std::cout << "Mismatch  (" << mismatchList.size() << "):" << std::endl;
        for(auto &d: mismatchList)
            std::cout << "  - " << d->getDeviceInfo()->getName() << "  SN: " << d->getDeviceInfo()->getSerialNumber() << std::endl;
        if(!mismatchList.empty())
            std::cout << "  (check firmware file matches the device model)" << std::endl;

        std::cout << "Failed    (" << failedList.size() << "):" << std::endl;
        for(auto &d: failedList)
            std::cout << "  - " << d->getDeviceInfo()->getName() << "  SN: " << d->getDeviceInfo()->getSerialNumber() << std::endl;

        // Reboot successful devices
        if(!successList.empty()) {
            std::cout << "\nRebooting " << successList.size() << " successfully updated device(s)..." << std::endl;
            for(auto &d: successList) {
                try {
                    d->reboot();
                }
                catch(...) {
                }
            }
        }

        return failedList.empty() ? 0 : 1;
    }

    // ────────────────────────────────────────────
    // Mode B: Update SINGLE device
    // ────────────────────────────────────────────
    std::shared_ptr<ob::Device> targetDevice;
    if(args.serial.empty()) {
        if(devices.size() > 1) {
            std::cerr << "\nError: multiple devices found. Use -s <serial> to select one, or -a to update all." << std::endl;
            printDeviceList(devices);
            return 1;
        }
        targetDevice = devices[0];
    }
    else {
        for(auto &d: devices) {
            if(std::string(d->getDeviceInfo()->getSerialNumber()) == args.serial) {
                targetDevice = d;
                break;
            }
        }
        if(!targetDevice) {
            std::cerr << "Error: device with serial \"" << args.serial << "\" not found." << std::endl;
            printDeviceList(devices);
            return 1;
        }
    }

    auto devInfo = targetDevice->getDeviceInfo();
    std::cout << "\nTarget device:" << std::endl;
    std::cout << "  " << devInfo->getName() << " | SN: " << devInfo->getSerialNumber() << " | FW: " << devInfo->getFirmwareVersion() << " | "
              << devInfo->getConnectionType() << std::endl;
    std::cout << "Firmware file:" << std::endl;
    std::cout << "  " << args.firmwarePath << std::endl;

    // Register device-changed callback for reboot-and-reupdate flow
    std::shared_ptr<ob::Device> deviceAfterReboot;
    std::mutex                  mtx;
    std::condition_variable     cv;
    std::string                 targetSN = devInfo->getSerialNumber();

    context->setDeviceChangedCallback([&](std::shared_ptr<ob::DeviceList> /*removedList*/, std::shared_ptr<ob::DeviceList> addedList) {
        try {
            auto dev = addedList->getDeviceBySN(targetSN.c_str());
            {
                std::lock_guard<std::mutex> lk(mtx);
                deviceAfterReboot = dev;
            }
            cv.notify_one();
        }
        catch(...) {
        }
    });

    // First update
    UpdateState state;
    updateSingleDevice(targetDevice, args.firmwarePath, state);

    // Handle reboot-and-reupdate
    if(state.needReupdate) {
        std::cout << "Device requires a second update pass after reboot..." << std::endl;
        targetDevice->reboot();
        targetDevice = nullptr;

        std::shared_ptr<ob::Device> rebooted;
        if(waitForDeviceReboot(context, targetSN, rebooted)) {
            targetDevice = rebooted;
        }
        else {
            // Manual fallback
            {
                std::lock_guard<std::mutex> lk(mtx);
                if(deviceAfterReboot)
                    targetDevice = deviceAfterReboot;
            }
            if(!targetDevice) {
                auto newList = context->queryDeviceList();
                try {
                    targetDevice = newList->getDeviceBySN(targetSN.c_str());
                }
                catch(...) {
                }
            }
        }

        if(!targetDevice) {
            std::cerr << "Device did not come back online. Re-run this tool once it reconnects." << std::endl;
            return 1;
        }

        std::cout << "Device is back online. Starting second update pass..." << std::endl;
        UpdateState state2;
        updateSingleDevice(targetDevice, args.firmwarePath, state2);
    }

    std::cout << "Firmware update completed successfully. Rebooting device..." << std::endl;
    targetDevice->reboot();

    // Cleanup
    deviceAfterReboot = nullptr;
    targetDevice      = nullptr;
    deviceList        = nullptr;
    context           = nullptr;
    return 0;
}
catch(ob::Error &e) {
    std::cerr << "function:" << e.getFunction() << "\nargs:" << e.getArgs() << "\nmessage:" << e.what() << "\ntype:" << e.getExceptionType() << std::endl;
    return EXIT_FAILURE;
}

// ============================================================
// Implementation
// ============================================================

static void printUsage(const char *prog, bool brief) {
    std::cout << "Usage:\n"
              << "  " << prog << " -l                        List connected devices\n"
              << "  " << prog << " -f <path>                 Update single device\n"
              << "  " << prog << " -f <path> -s <serial>     Update specific device by SN\n"
              << "  " << prog << " -f <path> -a              Update ALL connected devices\n"
              << std::endl;

    if(brief)
        return;

    std::cout << "Options:\n"
              << "  -h, --help              Show this help\n"
              << "  -l, --list              List connected devices and exit\n"
              << "  -f, --file <path>       Firmware image file (.bin or .img)\n"
              << "  -s, --serial <string>   Serial number of target device\n"
              << "  -a, --all               Update all connected devices sequentially\n"
              << std::endl;

    std::cout << "Examples:\n"
              << "  " << prog << " -l\n"
              << "  " << prog << " -f firmware.bin\n"
              << "  " << prog << " -f firmware.bin -s CP1234567890\n"
              << "  " << prog << " -f firmware.bin -a\n"
              << std::endl;
}

static bool parseArguments(int argc, char *argv[], CmdArgs &args) {
    if(argc < 2) {
        printUsage(argv[0], false);
        args.showHelp = true;
        return false;
    }

    for(int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if(arg == "-h" || arg == "--help") {
            printUsage(argv[0], false);
            args.showHelp = true;
            return false;
        }
        else if(arg == "-l" || arg == "--list") {
            args.listOnly = true;
            return true;
        }
        else if(arg == "-a" || arg == "--all") {
            args.updateAll = true;
        }
        else if(arg == "-f" || arg == "--file") {
            if(i + 1 >= argc) {
                std::cerr << "Error: -f requires a firmware file path." << std::endl;
                return false;
            }
            args.firmwarePath = argv[++i];
        }
        else if(arg == "-s" || arg == "--serial") {
            if(i + 1 >= argc) {
                std::cerr << "Error: -s requires a serial number." << std::endl;
                return false;
            }
            args.serial = argv[++i];
        }
        else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            printUsage(argv[0], true);
            return false;
        }
    }

    // Validate firmware path
    if(args.firmwarePath.empty()) {
        std::cerr << "Error: firmware file not specified. Use -f <path>." << std::endl;
        printUsage(argv[0], true);
        return false;
    }

    // Check file extension
    if(args.firmwarePath.size() > 4) {
        std::string ext = args.firmwarePath.substr(args.firmwarePath.size() - 4);
        for(auto &c: ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if(ext != ".bin" && ext != ".img") {
            std::cerr << "Error: firmware file must be .bin or .img." << std::endl;
            return false;
        }
    }
    else {
        std::cerr << "Error: invalid firmware file path." << std::endl;
        return false;
    }

    // Check file exists
    std::ifstream ifs(args.firmwarePath, std::ios::binary);
    if(!ifs.good()) {
        std::cerr << "Error: cannot open firmware file: " << args.firmwarePath << std::endl;
        return false;
    }

    // -a and -s are mutually exclusive
    if(args.updateAll && !args.serial.empty()) {
        std::cerr << "Error: -a and -s cannot be used together." << std::endl;
        return false;
    }

    return true;
}

static void printDeviceList(const std::vector<std::shared_ptr<ob::Device>> &devices) {
    std::cout << "\nConnected devices (" << devices.size() << "):" << std::endl;
    std::cout << std::string(80, '-') << std::endl;
    for(uint32_t i = 0; i < devices.size(); ++i) {
        auto info = devices[i]->getDeviceInfo();
        std::cout << "  [" << i << "] " << info->getName() << "  SN: " << info->getSerialNumber() << "  FW: " << info->getFirmwareVersion() << "  "
                  << info->getConnectionType() << std::endl;
    }
    std::cout << std::string(80, '-') << std::endl;
}

static void firmwareUpdateCallback(UpdateState &state, OBFwUpdateState cbState, const char *message, uint8_t percent) {
    if(state.firstCall) {
        state.firstCall = false;
    }
    else if(g_ansiEscapeSupported) {
        std::cout << "\033[3F";
    }

    if(g_ansiEscapeSupported)
        std::cout << "\033[K";
    std::cout << "  Progress: " << static_cast<uint32_t>(percent) << "%" << std::endl;

    if(g_ansiEscapeSupported)
        std::cout << "\033[K";
    std::cout << "  Status  : ";
    switch(cbState) {
    case STAT_VERIFY_IMAGE:
        std::cout << "Verifying image file";
        break;
    case STAT_VERIFY_SUCCESS:
        std::cout << "Image verification success";
        break;
    case STAT_FILE_TRANSFER:
        std::cout << "File transfer in progress";
        break;
    case STAT_START:
        std::cout << "Starting upgrade";
        break;
    case STAT_IN_PROGRESS:
        std::cout << "Upgrade in progress";
        break;
    case STAT_DONE:
        std::cout << "Update completed";
        state.success = true;
        break;
    case STAT_DONE_REBOOT_AND_REUPDATE:
        std::cout << "Reboot-and-reupdate required";
        state.needReupdate = true;
        state.success      = true;
        break;
    case ERR_MISMATCH:
        std::cout << "Mismatch between device and firmware";
        state.mismatch = true;
        break;
    default:
        if(cbState < 0) {
            std::cout << "Error (code " << cbState << ")";
            state.failure = true;
        }
        else {
            std::cout << "Unknown status (" << cbState << ")";
        }
        break;
    }
    std::cout << std::endl;

    if(g_ansiEscapeSupported)
        std::cout << "\033[K";
    std::cout << "  Message : " << (message ? message : "") << std::endl << std::flush;
}

static bool updateSingleDevice(std::shared_ptr<ob::Device> device, const std::string &firmwarePath, UpdateState &state) {
    std::cout << "\nUpgrading firmware, please wait...\n" << std::endl;
    try {
        device->updateFirmware(
            firmwarePath.c_str(), [&state](OBFwUpdateState s, const char *msg, uint8_t pct) { firmwareUpdateCallback(state, s, msg, pct); }, false);
    }
    catch(ob::Error &e) {
        std::cerr << "\n  Upgrade error: " << e.what() << std::endl;
        state.failure = true;
        return false;
    }
    std::cout << std::endl;
    return state.success;
}

static bool waitForDeviceReboot(std::shared_ptr<ob::Context> context, const std::string &serial, std::shared_ptr<ob::Device> &outDevice) {
    std::cout << "Waiting for device to reboot..." << std::endl;

    // Poll for up to 30 seconds
    for(int attempt = 0; attempt < 6; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        try {
            auto list = context->queryDeviceList();
            auto dev  = list->getDeviceBySN(serial.c_str());
            if(dev) {
                outDevice = dev;
                return true;
            }
        }
        catch(...) {
        }
        std::cout << "  Still waiting... (" << (attempt + 1) * 5 << "s)" << std::endl;
    }
    return false;
}
