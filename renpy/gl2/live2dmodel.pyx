# Copyright 2004-2026 Tom Rothamel <pytom@bishoujo.us>
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

from libc.stdint cimport intptr_t
from libc.stdlib cimport malloc, free
from libc.string cimport memcpy, memcmp
from libc.stdio cimport printf
from libc.math cimport floor, ceil

from renpy.gl2.gl2mesh import TEXTURE_LAYOUT
from renpy.gl2.gl2mesh2 cimport Mesh2
from renpy.gl2.gl2polygon cimport Point2

from renpy.display.matrix cimport Matrix
from renpy.display.render cimport Render

from renpy.uguu.gl cimport GL_ZERO, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD, GL_DST_COLOR, GL_DST_ALPHA

import renpy
import weakref

cdef extern from "live2dcsm.h" nogil:
    void* load_live2d_object(const char* sofile)
    void* load_live2d_function(void* handle, const char* name)
    void deallocate_live2d_moc(void *moc)
    void deallocate_live2d_model(void *model)


cdef extern from "Live2DCubismCore.h":

    ctypedef struct csmMoc
    ctypedef struct csmModel
    ctypedef unsigned int csmVersion

    enum:
        csmAlignofMoc
        csmAlignofModel

    enum:
        csmBlendAdditive
        csmBlendMultiplicative
        csmIsDoubleSided
        csmIsInvertedMask

    enum:
        csmIsVisible
        csmVisibilityDidChange
        csmOpacityDidChange
        csmDrawOrderDidChange
        csmRenderOrderDidChange
        csmVertexPositionsDidChange

    ctypedef unsigned char csmFlags

    enum:
        csmMocVersion_Unknown
        csmMocVersion_30
        csmMocVersion_33
        csmMocVersion_40

    ctypedef unsigned int csmMocVersion

    ctypedef struct csmVector2:
        float X
        float Y

    ctypedef struct csmVector4:
        float X
        float Y
        float Z
        float W

    ctypedef void (__stdcall *csmLogFunction)(const char* message)

include "live2dcsm.pxi"


# Enable logging.
cdef void __stdcall log_function(const char *message) noexcept:
    print(message)

def post_init():
    csmSetLogFunction(log_function)

cdef class AlignedMemory:
    """
    This represents a region of aligned memory. The actual region of
    aligned memory is available through the .data pointer.
    """

    cdef void *base
    cdef void *data
    cdef int size

    def __init__(self, int size, intptr_t alignment, bytes data):
        self.base = malloc(size + alignment)
        if self.base == NULL:
            raise MemoryError()
        self.data = <void *> (((<intptr_t> self.base) + alignment) & ~(alignment-1))
        self.size = size

        memcpy(self.data, <void *> <unsigned char *> data, len(data))

    def __dealloc__(self):
        free(self.base)


cdef class Live2DMesh(Mesh2):
    """An immutable position snapshot sharing a drawable's static topology."""

    cdef Mesh2 topology
    cdef AlignedMemory positions

    def __init__(self, Mesh2 topology):
        Mesh2.__init__(self, topology.layout, 0, 0)
        self.topology = topology
        self.points = topology.points
        self.triangles = topology.triangles
        self.positions = AlignedMemory(self.points * sizeof(Point2), sizeof(float), b"")
        self.point = <Point2 *> self.positions.data
        self.point_data = <float *> self.point
        self.attribute = topology.attribute
        self.triangle = topology.triangle
        self.attribute_source = topology
        self.triangle_source = topology
        self.point_version = 1
        self.attribute_version = 1
        self.triangle_version = 1


cdef class Live2DMask(Render):
    """A weakly cached mask whose lifetime remains owned by the render tree."""

    cdef object __weakref__

class Parameter(object):
    """
    Represents the information known about a parameter.
    """

    def __init__(self, index, name, minimum, maximum, default):
        self.index = index
        self.name = name
        self.minimum = minimum
        self.maximum = maximum
        self.default = default
        self.remaining = 1.0

class Part(object):

    def __init__(self, index, name, default_opacity):
        self.index = index
        self.name = name
        self.default_opacity = default_opacity
        self.remaining = 1.0


cdef class Live2DModel:
    """
    Represents a Live2D model, generated from a MOC.
    """

    cdef AlignedMemory moc_data
    cdef csmMoc *moc
    cdef public csmMocVersion moc_version


    cdef AlignedMemory model_data
    cdef csmModel *model

    cdef csmVector2 pixel_size
    cdef csmVector2 pixel_origin
    cdef float pixels_per_unit

    cdef int parameter_count
    cdef const char **parameter_ids
    cdef const float *parameter_minimum_values
    cdef const float *parameter_maximum_values
    cdef const float *parameter_default_values
    cdef float *parameter_values

    cdef int part_count
    cdef const char **part_ids
    cdef float *part_opacities
    cdef const int *part_parent_part_indices

    cdef int drawable_count
    cdef const char **drawable_ids
    cdef const csmFlags *drawable_constant_flags
    cdef const csmFlags *drawable_dynamic_flags
    cdef const int *drawable_texture_indices
    cdef const int *drawable_draw_orders
    cdef const int *drawable_render_orders
    cdef const float *drawable_opacities
    cdef const int *drawable_mask_counts
    cdef const int **drawable_masks
    cdef const int *drawable_vertex_counts
    cdef const csmVector2 **drawable_vertex_positions
    cdef const csmVector2 **drawable_vertex_uvs
    cdef const int *drawable_index_counts
    cdef const unsigned short **drawable_indices
    cdef const csmVector4 *drawable_multiply_colors
    cdef const csmVector4 *drawable_screen_colors

    cdef public dict parameters
    cdef public dict parts

    cdef public dict parameter_groups
    cdef public dict opacity_groups

    cdef list meshes
    cdef list topologies
    cdef dict mask_cache

    cdef object filename

    _types = """
        parameters : dict[str, Parameter]
        parts : dict[str, Part]
        parameter_groups : dict
        opacity_groups : dict
        """


    def __dealloc__(self):
        """
        Deallocates the model, and the MOC.
        """

        if self.model is not NULL:
            deallocate_live2d_model(self.model)

        if self.moc is not NULL:
            deallocate_live2d_moc(self.moc)

    def __init__(self, fn):
        """
        Loads the Live2D model.
        """

        cdef int i

        self.filename = fn

        with renpy.loader.load(fn, directory="images") as f:
            data = f.read()

        # Load the MOC.
        self.moc_data = AlignedMemory(len(data), csmAlignofMoc, data)
        self.moc = csmReviveMocInPlace(self.moc_data.data, self.moc_data.size)

        if self.moc is NULL:
            raise Exception("Could not revive Live2D MOC.")

        self.moc_version = csmGetMocVersion(self.moc_data.data, self.moc_data.size)

        # Make a model.

        cdef unsigned int model_size = csmGetSizeofModel(self.moc)
        self.model_data = AlignedMemory(model_size, csmAlignofModel, b'')
        self.model = csmInitializeModelInPlace(self.moc, self.model_data.data, self.model_data.size)

        if self.model is NULL:
            raise Exception("Could not initialize Live2D Model.")

        csmReadCanvasInfo(self.model, &(self.pixel_size), &(self.pixel_origin), &(self.pixels_per_unit))

        # Query the model for pointers to all the things.

        self.parameter_count = csmGetParameterCount(self.model)
        self.parameter_ids = csmGetParameterIds(self.model)
        self.parameter_minimum_values = csmGetParameterMinimumValues(self.model)
        self.parameter_maximum_values = csmGetParameterMaximumValues(self.model)
        self.parameter_default_values = csmGetParameterDefaultValues(self.model)
        self.parameter_values = csmGetParameterValues(self.model)

        self.part_count = csmGetPartCount(self.model)
        self.part_ids = csmGetPartIds(self.model)
        self.part_opacities = csmGetPartOpacities(self.model)
        self.part_parent_part_indices = csmGetPartParentPartIndices(self.model)

        self.drawable_count = csmGetDrawableCount(self.model)
        self.drawable_ids = csmGetDrawableIds(self.model)
        self.drawable_constant_flags = csmGetDrawableConstantFlags(self.model)
        self.drawable_dynamic_flags = csmGetDrawableDynamicFlags(self.model)
        self.drawable_texture_indices = csmGetDrawableTextureIndices(self.model)
        self.drawable_draw_orders = csmGetDrawableDrawOrders(self.model)
        self.drawable_render_orders = csmGetDrawableRenderOrders(self.model)
        self.drawable_opacities = csmGetDrawableOpacities(self.model)
        self.drawable_mask_counts = csmGetDrawableMaskCounts(self.model)
        self.drawable_masks = csmGetDrawableMasks(self.model)
        self.drawable_vertex_counts = csmGetDrawableVertexCounts(self.model)
        self.drawable_vertex_positions = csmGetDrawableVertexPositions(self.model)
        self.drawable_vertex_uvs = csmGetDrawableVertexUvs(self.model)
        self.drawable_index_counts = csmGetDrawableIndexCounts(self.model)
        self.drawable_indices = csmGetDrawableIndices(self.model)
        self.drawable_multiply_colors = csmGetDrawableMultiplyColors(self.model)
        self.drawable_screen_colors = csmGetDrawableScreenColors(self.model)

        self.meshes = [None] * self.drawable_count
        self.topologies = [None] * self.drawable_count
        self.mask_cache = {}

        self.parameters = { }

        for i in range(self.parameter_count):
            name = self.parameter_ids[i].decode("utf-8")
            self.parameters[name] = Parameter(
                i, name,
                self.parameter_minimum_values[i],
                self.parameter_maximum_values[i],
                self.parameter_default_values[i],
                )

        self.parts = { }

        for i in range(self.part_count):
            name = self.part_ids[i].decode("utf-8")
            self.parts[name] = Part(i, name, self.part_opacities[i])

        self.opacity_groups = { }
        self.parameter_groups = { }

        csmUpdateModel(self.model)

    def __repr__(self):
        """
        Returns a string representation of the model.
        """

        return f"<Live2DModel {self.filename}>"

    def reset_parameters(self):
        """
        Resets the parameters, and resets the remaining weight to 1.0.
        """

        for i in self.parameters.values():
            self.parameter_values[i.index] = 0.0
            i.remaining = 1.0

        for i in self.parts.values():
            self.part_opacities[i.index] = 0.0
            i.remaining = 1.0


    def set_part_opacity(self, name, value):
        part = self.parts.get(name, None)

        if part is None:
            for i in self.opacity_groups.get(name, [ ]):
                self.set_part_opacity(i, value)
            return

        self.part_opacities[part.index] += value * part.remaining
        part.remaining = 0.0

    def set_parameter(self, name, value, weight=1.0):
        parameter = self.parameters.get(name, None)

        if parameter is None:
            for i in self.parameter_groups.get(name, [ ]):
                self.set_parameter(i, value, weight=weight)
            return

        weight = min(weight, parameter.remaining)

        old = self.parameter_values[parameter.index]
        self.parameter_values[parameter.index] += weight * value
        parameter.remaining -= weight

    def finish_parameters(self):
        for i in self.parameters.values():
            self.parameter_values[i.index] += i.default * i.remaining
            i.remaining = 0.0

        for i in self.parts.values():
            self.part_opacities[i.index] += i.default_opacity * i.remaining
            i.remaining = 0.0

    def blend_parameter(self, name, blend, value, weight=1.0):

        parameter = self.parameters.get(name, None)

        if parameter is None:
            for i in self.parameter_groups.get(name, [ ]):
                self.blend_parameter(i, blend, value, weight=weight)
            return

        old = self.parameter_values[parameter.index]

        if blend == "Multiply":
            value = old * value
        elif blend == "Add":
            value = old + value
        elif blend == "Overwrite":
            value = value

        self.parameter_values[parameter.index] = old + weight * (value - old)

    def blend_opacity(self, name, blend, value, weight=1.0):

        part = self.parts.get(name, None)

        if part is None:
            for i in self.opacity_groups.get(name, [ ]):
                self.blend_opacity(i, blend, value, weight=weight)
            return

        old = self.part_opacities[part.index]

        if blend == "Multiply":
            value = old * value
        elif blend == "Add":
            value = old + value
        elif blend == "Overwrite":
            value = value

        self.part_opacities[part.index] = old + weight * (value - old)


    def get_size(self):
        return (self.pixel_size.X, self.pixel_size.Y)

    def render(self, textures, zoom):

        cdef int i
        cdef int j
        cdef double w, h, offset_x, offset_y, ppu
        cdef double left, top, right, bottom, x, y

        cdef Render r
        cdef Render m
        cdef Render rv
        cdef Mesh2 mesh
        cdef Mesh2 topology

        shaders = ("renpy.texture", "live2d.flip_texture", "live2d.colors")
        mask_shaders = ("-renpy.texture", "live2d.mask", "live2d.flip_texture")
        inverted_mask_shaders = ("-renpy.texture", "live2d.inverted_mask", "live2d.flip_texture")

        csmUpdateModel(self.model)

        # Render the model.
        w = self.pixel_size.X * zoom
        h = self.pixel_size.Y * zoom

        rv = Render(w, h)
        if w <= 0 or h <= 0:
            return rv

        offset_x = self.pixel_origin.X * zoom
        offset_y = self.pixel_origin.Y * zoom

        ppu = self.pixels_per_unit * zoom

        reverse = Matrix.offset(offset_x, offset_y, 0.0) * Matrix.scale(ppu, -ppu, 1.0)
        forward = reverse.inverse()

        renders = [ ]
        raw_renders = [ ]
        mask_renders = [ ]
        mask_bounds = [ ]

        cdef csmVector4 multiply
        cdef csmVector4 screen

        # The indexes of layers that have been used as masks.
        used_masks = set()

        for i in range(self.drawable_count):

            # If this drawable is not visible, skip it.
            if self.drawable_dynamic_flags[i] & csmIsVisible == 0:
                continue

            if self.drawable_opacities[i] == 0.0:
                continue

            for j in range(self.drawable_mask_counts[i]):
                used_masks.add(self.drawable_masks[i][j])

        for i in range(self.drawable_count):

            # True if the layers is visible.
            is_visible = self.drawable_dynamic_flags[i] & csmIsVisible and self.drawable_opacities[i] != 0.0

            # True if the layer is used as a mask by a visible layer.
            is_mask = i in used_masks

            if not is_visible and not is_mask:
                mask_renders.append(None)
                mask_bounds.append(None)
                raw_renders.append(None)

                continue

            multiply = self.drawable_multiply_colors[i]
            screen = self.drawable_screen_colors[i]

            multiply_tuple = (multiply.X, multiply.Y, multiply.Z, multiply.W)
            screen_tuple = (screen.X, screen.Y, screen.Z, screen.W)

            topology = self.topologies[i]
            if topology is None:
                topology = Mesh2(TEXTURE_LAYOUT, self.drawable_vertex_counts[i], self.drawable_index_counts[i] // 3)
                topology.points = self.drawable_vertex_counts[i]
                topology.triangles = self.drawable_index_counts[i] // 3
                memcpy(topology.attribute, self.drawable_vertex_uvs[i], sizeof(float) * topology.points * 2)
                for j in range(topology.triangles * 3):
                    topology.triangle[j] = self.drawable_indices[i][j]
                self.topologies[i] = topology
                topology.attribute_version = 1
                topology.triangle_version = 1

            mesh = self.meshes[i]
            # Compare against the last snapshot, including after the drawable
            # was hidden or the shared Core model evaluated another instance.
            # This avoids relying on dynamic flags accumulated between updates.
            if mesh is None or (mesh.points and memcmp(mesh.point_data, self.drawable_vertex_positions[i], sizeof(float) * mesh.points * 2)):
                mesh = Live2DMesh(topology)
                memcpy(mesh.point_data, self.drawable_vertex_positions[i], sizeof(float) * mesh.points * 2)
                self.meshes[i] = mesh

            tex = textures[self.drawable_texture_indices[i]]

            if is_mask:

                # Create a render that can be used as a mask.
                mr = Render(w, h)
                mr.reverse = reverse
                mr.forward = forward
                mr.mesh = mesh

                mr.add_uniform("u_multiply", multiply_tuple)
                mr.add_uniform("u_screen", screen_tuple)

                for s in shaders:
                    mr.add_shader(s)

                mr.blit(tex, (0, 0))

                mask_renders.append(mr)

                # Masks are rasterized into separate textures. Keep their
                # bounds in model pixels so a small eye/mouth mask does not
                # allocate and clear a texture the size of the whole model.
                left, top, right, bottom = w, h, 0, 0
                for j in range(self.drawable_vertex_counts[i]):
                    x = offset_x + self.drawable_vertex_positions[i][j].X * ppu
                    y = offset_y - self.drawable_vertex_positions[i][j].Y * ppu
                    left = min(left, x)
                    top = min(top, y)
                    right = max(right, x)
                    bottom = max(bottom, y)
                mask_bounds.append((left, top, right, bottom))

            else:
                mask_renders.append(None)
                mask_bounds.append(None)

            if is_visible:

                # Create the render that is actually drawn.
                r = Render(w, h)
                r.reverse = reverse
                r.forward = forward
                r.mesh = mesh

                r.add_uniform("u_multiply", multiply_tuple)
                r.add_uniform("u_screen", screen_tuple)

                for s in shaders:
                    r.add_shader(s)

                r.blit(tex, (0, 0))

                raw_renders.append(r)

                if self.drawable_constant_flags[i] & csmBlendAdditive:
                    r.add_property("blend_func", (GL_FUNC_ADD, GL_ONE, GL_ONE, GL_FUNC_ADD, GL_ZERO, GL_ONE))
                elif self.drawable_constant_flags[i] & csmBlendMultiplicative:
                    r.add_property("blend_func", (GL_FUNC_ADD, GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD, GL_ZERO, GL_ONE))

                alpha = self.drawable_opacities[i]

                if alpha != 1.0:

                    r.add_shader("renpy.alpha")
                    r.add_uniform("u_renpy_alpha", alpha)
                    r.add_uniform("u_renpy_over", 1.0)

                renders.append((self.drawable_render_orders[i], r))

            else:
                raw_renders.append(None)

        multi_masks = { }
        cropped_masks = { }
        next_mask_cache = {}
        mask_token = (w, h, renpy.display.draw.texture_loader.mask_cache_token())

        for i in range(self.drawable_count):

            multiply = self.drawable_multiply_colors[i]
            screen = self.drawable_screen_colors[i]

            if self.drawable_mask_counts[i] == 0:
                continue

            r = raw_renders[i]

            # Layer is not visible.
            if r is None:
                continue

            if self.drawable_mask_counts[i] == 1:
                key = (self.drawable_masks[i][0],)
                m = mask_renders[key[0]]

            else:

                key = [ ]

                for j in range(self.drawable_mask_counts[i]):
                    key.append(self.drawable_masks[i][j])

                key = tuple(key)

                m = multi_masks.get(key, None)

                if m is None:
                    m = renpy.display.render.Render(w, h)

                    for j in key:
                        m.blit(mask_renders[j], (0, 0))

                    multi_masks[key] = m

            cropped = cropped_masks.get(key)
            if cropped is None:
                # A mask render is immutable. Reuse it only while its source
                # geometry, textures, colors and rasterization state match.
                # Mark-and-sweep may kill a mask retained by this Python cache.
                signature = tuple(
                    (self.meshes[j], id(textures[self.drawable_texture_indices[j]]), mask_renders[j].uniforms)
                    for j in key)
                cached = self.mask_cache.get(key)
                if cached is not None and cached[0] == mask_token and cached[1] == signature:
                    cached_mask = cached[2]()
                    if cached_mask is not None and not cached_mask.killed:
                        m = cached_mask
                        bounds = cached[3]
                        cropped = cropped_masks[key] = (m, bounds)

            if cropped is None:
                left, top, right, bottom = w, h, 0, 0
                for j in key:
                    bounds = mask_bounds[j]
                    left = min(left, bounds[0])
                    top = min(top, bounds[1])
                    right = max(right, bounds[2])
                    bottom = max(bottom, bounds[3])

                # Leave transparent texels around the geometry for bilinear
                # filtering, and preserve the original model-canvas clipping.
                left = max(0, floor(left) - 2)
                top = max(0, floor(top) - 2)
                right = min(w, ceil(right) + 2)
                bottom = min(h, ceil(bottom) + 2)
                if right > left and bottom > top:
                    m = m.subsurface((left, top, right - left, bottom - top), subpixel=True)
                    bounds = (left / w, top / h, (right - left) / w, (bottom - top) / h)
                else:
                    m = Render(1, 1)
                    bounds = (0.0, 0.0, 1.0 / w, 1.0 / h)
                # A weak reference lets mark-and-sweep release masks when a
                # model is hidden, even though its Core model stays cached.
                cached_render = Live2DMask(m.width, m.height)
                cached_render.blit(m, (0, 0))
                m = cached_render
                m.add_property("live2d_mask", True)
                cropped = cropped_masks[key] = (m, bounds)

            if key not in next_mask_cache:
                next_mask_cache[key] = (mask_token, signature, weakref.ref(cropped[0]), cropped[1])

            m, bounds = cropped

            if self.drawable_constant_flags[i] & csmIsInvertedMask:

                shaders = inverted_mask_shaders
            else:
                shaders = mask_shaders

            for s in shaders:
                r.add_shader(s)

            r.add_uniform("u_live2d_ppu", ppu)
            r.add_uniform("u_live2d_offset", (offset_x, offset_y))
            r.add_uniform("u_live2d_mask_bounds", bounds)

            r.blit(m, (0, 0))

        renders.sort()

        for t in renders:
            rv.subpixel_blit(t[1], (0, 0))

        self.mask_cache = next_mask_cache

        return rv
