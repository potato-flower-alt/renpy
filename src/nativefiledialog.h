// Copyright 2026 Tom Rothamel. See LICENSE.txt.
#ifndef RENPY_NATIVEFILEDIALOG_H
#define RENPY_NATIVEFILEDIALOG_H

#include <SDL2/SDL.h>
#include <atomic>
#include <string>
#include <vector>

struct NFDFilter {
    std::string name;
    std::vector<std::string> patterns;
};

struct NFDRequest {
    std::atomic<int> references{1};
    std::atomic<bool> cancelled{false};
    bool close_sent = false; // Owned by the Windows STA thread only.
    // 0 pending, 1 selected, 2 cancelled, 3 unavailable, 4 error.
    std::atomic<int> status{0};
    std::atomic<int> phase{0};
    std::atomic<void *> cancel_window{nullptr};
    std::atomic<int> cancel_thread{0};
    std::string kind, initial, title, path, backend, error, parent;
    std::vector<NFDFilter> filters;
    void *window = nullptr;
    void *panel = nullptr;
    void *wayland = nullptr;
    bool parent_attached = false;
};

NFDRequest *nfd_new(SDL_Window *, const char *, const char *, const char *);
void nfd_filter(NFDRequest *, const char *);
void nfd_pattern(NFDRequest *, const char *);
bool nfd_prepare(NFDRequest *, bool);
void nfd_pump(NFDRequest *);
void nfd_run(NFDRequest *);
void nfd_start(NFDRequest *);
void nfd_cancel(NFDRequest *);
void nfd_release_parent(NFDRequest *);
void nfd_retain(NFDRequest *);
void nfd_release(NFDRequest *);
void nfd_finish(NFDRequest *, int, const std::string &, const std::string &);
int nfd_status(NFDRequest *);
int nfd_phase(NFDRequest *);
int nfd_cancel_thread(NFDRequest *);
const char *nfd_path(NFDRequest *);
const char *nfd_backend(NFDRequest *);
const char *nfd_error(NFDRequest *);
const char *nfd_parent(NFDRequest *);
bool nfd_parent_attached(NFDRequest *);

#endif
