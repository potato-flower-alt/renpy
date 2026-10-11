# Copyright 2026 Tom Rothamel. See LICENSE.txt.
# distutils: language = c++

from renpy.pygame.display cimport Window
from sdl2 cimport SDL_Window

cdef extern from "nativefiledialog.h":
    cdef cppclass NFDRequest:
        pass
    int nfd_status(NFDRequest *) noexcept
    int nfd_phase(NFDRequest *) noexcept
    int nfd_cancel_thread(NFDRequest *) noexcept
    const char *nfd_path(NFDRequest *) noexcept
    const char *nfd_backend(NFDRequest *) noexcept
    const char *nfd_error(NFDRequest *) noexcept
    const char *nfd_parent(NFDRequest *) noexcept
    bint nfd_parent_attached(NFDRequest *) noexcept
    NFDRequest *nfd_new(SDL_Window *, const char *, const char *, const char *) except +
    void nfd_filter(NFDRequest *, const char *) except +
    void nfd_pattern(NFDRequest *, const char *) except +
    bint nfd_prepare(NFDRequest *, bint) except +
    void nfd_pump(NFDRequest *) noexcept nogil
    void nfd_run(NFDRequest *) noexcept nogil
    void nfd_start(NFDRequest *) except +
    void nfd_cancel(NFDRequest *) noexcept
    void nfd_release_parent(NFDRequest *) noexcept
    void nfd_release(NFDRequest *) noexcept


cdef class NativeRequest:
    cdef NFDRequest *request

    def __cinit__(self, kind, initial, title, filters, window):
        cdef SDL_Window *native_window = NULL
        if window is not None:
            if not isinstance(window, Window):
                raise TypeError("Expected an engine Window.")
            native_window = (<Window> window).window
        self.request = nfd_new(native_window, kind.encode("utf-8"), (initial or "").encode("utf-8", "surrogateescape"), title.encode("utf-8"))
        for name, patterns in filters:
            nfd_filter(self.request, name.encode("utf-8"))
            for pattern in patterns:
                nfd_pattern(self.request, pattern.encode("utf-8"))

    def __dealloc__(self):
        if self.request != NULL:
            nfd_release_parent(self.request)
            nfd_release(self.request)

    def prepare(self, abandon=False):
        return nfd_prepare(self.request, abandon)

    def pump(self):
        with nogil:
            nfd_pump(self.request)

    def run(self):
        with nogil:
            nfd_run(self.request)

    def start(self):
        nfd_start(self.request)

    def cancel(self):
        nfd_cancel(self.request)

    def pending_stage(self):
        return nfd_phase(self.request)

    def cancel_thread(self):
        return nfd_cancel_thread(self.request)

    def release_parent(self):
        nfd_release_parent(self.request)

    def x11_parent(self):
        cdef bytes parent = nfd_parent(self.request)
        if parent.startswith(b"x11:"):
            return str(int(parent[4:], 16))
        return None

    def result(self):
        cdef int status = nfd_status(self.request)
        if status == 0:
            return ("pending", None, None, None, False)
        cdef bytes path = nfd_path(self.request)
        cdef bytes backend = nfd_backend(self.request)
        cdef bytes error = nfd_error(self.request)
        return (
            ("pending", "selected", "cancelled", "unavailable", "error")[status],
            path.decode("utf-8", "surrogateescape") if path else None,
            backend.decode("utf-8") if backend else None,
            error.decode("utf-8", "replace") if error else None,
            bool(nfd_parent_attached(self.request)),
        )
