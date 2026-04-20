// Copyright (c) Orbbec Inc. All Rights Reserved.
// Licensed under the MIT License.

// Console-mode implementation of CVWindow.
// Used when OpenCV is not available — prints frame info to stdout.

#include "utils_opencv.hpp"
#include "utils.hpp"
#include "utils_types.h"

#include <iostream>
#include <sstream>
#include <iomanip>

namespace ob_smpl {

static std::string replaceExtension(const std::string &path, const std::string &newExt) {
    auto dot = path.rfind('.');
    auto slash = path.find_last_of("\\/");
    if(dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return path.substr(0, dot) + newExt;
    }
    return path + newExt;
}

std::string saveFrame(std::shared_ptr<const ob::Frame> frame, const std::string &path) {
    if(!frame) return "";
    std::string name = replaceExtension(resolveSaveOutputPath(path), ".png");
    if(ob::FrameSaveHelper::saveFrameToPng(name.c_str(), frame)) {
        return name;
    }
    return "";
}

static std::string frameTypeToString(OBFrameType type) {
    switch(type) {
    case OB_FRAME_COLOR: return "Color";
    case OB_FRAME_DEPTH: return "Depth";
    case OB_FRAME_IR: return "IR";
    case OB_FRAME_IR_LEFT: return "IR_L";
    case OB_FRAME_IR_RIGHT: return "IR_R";
    case OB_FRAME_ACCEL: return "Accel";
    case OB_FRAME_GYRO: return "Gyro";
    case OB_FRAME_CONFIDENCE: return "Conf";
    default: return "Unknown";
    }
}

static std::string formatToString(OBFormat format) {
    switch(format) {
    case OB_FORMAT_YUYV: return "YUYV";
    case OB_FORMAT_YUY2: return "YUY2";
    case OB_FORMAT_UYVY: return "UYVY";
    case OB_FORMAT_NV12: return "NV12";
    case OB_FORMAT_NV21: return "NV21";
    case OB_FORMAT_MJPG: return "MJPG";
    case OB_FORMAT_Y16: return "Y16";
    case OB_FORMAT_Y8: return "Y8";
    case OB_FORMAT_Y10: return "Y10";
    case OB_FORMAT_Y11: return "Y11";
    case OB_FORMAT_Y12: return "Y12";
    case OB_FORMAT_Y14: return "Y14";
    case OB_FORMAT_RGB: return "RGB";
    case OB_FORMAT_BGR: return "BGR";
    case OB_FORMAT_RGBA: return "RGBA";
    case OB_FORMAT_BGRA: return "BGRA";
    case OB_FORMAT_I420: return "I420";
    case OB_FORMAT_Z16: return "Z16";
    default: return "fmt(" + std::to_string(static_cast<int>(format)) + ")";
    }
}

CVWindow::CVWindow(std::string name, uint32_t width, uint32_t height, ArrangeMode arrangeMode)
    : name_(std::move(name)),
      arrangeMode_(arrangeMode),
      width_(width),
      height_(height),
      closed_(false),
      showInfo_(true),
      showSyncTimeInfo_(false),
      alpha_(0.6f),
      logCreatedTime_(0),
      autoSaveEnabled_(true),
      saveIndex_(0),
      lastPrintTime_(0),
      frameCount_(0),
      promptPrinted_(false) {
    std::cout << "[" << name_ << "] Console mode (no OpenCV). Press Esc/Q to exit, S to save." << std::endl;
}

CVWindow::~CVWindow() noexcept {
    close();
}

bool CVWindow::run() {
    if(closed_) {
        return false;
    }

    // Print key prompt once
    if(!promptPrinted_ && !prompt_.empty()) {
        std::cout << "[" << name_ << "] Keys: " << prompt_ << std::endl;
        promptPrinted_ = true;
    }

    // Non-blocking key check
    char key = waitForKeyPressed(1);
    if(key != 0) {
        if(key == ESC_KEY || key == 'q' || key == 'Q') {
            closed_ = true;
            return false;
        }
        if((key == 's' || key == 'S') && autoSaveEnabled_) {
            // Copy frames under lock, save outside to avoid blocking producers
            decltype(lastFrameGroups_) framesCopy;
            {
                std::lock_guard<std::mutex> lock(framesMtx_);
                framesCopy = lastFrameGroups_;
            }
            int savedCount = 0;
            for(auto &groupItem: framesCopy) {
                for(auto &frame: groupItem.second) {
                    if(!frame)
                        continue;
                    std::string baseName = frameTypeToSavePrefix(frame->getType()) + "_" + std::to_string(saveIndex_);
                    auto        saved    = saveFrame(frame, baseName);
                    if(!saved.empty()) {
                        addLog("Saved: " + saved);
                        savedCount++;
                    }
                }
            }
            if(savedCount > 0) {
                saveIndex_++;
            }
            else {
                addLog("No frames to save");
            }
        }
        if(keyPressedCallback_) {
            keyPressedCallback_(static_cast<int>(key));
        }
    }

    // Throttle printing to ~1 Hz
    uint64_t now = getNowTimesMs();
    if(now - lastPrintTime_ < 1000) {
        return true;
    }
    lastPrintTime_ = now;

    // Build summary line from latest frames
    std::lock_guard<std::mutex> lock(framesMtx_);
    if(lastFrameGroups_.empty()) {
        return true;
    }

    std::ostringstream oss;
    oss << "[" << name_ << "] #" << frameCount_;

    for(auto &kv: lastFrameGroups_) {
        for(auto &frame: kv.second) {
            if(!frame) {
                continue;
            }
            printFrameInfo(frame);
        }
    }

    return true;
}

void CVWindow::printFrameInfo(const std::shared_ptr<const ob::Frame> &frame) {
    auto type = frame->getType();
    std::ostringstream oss;
    oss << "  " << frameTypeToString(type);

    if(frame->is<ob::VideoFrame>()) {
        auto vf = frame->as<const ob::VideoFrame>();
        oss << " " << vf->getWidth() << "x" << vf->getHeight();
        oss << " " << formatToString(vf->getFormat());

        // Print center distance for depth frames
        if(type == OB_FRAME_DEPTH) {
            auto df = frame->as<const ob::DepthFrame>();
            if(df->getFormat() == OB_FORMAT_Y16 || df->getFormat() == OB_FORMAT_Z16) {
                uint32_t        w     = df->getWidth();
                uint32_t        h     = df->getHeight();
                const uint16_t *data  = reinterpret_cast<const uint16_t *>(df->getData());
                float           scale = df->getValueScale();
                float           dist  = data[h / 2 * w + w / 2] * scale;
                oss << " center=" << std::fixed << std::setprecision(0) << dist << "mm";
            }
        }
    }
    else if(type == OB_FRAME_ACCEL) {
        auto af    = frame->as<const ob::AccelFrame>();
        auto value = af->getValue();
        oss << " ax=" << std::fixed << std::setprecision(2) << value.x << " ay=" << value.y << " az=" << value.z;
    }
    else if(type == OB_FRAME_GYRO) {
        auto gf    = frame->as<const ob::GyroFrame>();
        auto value = gf->getValue();
        oss << " gx=" << std::fixed << std::setprecision(2) << value.x << " gy=" << value.y << " gz=" << value.z;
    }

    oss << " ts=" << frame->getTimeStampUs() << "us";
    std::cout << oss.str() << std::endl;
}

void CVWindow::close() {
    closed_ = true;
}

void CVWindow::reset() {
    std::lock_guard<std::mutex> lock(framesMtx_);
    lastFrameGroups_.clear();
    frameCount_    = 0;
    lastPrintTime_ = 0;
    closed_        = false;
}

void CVWindow::pushFramesToView(std::vector<std::shared_ptr<const ob::Frame>> frames, int groupId) {
    if(frames.empty()) {
        return;
    }

    std::vector<std::shared_ptr<const ob::Frame>> singleFrames;
    for(auto &frame: frames) {
        if(frame == nullptr) {
            continue;
        }

        if(!frame->is<ob::FrameSet>()) {
            singleFrames.push_back(frame);
            continue;
        }

        // FrameSet contains multiple frames
        auto frameSet = frame->as<ob::FrameSet>();
        for(uint32_t index = 0; index < frameSet->getCount(); index++) {
            auto subFrame = frameSet->getFrameByIndex(index);
            singleFrames.push_back(subFrame);
        }
    }

    std::lock_guard<std::mutex> lock(framesMtx_);
    lastFrameGroups_[groupId] = singleFrames;
    frameCount_++;
}

void CVWindow::pushFramesToView(std::shared_ptr<const ob::Frame> currentFrame, int groupId) {
    if(currentFrame == nullptr) {
        return;
    }
    pushFramesToView(std::vector<std::shared_ptr<const ob::Frame>>{ currentFrame }, groupId);
}

void CVWindow::setShowInfo(bool show) {
    showInfo_ = show;
}

void CVWindow::setShowSyncTimeInfo(bool show) {
    showSyncTimeInfo_ = show;
}

void CVWindow::setAlpha(float alpha) {
    alpha_ = alpha;
    if(alpha_ > 1.0f)
        alpha_ = 1.0f;
    if(alpha_ < 0.0f)
        alpha_ = 0.0f;
}

void CVWindow::resize(int width, int height) {
    width_  = width;
    height_ = height;
}

void CVWindow::setKeyPressedCallback(std::function<void(int)> callback) {
    keyPressedCallback_ = callback;
}

void CVWindow::setKeyPrompt(const std::string &prompt) {
    prompt_ = prompt;
}

void CVWindow::setAutoSaveEnabled(bool enabled) {
    autoSaveEnabled_ = enabled;
}

void CVWindow::addLog(const std::string &log) {
    log_            = log;
    logCreatedTime_ = getNowTimesMs();
    std::cout << "[" << name_ << "] " << log << std::endl;
}

void CVWindow::destroyWindow() {
    // No-op in console mode
}

}  // namespace ob_smpl
