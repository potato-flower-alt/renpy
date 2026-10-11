# Automated desktop integration fixture. Copy to an output directory before running.
define config.name = "Native File Dialog Regression"
define config.save_directory = None
define config.developer = True

init python:
    import json
    import os
    import types

    native_fixture = types.ModuleType("native_fixture")
    native_fixture.request = None
    native_fixture.results = []

    def fixture_finalise():
        assert not renpy.nativefiledialog.active()
        assert not renpy.nativefiledialog._retired
        with open(os.path.join(config.basedir, "passed.json"), "w", encoding="utf-8") as stream:
            json.dump({
                "requests": native_fixture.results,
                "legacy_screenshot": True,
                "quit_cancelled_and_released": True,
            }, stream, indent=2)

    config.quit_callbacks.append(fixture_finalise)

    def fixture_begin(kind):
        initial = os.path.join(config.basedir, "missing", "child", "猫 100%.png")
        native_fixture.request = renpy.show_file_dialog(
            kind, initial,
            [("Images", ["*.png", "*.jpg", "*.jpeg", "*.bmp"])],
            title="Native API test ' \" 中文")

    def fixture_poll():
        result = native_fixture.request.poll()
        if result.done:
            assert result.status == "cancelled", result
            native_fixture.results.append({
                "status": result.status,
                "backend": result.backend,
                "parent_attached": result.parent_attached,
            })
            assert not os.path.exists(os.path.join(config.basedir, "missing"))
            native_fixture.request = None
            return True

screen fixture_wait():
    modal True
    text "Testing system chooser cancellation…" align (0.5, 0.5)
    timer 1.0 action Function(native_fixture.request.cancel)
    timer 0.05 repeat True action Function(fixture_poll)

label main_menu:
    jump start

label start:
    scene expression Solid("#18212b")
    $ fixture_begin("save")
    call screen fixture_wait
    $ fixture_begin("open")
    call screen fixture_wait
    $ fixture_begin("folder")
    call screen fixture_wait

    python:
        screenshot_path = os.path.join(config.basedir, "legacy-screenshot.png")
        renpy.screenshot(screenshot_path)
        with open(screenshot_path, "rb") as stream:
            assert stream.read(8) == b"\x89PNG\r\n\x1a\n"
        fixture_begin("save")
        renpy.quit()
