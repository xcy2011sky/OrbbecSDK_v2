// Copyright (c) Orbbec Inc. All Rights Reserved.
// Licensed under the MIT License.

// RGBD Viewer — Acquire aligned RGB-D streams and display.
// Demonstrates: Pipeline config, software Depth-to-Color alignment (ob::Align),
//               OVERLAY visualization, save depth/color frames.

#include <libobsensor/ObSensor.hpp>

#include "utils.hpp"
#include "utils_opencv.hpp"

#include <iostream>

#ifdef OB_HAVE_OPENCV
#include <opencv2/imgcodecs.hpp>
#endif

int main(void) try {
    auto config = std::make_shared<ob::Config>();
    config->enableVideoStream(OB_STREAM_DEPTH, OB_WIDTH_ANY, OB_HEIGHT_ANY, OB_FPS_ANY, OB_FORMAT_ANY);
    config->enableVideoStream(OB_STREAM_COLOR, OB_WIDTH_ANY, OB_HEIGHT_ANY, OB_FPS_ANY, OB_FORMAT_ANY);
    config->setFrameAggregateOutputMode(OB_FRAME_AGGREGATE_OUTPUT_ALL_TYPE_FRAME_REQUIRE);

    auto pipe = std::make_shared<ob::Pipeline>();
    pipe->start(config);

    // Software Align: depth → color coordinate space
    auto alignFilter = std::make_shared<ob::Align>(OB_STREAM_COLOR);

    // Create a window for rendering with overlay mode.
    ob_smpl::CVWindow win("RGBD Viewer", 1280, 720, ob_smpl::ARRANGE_OVERLAY);
    std::shared_ptr<ob::FrameSet> latestAlignedFrameSet;

#ifdef OB_HAVE_OPENCV
    uint32_t overlaySaveIndex = 0;
    win.setKeyPressedCallback([&](int key) {
        if(key != 's' && key != 'S') {
            return;
        }
        if(!latestAlignedFrameSet) {
            win.addLog("No aligned frames available for RGBD overlay save");
            return;
        }

        auto colorFrame = latestAlignedFrameSet->getFrame(OB_FRAME_COLOR);
        auto depthFrame = latestAlignedFrameSet->getFrame(OB_FRAME_DEPTH);
        if(!colorFrame || !depthFrame) {
            win.addLog("Missing color/depth frame for RGBD overlay save");
            return;
        }

        auto colorMat = ob_smpl::convertColorFrameToBGR(colorFrame);
        auto depthMat = ob_smpl::renderDepth3D(depthFrame);
        if(colorMat.empty() || depthMat.empty()) {
            win.addLog("Failed to convert frame for RGBD overlay save");
            return;
        }

        if(colorMat.size() != depthMat.size()) {
            cv::resize(depthMat, depthMat, colorMat.size(), 0, 0, cv::INTER_NEAREST);
        }

        cv::Mat fused;
        // Depth alpha is 50% to visually validate alignment with color content.
        cv::addWeighted(colorMat, 0.5, depthMat, 0.5, 0.0, fused);

        std::string path = ob_smpl::resolveSaveOutputPath("rgbd_alignment_overlay_" + std::to_string(overlaySaveIndex++) + ".png");
        if(cv::imwrite(path, fused)) {
            win.addLog("Saved: " + path);
        }
        else {
            win.addLog("Failed to save RGBD overlay image");
        }
    });
#endif

    while(win.run()) {
        auto frameSet = pipe->waitForFrameset(100);
        if(!frameSet) {
            continue;
        }

        // Align depth to color
        auto alignedFrame = alignFilter->process(frameSet);
        if(!alignedFrame) {
            continue;
        }

        auto alignedFrameSet = alignedFrame->as<ob::FrameSet>();
        if(!alignedFrameSet) {
            continue;
        }

        latestAlignedFrameSet = alignedFrameSet;

        // Push aligned frameset to display (CVWindow handles visualization)
        win.pushFramesToView(alignedFrameSet);
    }

    pipe->stop();

    return 0;
}
catch(ob::Error &e) {
    std::cerr << "function:" << e.getFunction() << "\nargs:" << e.getArgs() << "\nmessage:" << e.what() << "\ntype:" << e.getExceptionType() << std::endl;
    std::cout << "\nPress any key to exit.";
    ob_smpl::waitForKeyPressed();
    exit(EXIT_FAILURE);
}
