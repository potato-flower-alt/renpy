"""Run using system Python with dbus-python/GLib under dbus-run-session.

  dbus-run-session -- /usr/bin/python3 nativefiledialog_mock_portal.py DRIVER

Exercises the real libdbus bridge, not a Python reimplementation of the wire protocol.
"""

import subprocess
import sys
import threading
import unittest

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib


DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
service_name = dbus.service.BusName("org.freedesktop.portal.Desktop", bus)
loop = GLib.MainLoop()
driver = sys.argv.pop(1)
requests = []


class Request(dbus.service.Object):
    def __init__(self, path):
        super().__init__(bus, path)
        self.closed = threading.Event()

    @dbus.service.signal("org.freedesktop.portal.Request", signature="ua{sv}")
    def Response(self, code, results):
        pass

    @dbus.service.method("org.freedesktop.portal.Request", in_signature="", out_signature="")
    def Close(self):
        self.closed.set()


class Chooser(dbus.service.Object):
    mode = "selected"
    version = 3
    last_options = None

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v")
    def Get(self, interface, property):
        if interface != "org.freedesktop.portal.FileChooser" or property != "version":
            raise dbus.exceptions.DBusException("Unknown property", name="org.freedesktop.DBus.Error.UnknownProperty")
        return dbus.UInt32(self.version, variant_level=1)

    @dbus.service.method(
        "org.freedesktop.portal.FileChooser", in_signature="ssa{sv}", out_signature="o", sender_keyword="sender"
    )
    def SaveFile(self, parent, title, options, sender=None):
        return self.choose(parent, title, options, sender)

    @dbus.service.method(
        "org.freedesktop.portal.FileChooser", in_signature="ssa{sv}", out_signature="o", sender_keyword="sender"
    )
    def OpenFile(self, parent, title, options, sender=None):
        return self.choose(parent, title, options, sender)

    def choose(self, parent, title, options, sender):
        self.last_options = options
        token = str(options["handle_token"])
        if self.mode == "alternate":
            token += "_alternate"
        path = "/org/freedesktop/portal/desktop/request/{}/{}".format(sender[1:].replace(".", "_"), token)
        request = Request(path)
        requests.append(request)
        results = dbus.Dictionary(signature="sv")
        if self.mode == "cancel":
            request.Response(dbus.UInt32(1), results)
        elif self.mode == "failure":
            request.Response(dbus.UInt32(2), results)
        elif self.mode == "invalid_handle":
            return dbus.ObjectPath("/somewhere/else")
        elif self.mode not in ("wait", "disappear"):
            uris = [] if self.mode == "empty" else ["file:///tmp/%E7%8C%AB%20a%25100.png"]
            if self.mode == "multiple":
                uris.append("file:///tmp/another.png")
            if self.mode == "wrong_type":
                results["uris"] = dbus.UInt32(7)
            else:
                results["uris"] = dbus.Array(uris, signature="s")
            # Intentionally emit BEFORE returning the method reply.
            request.Response(dbus.UInt32(0), results)
        if self.mode == "disappear":
            GLib.timeout_add(100, self.disappear)
        return dbus.ObjectPath(path)

    def disappear(self):
        bus.release_name("org.freedesktop.portal.Desktop")
        return False


chooser = Chooser(bus, "/org/freedesktop/portal/desktop")
thread = threading.Thread(target=loop.run, daemon=True)
thread.start()


class PortalTests(unittest.TestCase):
    def run_driver(self, mode, kind="save", cancel=False):
        chooser.mode = mode
        chooser.version = 3
        result = subprocess.run(
            [driver, kind] + (["cancel"] if cancel else []), capture_output=True, text=True, timeout=8
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        return int(lines[0]), lines[1], lines[2], lines[3] if len(lines) > 3 else ""

    def test_selected_and_early_response(self):
        status, path, backend, error = self.run_driver("selected")
        self.assertEqual((status, backend, error), (1, "portal", ""))
        self.assertEqual(path, "file:///tmp/%E7%8C%AB%20a%25100.png")
        self.assertEqual(str(chooser.last_options["current_name"]), "猫 100%.png")
        self.assertEqual(bytes(chooser.last_options["current_folder"]), b"/tmp/\0")
        self.assertEqual(str(chooser.last_options["filters"][0][0]), "Images")
        self.assertEqual(str(chooser.last_options["filters"][0][1][0][1]), "*.png")

    def test_alternate_handle_early_response(self):
        self.assertEqual(self.run_driver("alternate")[0], 1)

    def test_user_cancel_and_backend_error(self):
        self.assertEqual(self.run_driver("cancel")[0], 2)
        self.assertEqual(self.run_driver("failure")[0], 4)

    def test_invalid_results(self):
        for mode in ("empty", "multiple", "wrong_type", "invalid_handle"):
            with self.subTest(mode=mode):
                self.assertEqual(self.run_driver(mode)[0], 4)

    def test_explicit_cancel_closes_request(self):
        self.assertEqual(self.run_driver("wait", cancel=True)[0], 2)
        self.assertTrue(requests[-1].closed.wait(1))

    def test_folder_options_and_version(self):
        self.assertEqual(self.run_driver("selected", "folder")[0], 1)
        self.assertTrue(chooser.last_options["directory"])
        self.assertNotIn("filters", chooser.last_options)
        chooser.version = 2
        result = subprocess.run([driver, "folder"], capture_output=True, text=True, timeout=8)
        self.assertTrue(result.stdout.startswith("3\n"), result.stdout)

    def test_z_service_disappears_and_then_is_unavailable(self):
        self.assertEqual(self.run_driver("disappear")[0], 4)
        self.assertEqual(self.run_driver("selected")[0], 3)


try:
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(PortalTests)
    outcome = unittest.TextTestRunner(verbosity=2).run(suite)
finally:
    loop.quit()
    thread.join(2)
sys.exit(not outcome.wasSuccessful())
