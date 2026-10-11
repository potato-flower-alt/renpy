init python:
    def _test_live2d_mask_pixels(inverted, rectangles, crop, direct=False):
        """Exercise the real GL mask shader, including samples outside its crop."""
        from renpy.gl2.gl2mesh2 import Mesh2
        from renpy.display.matrix import Matrix

        size = 64
        mask = renpy.Render(size, size)
        for x, y, width, height in rectangles:
            white = renpy.render(Solid("#fff", xysize=(width, height)), width, height, 0, 0)
            mask.blit(white, (x, y))

        x, y, width, height = crop
        cropped = mask.subsurface(crop)
        if direct:
            cropped.add_property("live2d_mask", True)
        model = renpy.Render(size, size)
        model.mesh = Mesh2.texture_rectangle(-32, -32, 32, 32, 0, 0, 1, 1)
        model.reverse = Matrix.offset(32, 32, 0) * Matrix.scale(1, -1, 1)
        model.forward = model.reverse.inverse()
        model.add_shader("-renpy.texture")
        model.add_shader("live2d.inverted_mask" if inverted else "live2d.mask")
        model.add_shader("live2d.flip_texture")
        model.add_uniform("u_live2d_ppu", 1.0)
        model.add_uniform("u_live2d_offset", (32.0, 32.0))
        model.add_uniform("u_live2d_mask_bounds", (x/size, y/size, width/size, height/size))
        model.blit(renpy.render(Solid("#f00", xysize=(size, size)), size, size, 0, 0), (0, 0))
        model.blit(cropped, (0, 0))

        root = renpy.Render(size, size)
        root.blit(renpy.render(Solid("#00f", xysize=(size, size)), size, size, 0, 0), (0, 0))
        root.blit(model, (0, 0))
        surface = renpy.display.draw.screenshot(root)
        sw, sh = surface.get_size()
        for px, py in ((4, 4), (22, 14), (30, 24), (40, 38), (60, 60)):
            inside = any(rx <= px < rx+rw and ry <= py < ry+rh for rx, ry, rw, rh in rectangles)
            expected = (255, 0, 0) if inside != inverted else (0, 0, 255)
            actual = surface.get_at((int(px*sw/size), int(py*sh/size)))[:3]
            assert all(abs(a-b) <= 2 for a, b in zip(actual, expected)), (inverted, crop, px, py, actual, expected)

testcase live2d_cropped_masks:
    enabled renpy.has_live2d()
    python:
        for direct, inverted in ((False, False), (False, True), (True, False), (True, True)):
            # A tightly cropped white mask must not clamp its edge texels
            # across the rest of the model. Also check combined and empty masks.
            _test_live2d_mask_pixels(inverted, [(20, 12, 16, 16)], (20, 12, 16, 16), direct)
            _test_live2d_mask_pixels(inverted, [(20, 12, 16, 16), (38, 35, 10, 11)], (18, 10, 32, 38), direct)
            _test_live2d_mask_pixels(inverted, [], (0, 0, 1, 1), direct)

testcase live2d_mask_storage:
    enabled renpy.has_live2d()
    python:
        draw = renpy.display.draw

        def make_mask(draw=draw):
            mask = renpy.Render(97, 51)
            mask.add_property("live2d_mask", True)
            mask.fill("#fff")
            draw.load_all_textures(mask)
            return mask

        first = make_mask()
        retained = draw.render_to_texture(first)
        first_number = retained.number
        first.kill()
        draw.texture_loader.cleanup()

        # A transition/screenshot holding the old texture prevents reuse.
        second = make_mask()
        assert draw.render_to_texture(second).number != first_number

        # Retirement is deferred to a frame boundary, and then storage with
        # matching dimensions can be reused without overwriting second.
        del retained
        draw.texture_loader.cleanup()
        third = make_mask()
        if not config.depth_size:
            assert draw.render_to_texture(third).number == first_number
        second.kill()
        third.kill()
        draw.texture_loader.cleanup()
