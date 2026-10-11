testcase gl2_mesh_stream_reuse:
    enabled hasattr(renpy.display.draw, "state_cache")
    python:
        import array
        import builtins
        from renpy.gl2.gl2mesh2 import Mesh2
        from renpy.gl2.gl2mesh import SOLID_LAYOUT
        from renpy.gl2.gl2model import GL2Model

        draw = renpy.display.draw
        mesh = Mesh2(SOLID_LAYOUT, 5, 4)
        mesh.set_geometry_data(array.array("f", [0, 0, 100, 0, 100, 100, 0, 100, 50, 50]))
        mesh.set_triangle_data(array.array("I", [0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4]))
        model = GL2Model((100, 100), mesh, ("renpy.solid",), builtins.dict(u_renpy_solid_color=(1., 0., 0., 1.)))
        root = renpy.Render(100, 100)
        root.blit(model, (0, 0))
        for _ in range(3):
            draw.draw_screen(root, flip=False)
        before = draw.state_cache.mesh_uploads
        draw.draw_screen(root, flip=False)
        assert draw.state_cache.mesh_uploads == before
        mesh.set_geometry_data([0, 0, 100, 0, 100, 100, 0, 100, 40, 45])
        draw.draw_screen(root, flip=False)
        assert draw.state_cache.mesh_uploads == before + 1
        mesh.set_triangle_data([0, 1, 2, 0, 2, 3])
        draw.draw_screen(root, flip=False)
        assert draw.state_cache.mesh_uploads == before + 2
        root.kill()

testcase shader_prediction_during_redraw:
    enabled hasattr(renpy.display.draw, "state_cache")
    python:
        renpy.register_shader("test.prediction", fragment_300="gl_FragColor.rgb *= 0.9;")
        renpy.start_predict_shader("renpy.texture", "test.prediction")
        for _ in range(10):
            renpy.display.interface.idle_frame(False)
        cache = renpy.display.draw.shader_cache
        assert cache._normalize_partnames(("renpy.texture", "test.prediction")) in cache.cache
        renpy.stop_predict_shader("renpy.texture", "test.prediction")

testcase gl2_sampler_zero_after_rtt:
    enabled hasattr(renpy.display.draw, "state_cache")
    python:
        import builtins
        from renpy.gl2.gl2mesh2 import Mesh2
        from renpy.gl2.gl2model import GL2Model
        draw = renpy.display.draw
        renpy.register_shader("test.unbound_texture", variables="uniform sampler2D u_empty_texture;",
            fragment_300="gl_FragColor = texture2D(u_empty_texture, vec2(0.5));")
        red = renpy.render(Solid("#f00", xysize=(16, 16)), 16, 16, 0, 0)
        retained = draw.render_to_texture(red)
        root = renpy.Render(16, 16)
        root.blit(GL2Model((16, 16), Mesh2.rectangle(0, 0, 16, 16), ("test.unbound_texture",),
            builtins.dict(u_empty_texture=0)), (0, 0))
        surface = draw.screenshot(root)
        assert surface.get_at((surface.get_width() // 2, surface.get_height() // 2))[:3] == (0, 0, 0)
