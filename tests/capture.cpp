// SPDX-FileCopyrightText: 2026 LingmoOS Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// The real window class lives with the application in main.cpp. Include it
// without its entry point so the capture program drives exactly that window.
#define ARCHIVE_MANAGER_NO_MAIN
#include "../src/main.cpp"
#undef ARCHIVE_MANAGER_NO_MAIN

#include "base/event_loop.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <poll.h>
#include <string>

using namespace Lingmo;
using namespace Lingmo::ui;

namespace {
void spin(int milliseconds)
{
    base::EventLoop &loop = Application::instance()->loop();
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < end) {
        pollfd descriptor{loop.fd(), POLLIN, 0};
        ::poll(&descriptor, 1, 10);
        loop.dispatch();
    }
}
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s OUTDIR [ARCHIVE]\n", argv[0]);
        return 1;
    }
    const std::string directory = argv[1];
    const std::string archivePath =
            argc > 2 ? argv[2] : ARCHIVE_SOURCE_DIR "/tests/data/sample.zip";
    std::filesystem::create_directories(directory);

    Application app(argc, argv, Application::Platform::Headless);
    app.setApplicationId("lingmo-archive-manager");

    for (const bool dark : {false, true}) {
        Theme::setCurrent(dark ? Theme::dark() : Theme::light());
        ArchiveWindow window;
        window.window().setScale(1.5);
        window.window().resize({1000, 720});
        window.openArchive(archivePath);
        window.window().show();
        for (int i = 0; i < 10; ++i) {
            window.window().grab();
            spin(20);
        }
        window.window().grab().savePng(
                directory + "/archive-manager-" + (dark ? "dark" : "light") + ".png");
    }
    return 0;
}
