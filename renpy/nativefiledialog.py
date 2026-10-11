# Copyright 2026 Tom Rothamel <pytom@bishoujo.us>
# See LICENSE.txt for the license of this file.

"""Transient, asynchronous desktop file dialogs. Nothing here belongs to a store."""

from dataclasses import dataclass
import logging
import os
import queue
import shutil
import subprocess
import threading
import time
import urllib.parse

import renpy


log = logging.getLogger(__name__)


@dataclass(frozen=True)
class FileDialogResult:
    done: bool
    status: str
    path: str | None = None
    backend: str | None = None
    error: str | None = None
    parent_attached: bool = False


def _expired_request():
    return FileDialogRequest(FileDialogResult(True, "cancelled", error="The request was not restored from a save."))


class FileDialogRequest:
    """A handle, not an owner of native resources or a rollback-aware object."""

    __slots__ = ("_result",)

    def __init__(self, result=None):
        self._result = result or FileDialogResult(False, "pending")

    def poll(self):
        """Return the current immutable result without waiting or dispatching callbacks."""
        return self._result

    def cancel(self):
        """Ask the owning backend to close. Safe to call more than once."""
        if threading.current_thread() is not threading.main_thread():
            raise RuntimeError("File dialogs must be cancelled on the main thread.")
        if _active is not None and _active.request is self:
            _active.cancel()

    def __reduce__(self):
        # Pickle neither the result nor the live resource. Loading must never
        # re-display a dialog, even if a game accidentally saves this handle.
        return (_expired_request, ())


def _normalise_path(kind, path):
    if path is None:
        return None
    path = os.fspath(path)
    if not isinstance(path, str) or "\0" in path:
        raise ValueError("initial_path must be a string without NUL characters.")
    directory_hint = path.endswith(tuple(i for i in (os.sep, os.altsep) if i))
    path = os.path.abspath(os.path.expanduser(path))
    filename = ""
    if kind != "folder" and not directory_hint and not os.path.isdir(path):
        path, filename = os.path.split(path)
    while not os.path.isdir(path):
        parent = os.path.dirname(path)
        if parent == path:
            return None
        path = parent
    return os.path.join(path, filename) if filename else path + (os.sep if kind != "folder" else "")


def _normalise_filters(kind, filters):
    if kind == "folder" or filters is None:
        return ()
    result = []
    for description, patterns in filters:
        if not isinstance(description, str) or "\0" in description:
            raise ValueError("Filter descriptions must be strings without NUL characters.")
        if isinstance(patterns, str):
            raise ValueError("Filter patterns must be a sequence of strings.")
        patterns = tuple(patterns)
        if not patterns or any(not isinstance(p, str) or not p or "\0" in p for p in patterns):
            raise ValueError("Filters require nonempty patterns without NUL characters.")
        if any(
            p not in ("*", "*.*") and (not p.startswith("*.") or not p[2:] or any(c in p[2:] for c in "*?[]/\\;|\n\r"))
            for p in patterns
        ):
            raise ValueError("Portable filters support *, *.* and *.extension patterns.")
        if any(c in description for c in "|\n\r"):
            raise ValueError("Filter descriptions cannot contain | or line breaks.")
        result.append((description, patterns))
    return tuple(result)


def _file_uri_path(uri):
    """Decode once, reject remote/non-file URLs and malformed escapes."""
    if not isinstance(uri, str) or any(ord(ch) <= 32 or ord(ch) == 127 or ch == "\\" for ch in uri):
        raise ValueError("The portal returned an invalid file URI.")
    parts = urllib.parse.urlsplit(uri)
    if parts.scheme != "file" or parts.netloc not in ("", "localhost") or parts.query or parts.fragment:
        raise ValueError("The portal returned a non-local file URI.")
    for i, ch in enumerate(parts.path):
        if ch == "%" and (
            i + 2 >= len(parts.path) or any(c not in "0123456789abcdefABCDEF" for c in parts.path[i + 1 : i + 3])
        ):
            raise ValueError("The portal returned an invalid percent escape.")
    path = os.fsdecode(urllib.parse.unquote_to_bytes(parts.path))
    if not path.startswith("/") or "\0" in path:
        raise ValueError("The portal returned an invalid file path.")
    return path


def _fallback_arguments(backend, kind, initial, filters, title, x11_parent=None):
    if backend == "kdialog":
        operation = {"save": "--getsavefilename", "open": "--getopenfilename", "folder": "--getexistingdirectory"}[kind]
        args = [operation, initial or os.path.expanduser("~")]
        if kind != "folder":
            args.append("\n".join("{}|{}".format(" ".join(patterns), name) for name, patterns in filters))
        args += ["--title", title]
        if x11_parent:
            args += ["--attach", x11_parent]
    else:
        args = ["--file-selection", "--title=" + title]
        if kind == "save":
            args += ["--save", "--confirm-overwrite"]
        elif kind == "folder":
            args += ["--directory"]
        if initial:
            args += ["--filename=" + initial]
        args += ["--file-filter={} | {}".format(name, " ".join(patterns)) for name, patterns in filters]
        if x11_parent:
            args += ["--attach=" + x11_parent]
    return args


def _fallback(kind, initial, filters, title, cancelled, x11_parent=None):
    """Run a supported graphical program, never a shell or terminal dialog."""
    if cancelled.is_set():
        return FileDialogResult(True, "cancelled")
    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        return FileDialogResult(True, "unavailable", error="No graphical desktop is available.")
    for backend in ("kdialog", "zenity"):
        executable = shutil.which(backend)
        if not executable:
            continue
        attached = bool(x11_parent)
        try:
            process = subprocess.Popen(
                [executable] + _fallback_arguments(backend, kind, initial, filters, title, x11_parent),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
            )
        except OSError as exc:
            return FileDialogResult(True, "error", backend=backend, error=str(exc), parent_attached=attached)
        while True:
            try:
                stdout, stderr = process.communicate(timeout=0.05)
                break
            except subprocess.TimeoutExpired:
                if cancelled.is_set():
                    process.terminate()
                    try:
                        process.communicate(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate()
                    return FileDialogResult(True, "cancelled", backend=backend, parent_attached=attached)
        error = stderr.decode("utf-8", "replace").strip()
        if cancelled.is_set() or (process.returncode == 1 and not stdout and not error):
            return FileDialogResult(True, "cancelled", backend=backend, parent_attached=attached)
        if process.returncode != 0:
            return FileDialogResult(
                True,
                "error",
                backend=backend,
                error=error or "Dialog exited with code {}.".format(process.returncode),
                parent_attached=attached,
            )
        # The tools add one newline. Do not strip meaningful spaces from names.
        path = os.fsdecode(stdout[:-1] if stdout.endswith(b"\n") else stdout)
        if not os.path.isabs(path) or "\0" in path:
            return FileDialogResult(
                True, "error", backend=backend, error="Dialog returned an invalid path.", parent_attached=attached
            )
        return FileDialogResult(True, "selected", path, backend, parent_attached=attached)
    return FileDialogResult(True, "unavailable", error="Portal, KDialog and Zenity are unavailable.")


class _Operation:
    def __init__(self, request, native, kind, initial, filters, title):
        self.request = request
        self.native = native
        self.kind, self.initial, self.filters, self.title = kind, initial, filters, title
        self.cancelled = threading.Event()
        self.results = queue.SimpleQueue()
        self.thread = None
        self.started = False
        self.generation = _generation
        self.prepared_at = time.monotonic()
        self.portal_error = None

    def cancel(self):
        self.cancelled.set()
        if self.native is not None:
            self.native.cancel()

    def start(self):
        self.started = True
        if renpy.macintosh:
            self.native.start()
        else:
            self.thread = threading.Thread(target=self.run, name="RenPy file dialog", daemon=True)
            self.thread.start()

    def run(self):
        try:
            if self.cancelled.is_set():
                result = FileDialogResult(True, "cancelled")
            else:
                self.native.run()
                status, path, backend, error, attached = self.native.result()
                result = FileDialogResult(True, status, path, backend, error, attached)
                if renpy.linux and status == "unavailable" and not self.cancelled.is_set():
                    self.portal_error = error
                    result = _fallback(
                        self.kind, self.initial, self.filters, self.title, self.cancelled, self.native.x11_parent()
                    )
                elif backend == "portal" and status == "selected":
                    result = FileDialogResult(True, "selected", _file_uri_path(path), backend, parent_attached=attached)
                if self.cancelled.is_set():
                    result = FileDialogResult(
                        True, "cancelled", backend=result.backend, parent_attached=result.parent_attached
                    )
        except Exception as exc:
            result = FileDialogResult(True, "error", error=str(exc))
        self.results.put(result)
        _wake()

    def collect(self):
        if getattr(renpy, "windows", False):
            self.native.pump()
        if not self.started:
            if self.cancelled.is_set():
                return FileDialogResult(True, "cancelled")
            if not self.native.prepare(time.monotonic() - self.prepared_at > 2):
                return None
            self.start()
        if renpy.macintosh:
            status, path, backend, error, attached = self.native.result()
            if status != "pending":
                return FileDialogResult(True, status, path, backend, error, attached)
            return None
        try:
            return self.results.get_nowait()
        except queue.Empty:
            return None


_active = None
_retired = []
_generation = 0


def _wake():
    try:
        from renpy.display.core import FILEDIALOG

        renpy.pygame.event.post(renpy.pygame.event.Event(FILEDIALOG))
    except Exception:
        # During shutdown the SDL event queue may already be gone. PERIODIC
        # also drains the queue while the engine is running.
        pass


def active():
    return _active is not None


def update():
    """Called by the SDL loop, always on its owning thread."""
    global _active
    for operation in list(_retired):
        if operation.collect() is not None:
            operation.native.release_parent()
            _retired.remove(operation)
    operation = _active
    if operation is None:
        return
    try:
        result = operation.collect()
    except Exception as exc:
        operation.cancel()
        result = FileDialogResult(True, "error", error=str(exc))
    if result is None:
        return
    operation.native.release_parent()
    _active = None
    if operation.generation != _generation:
        return
    operation.request._result = result
    engine_log = getattr(renpy.display, "log", None)
    if engine_log is not None:
        engine_log.write(
            "Native file dialog: status=%s backend=%s parent_attached=%s error=%s portal_unavailable=%s",
            result.status,
            result.backend,
            result.parent_attached,
            result.error,
            operation.portal_error,
        )
    if not result.parent_attached and result.backend:
        log.info("File dialog backend %s did not attach to the game window.", result.backend)
    if result.status in ("error", "unavailable"):
        log.warning("File dialog %s (%s): %s", result.status, result.backend, result.error)
    renpy.pygame.event.clear(renpy.display.core.input_events)
    renpy.exports.restart_interaction()


def reset(reason="The engine context changed."):
    global _active, _generation
    _generation += 1
    if _active is not None:
        operation = _active
        operation.cancel()
        operation.request._result = FileDialogResult(True, "cancelled", error=reason)
        _retired.append(operation)
        _active = None
    for operation in _retired:
        operation.cancel()


def shutdown():
    """Close owned dialogs before the engine tears down SDL and its windows."""
    reset("The game is exiting.")
    deadline = time.monotonic() + 8
    while _retired and time.monotonic() < deadline:
        renpy.pygame.event.pump()
        update()
        if _retired:
            time.sleep(0.01)
    if _retired:
        log.warning("File dialog backend did not finish closing before process shutdown.")


def show_file_dialog(kind, initial_path=None, filters=None, title=None, parent_window=None):
    """
    :doc: nativefiledialog

    Start an asynchronous desktop file dialog. See :doc:`nativefiledialog` for
    filters, request lifetime, results, and platform requirements.
    """
    global _active
    if threading.current_thread() is not threading.main_thread():
        raise RuntimeError("File dialogs must be requested on the main thread.")
    if renpy.game.context().init_phase or renpy.exports.predicting():
        raise RuntimeError("File dialogs cannot be requested during init or prediction.")
    if not getattr(renpy.game.interface, "started", False):
        raise RuntimeError("File dialogs require a running game interface.")
    if kind not in ("save", "open", "folder"):
        raise ValueError("kind must be save, open or folder.")
    initial = _normalise_path(kind, initial_path)
    filters = _normalise_filters(kind, filters)
    title = title if title is not None else {"save": "Save File", "open": "Open File", "folder": "Select Folder"}[kind]
    if not isinstance(title, str) or "\0" in title:
        raise ValueError("title must be a string without NUL characters.")
    if _active is not None or _retired:
        return FileDialogRequest(FileDialogResult(True, "error", error="Another file dialog is active or closing."))
    if renpy.mobile or renpy.emscripten:
        return FileDialogRequest(
            FileDialogResult(True, "unavailable", error="Native file dialogs require a desktop platform.")
        )
    from renpy.pygame.display import Window

    if parent_window is None:
        parent_window = renpy.pygame.display.get_window()
    if parent_window is not None and not isinstance(parent_window, Window):
        raise TypeError("parent_window must be an engine Window object.")
    try:
        from renpy import _nativefiledialog
    except ImportError:
        return FileDialogRequest(
            FileDialogResult(True, "unavailable", error="This runtime was built without native file dialog support.")
        )
    request = FileDialogRequest()
    try:
        native = _nativefiledialog.NativeRequest(kind, initial, title, filters, parent_window)
        _active = _Operation(request, native, kind, initial, filters, title)
        update()
        _wake()
    except Exception as exc:
        if _active is not None:
            reset(str(exc))
        request._result = FileDialogResult(True, "error", error=str(exc))
    return request
