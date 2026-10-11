#!/usr/bin/env python

# Copyright 2004-2025 Tom Rothamel <pytom@bishoujo.us>
#
# Permission is hereby granted, free of charge, to any person
# obtaining a copy of this software and associated documentation files
# (the "Software"), to deal in the Software without restriction,
# including without limitation the rights to use, copy, modify, merge,
# publish, distribute, sublicense, and/or sell copies of the Software,
# and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

import sys
import os

# Change to the directory containing this file.
BASE = os.path.abspath(os.path.dirname(sys.argv[0]))
os.chdir(BASE)

SCRIPTS = os.path.join(BASE, 'scripts')
sys.path.insert(0, SCRIPTS)

import setuplib
from setuplib import windows, cython, find_unnecessary_gen, generate_all_cython, env

import generate_styles

def main():

    setuplib.init()
    setuplib.check_imports(SCRIPTS, "setuplib.py", "generate_styles.py")

    generate_styles.generate()

    # These control the level of optimization versus debugging.
    setuplib.extra_compile_args = [ "-Wno-unused-function" ]
    setuplib.extra_link_args = [ ]

    cubism = os.environ.get("CUBISM", None)
    if cubism:
        setuplib.include_dirs.append("{}/Core/include".format(cubism))

    # src/ directory.
    cython("_renpy", [ "src/IMG_savepng.c", "src/core.c" ], packages="sdl2 libpng")

    # renpy
    cython("renpy.astsupport")
    cython("renpy.cslots")
    cython("renpy.lexersupport")
    cython("renpy.pydict")
    cython("renpy.style")
    cython("renpy.encryption")
    cython("renpy.aescrypt", [ "src/aes_rpa.c" ], packages="openssl")
    cython("renpy.tfd", [ "src/tinyfiledialogs/tinyfiledialogs.c" ])
    if sys.platform in ("win32", "linux", "darwin") and not os.environ.get("RENPY_WEB"):
        native_sources = ["src/nativefiledialog.cc"]
        native_packages = "sdl2"
        native_args = ["-std=c++17"]
        if sys.platform == "linux":
            native_packages += " dbus-1 wayland-client"
        elif sys.platform == "darwin":
            from setuptools._distutils.unixccompiler import UnixCCompiler

            # setuptools does not recognise Objective-C++ sources by default.
            # clang selects Objective-C++ from .mm; use C++ for compiler/linker
            # selection and include .mm in the accepted source suffixes.
            if ".mm" not in UnixCCompiler.src_extensions:
                UnixCCompiler.src_extensions = UnixCCompiler.src_extensions + [".mm"]
            UnixCCompiler.language_map = dict(UnixCCompiler.language_map, **{".mm": "c++"})
            native_sources.append("src/nativefiledialog_mac.mm")
            native_args += ["-fblocks"]
        cython("renpy._nativefiledialog", native_sources, language="c++", packages=native_packages, compile_args=native_args)
        native_extension = setuplib.extensions[-1]
        if sys.platform == "win32":
            native_extension.libraries += ["ole32", "shell32", "uuid", "user32"]
        elif sys.platform == "darwin":
            native_extension.extra_link_args += ["-framework", "Cocoa"]
        else:
            # Only headers are used from these packages; the runtime probes
            # their libraries with dlopen so missing backends remain optional.
            native_extension.libraries = [i for i in native_extension.libraries if i not in ("dbus-1", "wayland-client")]
    cython("renpy.ecsign", [ "src/ec_sign_core.c" ], packages="openssl")

    # renpy.audio
    cython(
        "renpy.audio.renpysound",
        [ "src/renpysound_core.c", "src/ffmedia.c", "src/spectrum.c" ],
        compile_args=[ "-Wno-deprecated-declarations" ] if ("RENPY_FFMPEG_NO_DEPRECATED_DECLARATIONS" in os.environ) else [ ],
        packages="libavfilter libavformat libavcodec libavutil libswresample libswscale sdl2")

    cython("renpy.audio.filter")

    # renpy.styledata
    cython("renpy.styledata.styleclass")
    cython("renpy.styledata.stylesets")

    for p in generate_styles.prefixes:
        cython("renpy.styledata.style_{}functions".format(p), pyx=setuplib.gen + "/style_{}functions.pyx".format(p))

    # renpy.display
    cython("renpy.display.matrix")
    cython("renpy.display.render")
    cython("renpy.display.accelerator", packages="sdl2")
    cython("renpy.display.quaternion")

    # renpy.uguu
    cython("renpy.uguu.gl", packages="sdl2")
    cython("renpy.uguu.uguu", packages="sdl2")

    # renpy.gl2
    cython("renpy.gl2.gl2mesh")
    cython("renpy.gl2.gl2mesh2")
    cython("renpy.gl2.gl2mesh3")
    cython("renpy.gl2.gl2polygon")
    cython("renpy.gl2.gl2model")
    cython("renpy.gl2.gl2draw")
    cython("renpy.gl2.gl2texture")
    cython("renpy.gl2.gl2uniform")
    cython("renpy.gl2.gl2shader")
    cython("renpy.gl2.gl2statecache")

    if cubism:
        cython("renpy.gl2.live2dmodel", [ "src/live2dcsm.c" ],)

    cython("renpy.gl2.assimp", [ "src/assimpio.cc" ], language="c++", packages="assimp sdl2")

    # renpy.text
    cython("renpy.text.textsupport")
    cython("renpy.text.texwrap")
    cython("renpy.text.ftfont", [ "src/ftsupport.c", "src/ttgsubtable.c" ], packages="freetype2 harfbuzz")
    cython("renpy.text.hbfont", [ "src/ftsupport.c" ], packages="freetype2 harfbuzz")
    cython("renpy.text.bidi", [ "src/renpybidicore.c" ], packages="fribidi")

    # renpy.pygame
    cython("renpy.pygame.error", packages="sdl2")
    cython("renpy.pygame.color", packages="sdl2")
    cython("renpy.pygame.controller", packages="sdl2")
    cython("renpy.pygame.rect", packages="sdl2")
    cython("renpy.pygame.rwobject", packages="sdl2")
    cython("renpy.pygame.surface", source=[ "src/pygame/alphablit.c" ], packages="sdl2")
    cython("renpy.pygame.display", packages="sdl2")
    cython("renpy.pygame.event", packages="sdl2")
    cython("renpy.pygame.locals", packages="sdl2")
    cython("renpy.pygame.key", packages="sdl2")
    cython("renpy.pygame.mouse", packages="sdl2")
    cython("renpy.pygame.joystick", packages="sdl2")
    cython("renpy.pygame.power", packages="sdl2")
    cython("renpy.pygame.pygame_time", packages="sdl2")
    cython("renpy.pygame.image", source=[ "src/pygame/write_jpeg.c", "src/pygame/write_png.c" ], packages="sdl2 SDL2_image libjpeg libpng")
    cython("renpy.pygame.transform", source=[ "src/pygame/SDL2_rotozoom.c" ], packages="sdl2")
    cython("renpy.pygame.gfxdraw", source=[ "src/pygame/SDL_gfxPrimitives.c" ], packages="sdl2")
    cython("renpy.pygame.draw", packages="sdl2")
    cython("renpy.pygame.scrap", packages="sdl2")

    generate_all_cython()
    find_unnecessary_gen()

    env("CC")
    env("LD")
    env("CXX")
    env("CFLAGS")
    env("LDFLAGS")

    setuplib.setup("renpy", "8.99.99")


if __name__ == "__main__":
    main()
