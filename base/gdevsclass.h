/* Copyright (C) 2001-2019 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  1305 Grant Avenue - Suite 200, Novato,
   CA 94945, U.S.A., +1(415)492-9861, for further information.
*/


/* Common definitions for default subclass device */

#ifndef gdev_default_subclass_INCLUDED
#  define gdev_default_subclass_INCLUDED

#ifndef gxdevice_INCLUDED
#include "gxdevice.h"
#endif

typedef struct {
    subclass_common;
} default_subclass_subclass_data;

/* Device procedures, we need to prototype all of them */
dev_proc_open_device(default_subclass_open_device);
dev_proc_get_initial_matrix(default_subclass_get_initial_matrix);
dev_proc_sync_output(default_subclass_sync_output);
dev_proc_output_page(default_subclass_output_page);
dev_proc_close_device(default_subclass_close_device);
dev_proc_map_rgb_color(default_subclass_map_rgb_color);
dev_proc_map_color_rgb(default_subclass_map_color_rgb);
dev_proc_fill_rectangle(default_subclass_fill_rectangle);
dev_proc_tile_rectangle(default_subclass_tile_rectangle);
dev_proc_copy_mono(default_subclass_copy_mono);
dev_proc_copy_color(default_subclass_copy_color);
dev_proc_draw_line(default_subclass_draw_line);
dev_proc_get_bits(default_subclass_get_bits);
dev_proc_get_params(default_subclass_get_params);
dev_proc_put_params(default_subclass_put_params);
dev_proc_map_cmyk_color(default_subclass_map_cmyk_color);
dev_proc_get_xfont_procs(default_subclass_get_xfont_procs);
dev_proc_get_xfont_device(default_subclass_get_xfont_device);
dev_proc_map_rgb_alpha_color(default_subclass_map_rgb_alpha_color);
dev_proc_get_page_device(default_subclass_get_page_device);
dev_proc_get_alpha_bits(default_subclass_get_alpha_bits);
dev_proc_copy_alpha(default_subclass_copy_alpha);
dev_proc_get_band(default_subclass_get_band);
dev_proc_copy_rop(default_subclass_copy_rop);
dev_proc_fill_path(default_subclass_fill_path);
dev_proc_stroke_path(default_subclass_stroke_path);
dev_proc_fill_mask(default_subclass_fill_mask);
dev_proc_fill_trapezoid(default_subclass_fill_trapezoid);
dev_proc_fill_parallelogram(default_subclass_fill_parallelogram);
dev_proc_fill_triangle(default_subclass_fill_triangle);
dev_proc_draw_thin_line(default_subclass_draw_thin_line);
dev_proc_begin_image(default_subclass_begin_image);
dev_proc_image_data(default_subclass_image_data);
dev_proc_end_image(default_subclass_end_image);
dev_proc_strip_tile_rectangle(default_subclass_strip_tile_rectangle);
dev_proc_strip_copy_rop(default_subclass_strip_copy_rop);
dev_proc_get_clipping_box(default_subclass_get_clipping_box);
dev_proc_begin_typed_image(default_subclass_begin_typed_image);
dev_proc_get_bits_rectangle(default_subclass_get_bits_rectangle);
dev_proc_map_color_rgb_alpha(default_subclass_map_color_rgb_alpha);
dev_proc_create_compositor(default_subclass_create_compositor);
dev_proc_get_hardware_params(default_subclass_get_hardware_params);
dev_proc_text_begin(default_subclass_text_begin);
dev_proc_finish_copydevice(default_subclass_finish_copydevice);
dev_proc_begin_transparency_group(default_subclass_begin_transparency_group);
dev_proc_end_transparency_group(default_subclass_end_transparency_group);
dev_proc_begin_transparency_mask(default_subclass_begin_transparency_mask);
dev_proc_end_transparency_mask(default_subclass_end_transparency_mask);
dev_proc_discard_transparency_layer(default_subclass_discard_transparency_layer);
dev_proc_get_color_mapping_procs(default_subclass_get_color_mapping_procs);
dev_proc_get_color_comp_index(default_subclass_get_color_comp_index);
dev_proc_encode_color(default_subclass_encode_color);
dev_proc_decode_color(default_subclass_decode_color);
dev_proc_pattern_manage(default_subclass_pattern_manage);
dev_proc_fill_rectangle_hl_color(default_subclass_fill_rectangle_hl_color);
dev_proc_include_color_space(default_subclass_include_color_space);
dev_proc_fill_linear_color_scanline(default_subclass_fill_linear_color_scanline);
dev_proc_fill_linear_color_trapezoid(default_subclass_fill_linear_color_trapezoid);
dev_proc_fill_linear_color_triangle(default_subclass_fill_linear_color_triangle);
dev_proc_update_spot_equivalent_colors(default_subclass_update_spot_equivalent_colors);
dev_proc_ret_devn_params(default_subclass_ret_devn_params);
dev_proc_fillpage(default_subclass_fillpage);
dev_proc_push_transparency_state(default_subclass_push_transparency_state);
dev_proc_pop_transparency_state(default_subclass_pop_transparency_state);
dev_proc_put_image(default_subclass_put_image);
dev_proc_dev_spec_op(default_subclass_dev_spec_op);
dev_proc_copy_planes(default_subclass_copy_planes);
dev_proc_get_profile(default_subclass_get_profile);
dev_proc_set_graphics_type_tag(default_subclass_set_graphics_type_tag);
dev_proc_strip_copy_rop2(default_subclass_strip_copy_rop2);
dev_proc_strip_tile_rect_devn(default_subclass_strip_tile_rect_devn);
dev_proc_copy_alpha_hl_color(default_subclass_copy_alpha_hl_color);
dev_proc_process_page(default_subclass_process_page);
dev_proc_transform_pixel_region(default_subclass_transform_pixel_region);
dev_proc_fill_stroke_path(default_subclass_fill_stroke_path);


void default_subclass_finalize(const gs_memory_t *cmem, void *vptr);
#endif /* gdev_obj_filter_INCLUDED */
