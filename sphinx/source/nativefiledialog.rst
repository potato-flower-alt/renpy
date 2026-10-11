Native File Dialogs
==================

``renpy.show_file_dialog`` opens the desktop's save, open, or folder chooser
without blocking Ren'Py's event loop. It returns a temporary request with
``poll()`` and ``cancel()`` methods. Only one request may be active at a time.
An additional request returns an ``error`` result.

.. function:: renpy.show_file_dialog(kind, initial_path=None, filters=None, title=None, parent_window=None)

    ``kind`` is ``"save"``, ``"open"``, or ``"folder"``. Selections are single
    paths. ``initial_path`` may include a proposed filename for save or open.
    A missing initial directory is replaced by its nearest existing parent,
    preserving the filename. The function does not create directories or files.

    ``filters`` is a sequence of ``(description, patterns)`` pairs, for example
    ``[("Images", ["*.png", "*.jpg", "*.jpeg", "*.bmp"])]``. Portable patterns
    are ``*``, ``*.*``, and ``*.extension``. Folder dialogs ignore filters.
    Filtering is a chooser hint; callers must validate the chosen extension.
    No extension is appended automatically by this API.

    ``title`` defaults to Save File, Open File, or Select Folder. Translate
    custom titles in game code. ``parent_window`` defaults to the game window;
    an explicit value must be an engine Window, not an OS handle.

    Call from the main thread during runtime, never init or prediction.
    Invalid arguments raise ValueError or TypeError. Unsupported runtimes
    return an ``unavailable`` result.

Results
-------

``request.poll()`` immediately returns an immutable object with these fields:

* ``done``: False while pending, True for all terminal states.
* ``status``: ``pending``, ``selected``, ``cancelled``, ``unavailable``, or ``error``.
* ``path``: the selected absolute local path, only for ``selected``.
* ``backend``: ``windows-com``, ``macos-cocoa``, ``portal``, ``kdialog``, or ``zenity``.
  May be None when no backend started.
* ``error``: diagnostic text, or None. Cancellation is distinct from failure.
* ``parent_attached``: whether a parent was supplied to the backend. The desktop
  compositor ultimately decides how to place and associate the window.

``cancel()`` is idempotent, must run on the main thread, and asks the owning
backend to close. Polling continues to return pending until cleanup completes.
While a request is pending, Ren'Py keeps rendering and processing OS events,
blocks game input, and suspends dialogue auto-forward and skipping.

Lifetime and example
--------------------

Keep requests in a Python module, not a saved store variable. Put only the
chosen path in the store. Requests, callbacks, threads, and native resources
are not restored from saves or replayed by rollback. An accidentally pickled
request restores as an already cancelled placeholder. Load, restart, reload,
and exit cancel outstanding requests.

This module-style example uses screen-local polling and does not change
Ren'Py's existing Screenshot action::

    init python:
        import types
        file_dialog_demo = types.ModuleType("file_dialog_demo")
        file_dialog_demo.request = None

        def begin_file_dialog_demo():
            file_dialog_demo.request = renpy.show_file_dialog(
                "save", "~/Pictures/example.png",
                [("Images", ["*.png", "*.jpg", "*.jpeg", "*.bmp"])],
                title=_("Save image"))

        def poll_file_dialog_demo():
            result = file_dialog_demo.request.poll()
            if result.done:
                file_dialog_demo.request = None
                return result

    screen file_dialog_demo_wait():
        modal True
        timer 0.05 repeat True action Function(poll_file_dialog_demo)
        text _("Choose a location in the system dialog.") align (0.5, 0.5)

    label file_dialog_demo:
        $ begin_file_dialog_demo()
        call screen file_dialog_demo_wait
        if _return.status == "selected":
            $ selected_path = _return.path
            # Validate the format, then save the captured image here.
        elif _return.status in ("error", "unavailable"):
            $ renpy.notify(_return.error)
        return

Platforms
---------

Windows uses the COM common item dialogs; macOS uses Cocoa panels. Linux first
requests ``org.freedesktop.portal.FileChooser`` through the session bus.
Folder selection requires FileChooser version 3. If the service or capability
is unavailable, the engine tries installed KDialog and then Zenity. Games
do not need to install Python GUI packages. A cancelled or failed Portal
request does not trigger another chooser.

Linux uses X11 identifiers or exported Wayland handles for parenting. When
exporting is unavailable, an independent system window is permitted and
``parent_attached`` is False. This is recorded in the engine log. KDialog and
Zenity fallback parenting is available on X11. No terminal or in-game chooser
is used. Android, iOS, and Web are unsupported.

If the game window is still hidden, the chooser opens independently until
there is a visible window to parent it to.

Native dialogs allow users to create a directory through their own UI. This
API does not itself create a directory, test-write a file, or save a screenshot.
Filesystem permissions and image encoding errors belong to the caller's save
operation. macOS panels expose the combined set of allowed extensions rather
than a separate filter dropdown.

The legacy ``renpy.tfd`` interface and all its existing callers are unchanged.
