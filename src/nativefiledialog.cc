// Copyright 2026 Tom Rothamel. See LICENSE.txt.
#include "nativefiledialog.h"
#include <SDL2/SDL_syswm.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <dbus/dbus.h>
#include <wayland-client.h>
#endif

#ifdef __APPLE__
void nfd_mac_start(NFDRequest *);
void nfd_mac_cancel(NFDRequest *);
void *nfd_mac_retain_parent(void *);
void nfd_mac_release_parent(NFDRequest *);
#endif

void nfd_finish(NFDRequest *r, int status, const std::string &path, const std::string &error) {
    r->path = path;
    r->error = error;
    r->status.store(status, std::memory_order_release);
}
void nfd_retain(NFDRequest *r) { ++r->references; }
void nfd_release(NFDRequest *r) { if (--r->references == 0) delete r; }
void nfd_filter(NFDRequest *r, const char *name) { r->filters.push_back({name, {}}); }
void nfd_pattern(NFDRequest *r, const char *pattern) { r->filters.back().patterns.push_back(pattern); }
int nfd_status(NFDRequest *r) { return r->status.load(std::memory_order_acquire); }
int nfd_phase(NFDRequest *r) { return r->phase.load(); }
int nfd_cancel_thread(NFDRequest *r) { return r->cancel_thread.load(); }
const char *nfd_path(NFDRequest *r) { return r->path.c_str(); }
const char *nfd_backend(NFDRequest *r) { return r->backend.c_str(); }
const char *nfd_error(NFDRequest *r) { return r->error.c_str(); }
const char *nfd_parent(NFDRequest *r) { return r->parent.c_str(); }
bool nfd_parent_attached(NFDRequest *r) { return r->parent_attached; }

#if defined(__linux__) && !defined(__ANDROID__)
// Private Wayland queue: SDL remains the only reader of its display socket.
// These descriptors cover only the export half of xdg-foreign v1/v2.
struct WaylandExport {
    void *library = nullptr;
    wl_display *display = nullptr;
    wl_surface *surface = nullptr;
    wl_event_queue *queue = nullptr;
    wl_proxy *registry = nullptr, *exporter = nullptr, *exported = nullptr;
    bool have_v2 = false;
    uint32_t exporter_name = 0;
    NFDRequest *request = nullptr;
    decltype(&wl_display_create_queue) create_queue;
    decltype(&wl_event_queue_destroy) destroy_queue;
    decltype(&wl_display_dispatch_queue_pending) dispatch_pending;
    decltype(&wl_display_flush) flush;
    decltype(&wl_proxy_marshal_constructor_versioned) construct;
    decltype(&wl_proxy_marshal) marshal;
    decltype(&wl_proxy_destroy) destroy;
    decltype(&wl_proxy_set_queue) set_queue;
    decltype(&wl_proxy_add_listener) add_listener;
    const wl_interface *registry_interface = nullptr, *surface_interface = nullptr;
    wl_interface exporter_interface{}, exported_interface{};
    wl_message exporter_methods[2]{}, exported_methods[1]{}, exported_events[1]{};
    const wl_interface *export_types[2]{};

    template<class T> bool symbol(T &target, const char *name) {
        target = reinterpret_cast<T>(dlsym(library, name));
        return target != nullptr;
    }
    bool load() {
        library = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library) return false;
#define WL_LOAD(field, name) if (!symbol(field, name)) return false
        WL_LOAD(create_queue, "wl_display_create_queue");
        WL_LOAD(destroy_queue, "wl_event_queue_destroy");
        WL_LOAD(dispatch_pending, "wl_display_dispatch_queue_pending");
        WL_LOAD(flush, "wl_display_flush");
        WL_LOAD(construct, "wl_proxy_marshal_constructor_versioned");
        WL_LOAD(marshal, "wl_proxy_marshal");
        WL_LOAD(destroy, "wl_proxy_destroy");
        WL_LOAD(set_queue, "wl_proxy_set_queue");
        WL_LOAD(add_listener, "wl_proxy_add_listener");
        WL_LOAD(registry_interface, "wl_registry_interface");
        WL_LOAD(surface_interface, "wl_surface_interface");
#undef WL_LOAD
        return true;
    }
    void cleanup() {
        if (exported) { marshal(exported, 0); destroy(exported); exported = nullptr; }
        if (exporter) { marshal(exporter, 0); destroy(exporter); exporter = nullptr; }
        if (registry) { destroy(registry); registry = nullptr; }
        if (display && queue) { flush(display); destroy_queue(queue); queue = nullptr; }
        if (library) { dlclose(library); library = nullptr; }
    }
    ~WaylandExport() { cleanup(); }
    static void handle(void *data, wl_proxy *, const char *handle) {
        auto w = static_cast<WaylandExport *>(data);
        w->request->parent = std::string("wayland:") + handle;
    }
    static void global(void *data, wl_registry *, uint32_t name, const char *interface, uint32_t) {
        auto w = static_cast<WaylandExport *>(data);
        if (!strcmp(interface, "zxdg_exporter_v2") || (!w->exporter_name && !strcmp(interface, "zxdg_exporter_v1"))) {
            w->exporter_name = name;
            w->have_v2 = !strcmp(interface, "zxdg_exporter_v2");
        }
    }
    static void removed(void *, wl_registry *, uint32_t) {}
    void initialise() {
        queue = create_queue(display);
        if (!queue) return;
        registry = construct(reinterpret_cast<wl_proxy *>(display), 1, registry_interface, 1, nullptr);
        if (!registry) return;
        set_queue(registry, queue);
        static const wl_registry_listener listener = {global, removed};
        add_listener(registry, reinterpret_cast<void (**)(void)>(const_cast<wl_registry_listener *>(&listener)), this);
        flush(display);
    }
    void advance() {
        if (!queue || !registry) return;
        dispatch_pending(display, queue);
        if (exporter_name && !exporter) {
            const char *exporter_name_string = have_v2 ? "zxdg_exporter_v2" : "zxdg_exporter_v1";
            const char *exported_name_string = have_v2 ? "zxdg_exported_v2" : "zxdg_exported_v1";
            exported_methods[0] = {"destroy", "", nullptr};
            exported_events[0] = {"handle", "s", nullptr};
            exported_interface = {exported_name_string, 1, 1, exported_methods, 1, exported_events};
            export_types[0] = &exported_interface;
            export_types[1] = surface_interface;
            exporter_methods[0] = {"destroy", "", nullptr};
            exporter_methods[1] = {have_v2 ? "export_toplevel" : "export", "no", export_types};
            exporter_interface = {exporter_name_string, 1, 2, exporter_methods, 0, nullptr};
            exporter = construct(registry, 0, &exporter_interface, 1, exporter_name, exporter_name_string, 1, nullptr);
            if (!exporter) return;
            set_queue(exporter, queue);
            exported = construct(exporter, 1, &exported_interface, 1, nullptr, surface);
            if (!exported) return;
            set_queue(exported, queue);
            static void (*listener[])(void) = {reinterpret_cast<void (*)(void)>(handle)};
            add_listener(exported, listener, this);
            flush(display);
        }
    }
};
#endif

NFDRequest *nfd_new(SDL_Window *window, const char *kind, const char *initial, const char *title) {
    auto r = std::make_unique<NFDRequest>();
    r->kind = kind; r->initial = initial; r->title = title;
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    // A sheet/owned modal panel on a hidden window cannot reliably close or
    // restore focus. Use an independent panel until the game window is shown.
    if (window && !(SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) && SDL_GetWindowWMInfo(window, &info)) {
#ifdef _WIN32
        if (info.subsystem == SDL_SYSWM_WINDOWS) r->window = info.info.win.window;
#elif defined(__APPLE__)
        if (info.subsystem == SDL_SYSWM_COCOA) r->window = nfd_mac_retain_parent(info.info.cocoa.window);
#elif defined(__linux__) && !defined(__ANDROID__)
#ifdef SDL_VIDEO_DRIVER_X11
        if (info.subsystem == SDL_SYSWM_X11) {
            char parent[64];
            snprintf(parent, sizeof(parent), "x11:%lx", info.info.x11.window);
            r->parent = parent;
        }
#endif
#ifdef SDL_VIDEO_DRIVER_WAYLAND
        if (info.subsystem == SDL_SYSWM_WAYLAND) {
            auto w = std::make_unique<WaylandExport>();
            w->display = info.info.wl.display;
            w->surface = info.info.wl.surface;
            w->request = r.get();
            if (w->load()) {
                w->initialise();
                r->wayland = w.release();
            }
        }
#endif
#endif
    }
    return r.release();
}

bool nfd_prepare(NFDRequest *r, bool abandon) {
#if defined(__linux__) && !defined(__ANDROID__)
    if (r->wayland) {
        auto w = static_cast<WaylandExport *>(r->wayland);
        if (!abandon && !r->cancelled) w->advance();
        if (!abandon && !r->cancelled && r->parent.empty()) return false;
        if (r->parent.empty()) nfd_release_parent(r);
    }
#endif
    return true;
}
void nfd_pump(NFDRequest *) {
#ifdef _WIN32
    // The COM dialog's modal loop sends messages to its SDL owner on the main
    // thread. Pump without the Python GIL so cross-thread window activation and
    // enable messages can finish even when SDL_PollEvent has cached a sentinel.
    // SDL's WindowProc still translates input into the normal SDL event queue.
    MSG message;
    ULONGLONG deadline = GetTickCount64() + 4;
    for (int count = 0; count < 64 && GetTickCount64() <= deadline; ++count) {
        if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) break;
        if (message.message == WM_QUIT) {
            SDL_Event quit{};
            quit.type = SDL_QUIT;
            SDL_PushEvent(&quit);
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
#endif
}
void nfd_release_parent(NFDRequest *r) {
#ifdef __APPLE__
    nfd_mac_release_parent(r);
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    delete static_cast<WaylandExport *>(r->wayland);
    r->wayland = nullptr;
#endif
}
void nfd_cancel(NFDRequest *r) {
    if (r->cancelled.exchange(true)) return;
#ifdef _WIN32
    HWND messages = static_cast<HWND>(r->cancel_window.load());
    if (messages) PostMessageW(messages, WM_NULL, 0, 0);
#endif
#ifdef __APPLE__
    nfd_mac_cancel(r);
#endif
}

#ifdef _WIN32
static std::wstring wide(const std::string &s) {
    if (s.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid UTF-8 in a file dialog argument.");
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), &result[0], size);
    return result;
}
static std::string utf8(const wchar_t *s) {
    int size = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Could not decode the selected path.");
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &result[0], size, nullptr, nullptr);
    result.resize(size - 1);
    return result;
}
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
template<class T> struct ComPointer {
    T *p = nullptr;
    ~ComPointer() { if (p) p->Release(); }
};
static void check(HRESULT hr) {
    if (FAILED(hr)) {
        char error[64];
        snprintf(error, sizeof(error), "Windows file dialog HRESULT 0x%08lx", static_cast<unsigned long>(hr));
        throw std::runtime_error(error);
    }
}
static const wchar_t *cancel_property = L"RenPyNativeFileDialogRequest";
static const UINT_PTR cancel_timer = 0x726e7079;
static void CALLBACK dialog_timer(HWND window, UINT, UINT_PTR, DWORD) {
    auto r = static_cast<NFDRequest *>(GetPropW(window, cancel_property));
    if (r && r->cancelled && r->panel && IsWindowVisible(window) && !r->close_sent) {
        r->cancel_thread = static_cast<int>(GetCurrentThreadId());
        r->close_sent = true;
        r->phase = 11;
        HRESULT closed = static_cast<IFileDialog *>(r->panel)->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        r->phase = SUCCEEDED(closed) ? 120 : static_cast<int>(closed);
        if (SUCCEEDED(closed)) {
            KillTimer(window, cancel_timer);
            // Some Shell implementations defer Close while initial folder
            // navigation is still running. Queue the native window's cancel
            // message as well; it is handled on this same STA thread.
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        else r->close_sent = false;
    }
}
class DialogEvents : public IFileDialogEvents {
    std::atomic<ULONG> references{1};
    NFDRequest *request;
public:
    DialogEvents(NFDRequest *r) : request(r) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **value) override {
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IFileDialogEvents)) {
            *value = static_cast<IFileDialogEvents *>(this); AddRef(); return S_OK;
        }
        *value = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { auto count = --references; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog *dialog) override {
        request->phase = 10;
        if (!request->cancel_window.load()) {
            ComPointer<IOleWindow> ole_window;
            HWND window = nullptr;
            if (SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&ole_window.p))) &&
                SUCCEEDED(ole_window.p->GetWindow(&window)) && window &&
                SetPropW(window, cancel_property, request)) {
                if (SetTimer(window, cancel_timer, 25, dialog_timer)) request->cancel_window = window;
                else RemovePropW(window, cancel_property);
            }
        }
        // OnFolderChange may run before Show's modal loop is fully installed.
        // Closing reentrantly here can leave that loop waiting forever. The
        // owning window timer closes after the dialog becomes visible.
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog *, IShellItem *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog *, IShellItem *, FDE_SHAREVIOLATION_RESPONSE *response) override { *response = FDESVR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog *, IShellItem *, FDE_OVERWRITE_RESPONSE *response) override { *response = FDEOR_DEFAULT; return S_OK; }
};
static void windows_run(NFDRequest *r) {
    r->backend = "windows-com";
    r->phase = 1;
    ComScope scope;
    check(scope.result);
    r->phase = 2;
    ComPointer<IFileDialog> dialog;
    if (r->kind == "save")
        check(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog.p)));
    else
        check(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog.p)));
    r->phase = 3;
    FILEOPENDIALOGOPTIONS flags = FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT;
    flags |= r->kind == "save" ? (FOS_OVERWRITEPROMPT | FOS_NOTESTFILECREATE) : FOS_FILEMUSTEXIST;
    if (r->kind == "folder") flags |= FOS_PICKFOLDERS;
    check(dialog.p->SetOptions(flags));
    check(dialog.p->SetTitle(wide(r->title).c_str()));
    r->phase = 4;
    std::vector<std::wstring> names, patterns;
    std::vector<COMDLG_FILTERSPEC> filters;
    if (r->kind != "folder") {
        for (auto &filter : r->filters) {
            names.push_back(wide(filter.name));
            std::string joined;
            for (auto &pattern : filter.patterns) { if (!joined.empty()) joined += ";"; joined += pattern; }
            patterns.push_back(wide(joined));
        }
        for (size_t i = 0; i < names.size(); ++i) filters.push_back({names[i].c_str(), patterns[i].c_str()});
        if (!filters.empty()) check(dialog.p->SetFileTypes(static_cast<UINT>(filters.size()), filters.data()));
    }
    std::string directory = r->initial;
    if (r->kind != "folder") {
        auto last = directory.find_last_of("/\\");
        if (last != std::string::npos) {
            std::string filename = directory.substr(last + 1);
            directory.resize(last + 1);
            if (!filename.empty()) check(dialog.p->SetFileName(wide(filename).c_str()));
        }
    }
    if (!directory.empty()) {
        r->phase = 5;
        ComPointer<IShellItem> folder;
        check(SHCreateItemFromParsingName(wide(directory).c_str(), nullptr, IID_PPV_ARGS(&folder.p)));
        r->phase = 6;
        check(dialog.p->SetFolder(folder.p));
    }
    r->phase = 7;
    ComPointer<IFileDialogEvents> events;
    events.p = new DialogEvents(r);
    DWORD cookie;
    check(dialog.p->Advise(events.p, &cookie));
    r->panel = dialog.p;
    HWND parent = static_cast<HWND>(r->window);
    r->parent_attached = parent && IsWindow(parent) && IsWindowVisible(parent);
    r->phase = 8;
    HRESULT hr = r->cancelled ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : dialog.p->Show(r->parent_attached ? parent : nullptr);
    r->phase = 9;
    r->panel = nullptr;
    HWND messages = static_cast<HWND>(r->cancel_window.exchange(nullptr));
    if (messages && IsWindow(messages)) { KillTimer(messages, cancel_timer); RemovePropW(messages, cancel_property); }
    dialog.p->Unadvise(cookie);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) { nfd_finish(r, 2, "", ""); return; }
    check(hr);
    ComPointer<IShellItem> selected;
    check(dialog.p->GetResult(&selected.p));
    PWSTR path = nullptr;
    check(selected.p->GetDisplayName(SIGDN_FILESYSPATH, &path));
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> memory(path, CoTaskMemFree);
    nfd_finish(r, 1, utf8(path), "");
}
#endif

#if defined(__linux__) && !defined(__ANDROID__)
// Load libdbus at runtime so a missing library can fall back to a graphical
// program instead of preventing the engine itself from starting.
struct DBus {
    void *library = nullptr;
#define DBUS_FUNCTIONS(X) \
    X(dbus_threads_init_default) X(dbus_error_init) X(dbus_error_free) X(dbus_error_is_set) \
    X(dbus_bus_get_private) X(dbus_bus_name_has_owner) X(dbus_bus_get_unique_name) X(dbus_bus_start_service_by_name) \
    X(dbus_bus_add_match) X(dbus_bus_remove_match) X(dbus_connection_set_exit_on_disconnect) \
    X(dbus_connection_close) X(dbus_connection_unref) X(dbus_connection_read_write) \
    X(dbus_connection_pop_message) X(dbus_connection_get_is_connected) \
    X(dbus_connection_send_with_reply_and_block) X(dbus_connection_send) X(dbus_connection_flush) \
    X(dbus_message_new_method_call) X(dbus_message_unref) X(dbus_message_is_signal) \
    X(dbus_message_get_path) X(dbus_message_get_sender) X(dbus_message_get_type) X(dbus_message_get_error_name) \
    X(dbus_message_iter_init) X(dbus_message_iter_init_append) X(dbus_message_iter_next) \
    X(dbus_message_iter_get_arg_type) X(dbus_message_iter_get_basic) X(dbus_message_iter_recurse) \
    X(dbus_message_iter_append_basic) X(dbus_message_iter_open_container) X(dbus_message_iter_close_container)
#define FIELD(name) decltype(&::name) name = nullptr;
    DBUS_FUNCTIONS(FIELD)
#undef FIELD
    DBusConnection *connection = nullptr;
    std::string match;
    std::string owner_match;
    bool load() {
        library = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (!library) return false;
#define LOAD(name) name = reinterpret_cast<decltype(name)>(dlsym(library, #name)); if (!name) return false;
        DBUS_FUNCTIONS(LOAD)
#undef LOAD
        return dbus_threads_init_default();
    }
    ~DBus() {
        if (connection) {
            if (!match.empty()) dbus_bus_remove_match(connection, match.c_str(), nullptr);
            if (!owner_match.empty()) dbus_bus_remove_match(connection, owner_match.c_str(), nullptr);
            dbus_connection_close(connection);
            dbus_connection_unref(connection);
        }
        if (library) dlclose(library);
    }
    void basic(DBusMessageIter &iter, int type, const void *value) {
        if (!dbus_message_iter_append_basic(&iter, type, value)) throw std::bad_alloc();
    }
    void open(DBusMessageIter &iter, int type, const char *sig, DBusMessageIter &child) {
        if (!dbus_message_iter_open_container(&iter, type, sig, &child)) throw std::bad_alloc();
    }
    void close(DBusMessageIter &iter, DBusMessageIter &child) {
        if (!dbus_message_iter_close_container(&iter, &child)) throw std::bad_alloc();
    }
    void option(DBusMessageIter &options, const char *key, const char *sig, int type, const void *value) {
        DBusMessageIter entry, variant;
        open(options, DBUS_TYPE_DICT_ENTRY, nullptr, entry);
        basic(entry, DBUS_TYPE_STRING, &key);
        open(entry, DBUS_TYPE_VARIANT, sig, variant);
        basic(variant, type, value);
        close(entry, variant); close(options, entry);
    }
    void bytes_option(DBusMessageIter &options, const char *key, const std::string &value) {
        DBusMessageIter entry, variant, bytes;
        open(options, DBUS_TYPE_DICT_ENTRY, nullptr, entry);
        basic(entry, DBUS_TYPE_STRING, &key);
        open(entry, DBUS_TYPE_VARIANT, "ay", variant);
        open(variant, DBUS_TYPE_ARRAY, "y", bytes);
        for (size_t i = 0; i <= value.size(); ++i) {
            unsigned char byte = i == value.size() ? 0 : static_cast<unsigned char>(value[i]);
            basic(bytes, DBUS_TYPE_BYTE, &byte);
        }
        close(variant, bytes); close(entry, variant); close(options, entry);
    }
};
struct BusError {
    DBus &bus;
    DBusError error;
    BusError(DBus &bus) : bus(bus) { bus.dbus_error_init(&error); }
    ~BusError() { bus.dbus_error_free(&error); }
    std::string message() { return error.message ? error.message : "D-Bus request failed."; }
};
struct BusMessage {
    DBus &bus;
    DBusMessage *p;
    BusMessage(DBus &bus, DBusMessage *p) : bus(bus), p(p) {}
    ~BusMessage() { if (p) bus.dbus_message_unref(p); }
};
static const char *portal_name = "org.freedesktop.portal.Desktop";
static const char *portal_path = "/org/freedesktop/portal/desktop";
static const char *chooser_interface = "org.freedesktop.portal.FileChooser";
static void close_portal(DBus &bus, const std::string &handle) {
    BusMessage close(bus, bus.dbus_message_new_method_call(portal_name, handle.c_str(), "org.freedesktop.portal.Request", "Close"));
    if (close.p) {
        bus.dbus_connection_send(bus.connection, close.p, nullptr);
        bus.dbus_connection_flush(bus.connection);
    }
}
struct PortalRequestScope {
    DBus &bus;
    std::string handle;
    bool completed = false;
    ~PortalRequestScope() { if (!completed && !handle.empty()) close_portal(bus, handle); }
};
static void portal_run(NFDRequest *r) {
    r->backend = "portal";
    DBus bus;
    if (!bus.load()) { nfd_finish(r, 3, "", "libdbus-1 is unavailable."); return; }
    BusError error(bus);
    bus.connection = bus.dbus_bus_get_private(DBUS_BUS_SESSION, &error.error);
    if (!bus.connection) { nfd_finish(r, 3, "", error.message()); return; }
    bus.dbus_connection_set_exit_on_disconnect(bus.connection, false);
    if (!bus.dbus_bus_name_has_owner(bus.connection, portal_name, &error.error)) {
        if (bus.dbus_error_is_set(&error.error)) { nfd_finish(r, 4, "", error.message()); return; }
        dbus_uint32_t activated;
        if (!bus.dbus_bus_start_service_by_name(bus.connection, portal_name, 0, &activated, &error.error)) {
            bool unavailable = error.error.name && (!strcmp(error.error.name, DBUS_ERROR_SERVICE_UNKNOWN) ||
                !strcmp(error.error.name, DBUS_ERROR_FILE_NOT_FOUND));
            nfd_finish(r, unavailable ? 3 : 4, "", error.message()); return;
        }
    }
    // Query the interface before showing anything. A missing implementation or
    // directory capability is unavailable; a failed in-flight request is error.
    BusMessage query(bus, bus.dbus_message_new_method_call(portal_name, portal_path, "org.freedesktop.DBus.Properties", "Get"));
    if (!query.p) throw std::bad_alloc();
    DBusMessageIter iter;
    bus.dbus_message_iter_init_append(query.p, &iter);
    const char *property = "version";
    bus.basic(iter, DBUS_TYPE_STRING, &chooser_interface);
    bus.basic(iter, DBUS_TYPE_STRING, &property);
    BusMessage version(bus, bus.dbus_connection_send_with_reply_and_block(bus.connection, query.p, 2000, &error.error));
    if (!version.p) {
        bool unavailable = error.error.name && (!strcmp(error.error.name, DBUS_ERROR_UNKNOWN_INTERFACE) ||
            !strcmp(error.error.name, DBUS_ERROR_UNKNOWN_METHOD) || !strcmp(error.error.name, "org.freedesktop.DBus.Error.UnknownProperty"));
        nfd_finish(r, unavailable ? 3 : 4, "", error.message()); return;
    }
    DBusMessageIter value;
    if (!bus.dbus_message_iter_init(version.p, &iter) || bus.dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
        throw std::runtime_error("Portal returned an invalid version property.");
    bus.dbus_message_iter_recurse(&iter, &value);
    if (bus.dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_UINT32)
        throw std::runtime_error("Portal returned an invalid interface version.");
    dbus_uint32_t number;
    bus.dbus_message_iter_get_basic(&value, &number);
    const char *owner = bus.dbus_message_get_sender(version.p);
    if (!owner) throw std::runtime_error("Portal version reply has no sender.");
    std::string portal_owner = owner;
    if (number < 1 || (r->kind == "folder" && number < 3)) { nfd_finish(r, 3, "", "Portal FileChooser lacks the requested capability."); return; }
    if (r->cancelled) { nfd_finish(r, 2, "", ""); return; }
    // Subscribe to the namespace on this private connection before submitting
    // the call. Queued responses survive even if a backend replies immediately
    // or returns a handle different from the handle_token prediction.
    const char *unique = bus.dbus_bus_get_unique_name(bus.connection);
    std::string sender = unique ? unique : "";
    if (sender.empty()) throw std::runtime_error("D-Bus did not assign a connection name.");
    sender.erase(0, 1);
    std::replace(sender.begin(), sender.end(), '.', '_');
    std::string request_prefix = "/org/freedesktop/portal/desktop/request/" + sender + "/";
    bus.match = "type='signal',sender='org.freedesktop.portal.Desktop',interface='org.freedesktop.portal.Request',member='Response',path_namespace='" + request_prefix.substr(0, request_prefix.size() - 1) + "'";
    bus.dbus_bus_add_match(bus.connection, bus.match.c_str(), &error.error);
    if (bus.dbus_error_is_set(&error.error)) throw std::runtime_error(error.message());
    bus.owner_match = "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.freedesktop.portal.Desktop'";
    bus.dbus_bus_add_match(bus.connection, bus.owner_match.c_str(), &error.error);
    if (bus.dbus_error_is_set(&error.error)) throw std::runtime_error(error.message());
    std::string token = "renpy_" + std::to_string(reinterpret_cast<uintptr_t>(r));
    BusMessage call(bus, bus.dbus_message_new_method_call(portal_name, portal_path, chooser_interface, r->kind == "save" ? "SaveFile" : "OpenFile"));
    if (!call.p) throw std::bad_alloc();
    bus.dbus_message_iter_init_append(call.p, &iter);
    const char *parent = r->parent.c_str(), *title = r->title.c_str();
    bus.basic(iter, DBUS_TYPE_STRING, &parent); bus.basic(iter, DBUS_TYPE_STRING, &title);
    r->parent_attached = !r->parent.empty();
    DBusMessageIter options;
    bus.open(iter, DBUS_TYPE_ARRAY, "{sv}", options);
    const char *token_string = token.c_str();
    bus.option(options, "handle_token", "s", DBUS_TYPE_STRING, &token_string);
    dbus_bool_t yes = true, no = false;
    bus.option(options, "modal", "b", DBUS_TYPE_BOOLEAN, &yes);
    if (r->kind != "save") bus.option(options, "multiple", "b", DBUS_TYPE_BOOLEAN, &no);
    if (r->kind == "folder") bus.option(options, "directory", "b", DBUS_TYPE_BOOLEAN, &yes);
    if (!r->initial.empty()) {
        auto last = r->initial.find_last_of('/');
        std::string folder = r->kind == "folder" ? r->initial : r->initial.substr(0, last + 1);
        bus.bytes_option(options, "current_folder", folder);
        if (r->kind == "save" && last != std::string::npos && last + 1 < r->initial.size()) {
            const char *name = r->initial.c_str() + last + 1;
            bus.option(options, "current_name", "s", DBUS_TYPE_STRING, &name);
        }
    }
    if (!r->filters.empty() && r->kind != "folder") {
        DBusMessageIter entry, variant, filters;
        bus.open(options, DBUS_TYPE_DICT_ENTRY, nullptr, entry);
        const char *key = "filters";
        bus.basic(entry, DBUS_TYPE_STRING, &key);
        bus.open(entry, DBUS_TYPE_VARIANT, "a(sa(us))", variant);
        bus.open(variant, DBUS_TYPE_ARRAY, "(sa(us))", filters);
        for (auto &filter : r->filters) {
            DBusMessageIter item, patterns;
            bus.open(filters, DBUS_TYPE_STRUCT, nullptr, item);
            const char *name = filter.name.c_str();
            bus.basic(item, DBUS_TYPE_STRING, &name);
            bus.open(item, DBUS_TYPE_ARRAY, "(us)", patterns);
            for (auto &pattern : filter.patterns) {
                DBusMessageIter pair;
                bus.open(patterns, DBUS_TYPE_STRUCT, nullptr, pair);
                dbus_uint32_t glob = 0;
                const char *text = pattern.c_str();
                bus.basic(pair, DBUS_TYPE_UINT32, &glob); bus.basic(pair, DBUS_TYPE_STRING, &text);
                bus.close(patterns, pair);
            }
            bus.close(item, patterns); bus.close(filters, item);
        }
        bus.close(variant, filters); bus.close(entry, variant); bus.close(options, entry);
    }
    bus.close(iter, options);
    PortalRequestScope request{bus, request_prefix + token};
    BusMessage reply(bus, bus.dbus_connection_send_with_reply_and_block(bus.connection, call.p, 5000, &error.error));
    std::string &handle = request.handle;
    if (!reply.p) {
        nfd_finish(r, r->cancelled ? 2 : 4, "", error.message()); return;
    }
    if (!bus.dbus_message_iter_init(reply.p, &iter) || bus.dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH)
        throw std::runtime_error("Portal returned an invalid Request handle.");
    const char *returned_handle;
    bus.dbus_message_iter_get_basic(&iter, &returned_handle);
    if (strncmp(returned_handle, request_prefix.c_str(), request_prefix.size())) {
        throw std::runtime_error("Portal returned a Request outside the connection namespace.");
    }
    handle = returned_handle;
    while (true) {
        if (r->cancelled) { close_portal(bus, handle); nfd_finish(r, 2, "", ""); return; }
        if (!bus.dbus_connection_read_write(bus.connection, 50) || !bus.dbus_connection_get_is_connected(bus.connection))
            throw std::runtime_error("The session bus disconnected during the file dialog.");
        while (auto message = bus.dbus_connection_pop_message(bus.connection)) {
            BusMessage response(bus, message);
            if (bus.dbus_message_is_signal(message, "org.freedesktop.DBus", "NameOwnerChanged")) {
                if (bus.dbus_message_iter_init(message, &iter) && bus.dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
                    const char *name;
                    bus.dbus_message_iter_get_basic(&iter, &name);
                    if (!strcmp(name, portal_name)) throw std::runtime_error("The desktop Portal service changed during the file dialog.");
                }
                continue;
            }
            if (!bus.dbus_message_is_signal(message, "org.freedesktop.portal.Request", "Response") ||
                !bus.dbus_message_get_path(message) || handle != bus.dbus_message_get_path(message) ||
                !bus.dbus_message_get_sender(message) || portal_owner != bus.dbus_message_get_sender(message)) continue;
            if (!bus.dbus_message_iter_init(message, &iter) || bus.dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32)
                throw std::runtime_error("Portal returned an invalid Response.");
            dbus_uint32_t code;
            bus.dbus_message_iter_get_basic(&iter, &code);
            request.completed = true;
            if (code == 1) { nfd_finish(r, 2, "", ""); return; }
            if (code != 0) { nfd_finish(r, 4, "", "Portal reported failure (Response " + std::to_string(code) + ")."); return; }
            if (!bus.dbus_message_iter_next(&iter) || bus.dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
                throw std::runtime_error("Portal returned an invalid results dictionary.");
            DBusMessageIter entries;
            bus.dbus_message_iter_recurse(&iter, &entries);
            std::string uri;
            while (bus.dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter entry, variant, uris;
                bus.dbus_message_iter_recurse(&entries, &entry);
                if (bus.dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING) throw std::runtime_error("Invalid portal result key.");
                const char *key;
                bus.dbus_message_iter_get_basic(&entry, &key);
                if (!strcmp(key, "uris")) {
                    if (!bus.dbus_message_iter_next(&entry) || bus.dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
                        throw std::runtime_error("Invalid portal URI variant.");
                    bus.dbus_message_iter_recurse(&entry, &variant);
                    if (bus.dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_ARRAY) throw std::runtime_error("Invalid portal URI array.");
                    bus.dbus_message_iter_recurse(&variant, &uris);
                    if (bus.dbus_message_iter_get_arg_type(&uris) != DBUS_TYPE_STRING) throw std::runtime_error("Portal returned no selected URI.");
                    const char *text;
                    bus.dbus_message_iter_get_basic(&uris, &text);
                    uri = text;
                    if (bus.dbus_message_iter_next(&uris)) throw std::runtime_error("Portal returned multiple URIs for a single selection.");
                }
                bus.dbus_message_iter_next(&entries);
            }
            if (uri.empty()) throw std::runtime_error("Portal returned no selected URI.");
            nfd_finish(r, 1, uri, ""); return;
        }
    }
}
#endif

void nfd_run(NFDRequest *r) {
    try {
        if (r->cancelled) { nfd_finish(r, 2, "", ""); return; }
#ifdef _WIN32
        windows_run(r);
#elif defined(__linux__) && !defined(__ANDROID__)
        portal_run(r);
#else
        nfd_finish(r, 3, "", "This platform does not support worker file dialogs.");
#endif
    } catch (const std::exception &e) {
        nfd_finish(r, r->cancelled ? 2 : 4, "", e.what());
    } catch (...) {
        nfd_finish(r, 4, "", "Unexpected native file dialog failure.");
    }
}
void nfd_start(NFDRequest *r) {
#ifdef __APPLE__
    nfd_mac_start(r);
#else
    nfd_finish(r, 3, "", "This platform does not support main-thread file dialogs.");
#endif
}
