// Test the real Linux bridge against a private mock Portal session bus.
#include "nativefiledialog.h"
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
    auto r = nfd_new(nullptr, argc > 1 ? argv[1] : "save", "/tmp/猫 100%.png", "Title ' \" 猫");
    nfd_filter(r, "Images");
    nfd_pattern(r, "*.png");
    nfd_pattern(r, "*.jpg");
    nfd_prepare(r, true);
    std::thread cancel;
    if (argc > 2) cancel = std::thread([r] {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        nfd_cancel(r);
    });
    nfd_run(r);
    if (cancel.joinable()) cancel.join();
    std::cout << nfd_status(r) << '\n' << r->path << '\n' << r->backend << '\n' << r->error << '\n';
    nfd_release_parent(r);
    nfd_release(r);
}
