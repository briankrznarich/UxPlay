// SPDX-License-Identifier: GPL-3.0-or-later
#include "controlfloor.h"
#include "identity.h"
#include "pairing.h"
#include <mutex>
#include <allheaders.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <iostream>
#include <cstdlib>
#include <stdexcept>
using J = nlohmann::json;
static std::mutex outputMutex;
static void line(const std::string &text) {
    std::lock_guard<std::mutex> lock(outputMutex);
    std::cout << text << std::endl;
}
int main(int argc, char **argv) {
    gst_init(nullptr, nullptr);
    try {
        if (argc == 3 && std::string(argv[1]) == "--ocr-file") {
            controlfloor::ScreenReader reader;
            auto ocr = reader.readFile(argv[2]);
            auto result = controlfloor::aboutIdentity(ocr).json();
            result["keyboardId"] = controlfloor::keyboardID(ocr);
            result["text"] = ocr.text;
            std::cout << result.dump(2) << std::endl;
            return 0;
        }
        for (int i = 1; i < argc; ++i)
            if (!cf_option(argc, argv, &i)) throw std::runtime_error("unknown harness option");
        if (cf_test_source()) throw std::runtime_error("Gate test must not bypass OCR");
        if (cf_lan_enabled()) cf_lan_worker_transport([](const char *message) { line(std::string("pairing ")+message); });
        if (cf_start() != 0) return 1;
        GError *error = nullptr;
        std::string launch = "appsrc name=source is-live=true format=time ! ";
        if (std::getenv("CF_TEST_GL")) launch += "glupload ! glcolorconvert ! video/x-raw(memory:GLMemory),format=NV12 ! ";
        launch += "tee name=t ! queue ! fakesink sync=false async=false t. ! queue leaky=downstream max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! appsink name=cf_capture sync=false async=false max-buffers=1 drop=true";
        GstElement *pipeline = gst_parse_launch(launch.c_str(), &error);
        if (error || !pipeline) throw std::runtime_error(error ? error->message : "pipeline failed");
        cf_attach(pipeline);
        auto *src = gst_bin_get_by_name(GST_BIN(pipeline), "source");
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        line("ready");
        std::string input;
        while (std::getline(std::cin, input)) {
            auto command = J::parse(input);
            std::string action = command.at("action");
            if (action == "quit") break;
            if (action == "pairing") cf_lan_worker_message(command.at("message").dump());
            else if (action == "connect") {
                cf_connection_open();
                std::string id = command.at("id");
                cf_client(id.c_str(), "Test phone", "iPhone12,8");
            } else if (action == "disconnect") cf_disconnected();
            else if (action == "frame") {
                std::string path = command.at("path");
                Pix *image = pixRead(path.c_str());
                if (!image) throw std::runtime_error("cannot read fixture");
                Pix *rgb = pixConvertTo32(image); pixDestroy(&image);
                GstVideoInfo info;
                gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGB, pixGetWidth(rgb), pixGetHeight(rgb));
                GstCaps *caps = gst_video_info_to_caps(&info);
                gst_app_src_set_caps(GST_APP_SRC(src), caps); gst_caps_unref(caps);
                GstBuffer *buffer = gst_buffer_new_allocate(nullptr, info.size, nullptr);
                GstMapInfo map;
                gst_buffer_map(buffer, &map, GST_MAP_WRITE);
                for (int y = 0; y < info.height; ++y) for (int x = 0; x < info.width; ++x) {
                    l_int32 r, g, b; pixGetRGBPixel(rgb, x, y, &r, &g, &b);
                    auto *p = map.data + y * info.stride[0] + x * 3;
                    p[0] = r; p[1] = g; p[2] = b;
                }
                pixDestroy(&rgb); gst_buffer_unmap(buffer, &map);
                gst_app_src_push_buffer(GST_APP_SRC(src), buffer);
            } else throw std::runtime_error("unknown command");
            line("ok");
        }
        cf_disconnected();
        gst_element_set_state(pipeline, GST_STATE_NULL);
        cf_stop(); gst_object_unref(src); gst_object_unref(pipeline);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl; cf_stop(); return 1;
    }
}
