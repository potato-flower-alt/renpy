# Keep the request outside the saved store; save only the selected path.
define config.name = "Native File Dialog Demo"
define config.save_directory = None
default selected_path = None

init python:
    import types
    chooser_demo = types.ModuleType("chooser_demo")
    chooser_demo.request = None

    def chooser_begin(kind):
        chooser_demo.request = renpy.show_file_dialog(
            kind, "~/Pictures/example.png" if kind == "save" else "~/Pictures/",
            [("Images", ["*.png", "*.jpg", "*.jpeg", "*.bmp"])],
            title="Ren'Py native chooser")

    def chooser_poll():
        result = chooser_demo.request.poll()
        if result.done:
            chooser_demo.request = None
            return result

screen chooser_wait():
    modal True
    text "Choose a path in the system dialog." align (0.5, 0.5)
    timer 0.05 repeat True action Function(chooser_poll)

label main_menu:
    jump start

label start:
    scene expression Solid("#18212b")
    menu:
        "Save file":
            $ chooser_kind = "save"
        "Open file":
            $ chooser_kind = "open"
        "Choose folder":
            $ chooser_kind = "folder"
        "Quit":
            $ renpy.quit()
    $ chooser_begin(chooser_kind)
    call screen chooser_wait
    if _return.status == "selected":
        $ selected_path = _return.path
        "Selected: [selected_path]"
    elif _return.status == "cancelled":
        "Cancelled."
    else:
        $ chooser_error = _return.error
        "Chooser unavailable or failed: [chooser_error]"
    jump start
