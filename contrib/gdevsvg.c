/* Copyright (C) 2001-2012 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  7 Mt. Lassen Drive - Suite A-134, San Rafael,
   CA  94903, U.S.A., +1(415)492-9861, for further information.
   */


/* SVG (Scalable Vector Graphics) output device */

#include <stdio.h>
#include <stdlib.h>

#include "string_.h"
#include "gx.h"
#include "gserrors.h"
#include "gdevvec.h"
#include "stream.h"
#include "gxpath.h"
#include "gzcpath.h"
#include "gxdownscale.h"
#include "gsptype1.h"
#include "gsptype2.h"
#include "gxdevmem.h"
#include "gxdevcli.h"
#include "gxpcolor.h"
#include "gxcolor2.h"
#include "gzstate.h"


/*
* libpng versions 1.0.3 and later allow disabling access to the stdxxx
* files while retaining support for FILE * I/O.
*/
#define PNG_NO_CONSOLE_IO
/*
* Earlier libpng versions require disabling FILE * I/O altogether.
* This produces a compiler warning about no prototype for png_init_io.
* The right thing will happen at link time, since the library itself
* is compiled with stdio support.  Unfortunately, we can't do this
* conditionally depending on PNG_LIBPNG_VER, because this is defined
* in png.h.
*/
/*#define PNG_NO_STDIO*/
#include "png_.h"

/* SVG data constants */

#define XML_DECL    "<?xml version=\"1.0\" standalone=\"no\"?>"
#define SVG_XMLNS   "http://www.w3.org/2000/svg"
#define SVG_VERSION "1.1"

/* default resolution. */
#ifndef X_DPI
#  define X_DPI 300
#endif
#ifndef Y_DPI
#  define Y_DPI 300
#endif

/* internal line buffer */
#define SVG_LINESIZE 100

/* default constants */
#define SVG_DEFAULT_LINEWIDTH	1.0
#define SVG_DEFAULT_LINECAP	gs_cap_butt
#define SVG_DEFAULT_LINEJOIN	gs_join_miter
#define SVG_DEFAULT_MITERLIMIT	4.0

extern_st(st_gs_text_enum);
extern_st(st_gx_image_enum_common);

enum COLOR_TYPE{
	COLOR_NULL,
	COLOR_PURE,
	COLOR_DEV,
	COLOR_BINARY_HT,
	COLOR_COLOR_HT,
	COLOR_PATTERN2,
	COLOR_PATTERN1,
	COLOR_UNKNOWN
};


static char encoding_table[] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
'w', 'x', 'y', 'z', '0', '1', '2', '3',
'4', '5', '6', '7', '8', '9', '+', '/' };
static char *decoding_table = NULL;
static int mod_table[] = { 0, 2, 1 };


byte *base64_encode(gs_memory_t *memory, const unsigned char *data,
	size_t input_length,
	size_t *output_length) {

	*output_length = 4 * ((input_length + 2) / 3);

	byte *encoded_data = gs_alloc_bytes(memory, *output_length, "base64 buffer");
	memset(encoded_data, 0, *output_length);
	if (encoded_data == NULL) return NULL;

	for (int i = 0, j = 0; i < input_length;) {

		uint octet_a = i < input_length ? (unsigned char)data[i++] : 0;
		uint octet_b = i < input_length ? (unsigned char)data[i++] : 0;
		uint octet_c = i < input_length ? (unsigned char)data[i++] : 0;

		uint triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

		encoded_data[j++] = encoding_table[(triple >> 3 * 6) & 0x3F];
		encoded_data[j++] = encoding_table[(triple >> 2 * 6) & 0x3F];
		encoded_data[j++] = encoding_table[(triple >> 1 * 6) & 0x3F];
		encoded_data[j++] = encoding_table[(triple >> 0 * 6) & 0x3F];
	}

	for (int i = 0; i < mod_table[input_length % 3]; i++)
		encoded_data[*output_length - 1 - i] = '=';

	return encoded_data;
}



/* ---------------- Device definition ---------------- */

typedef struct gx_device_svg_s {
	/* superclass state */
	gx_device_vector_common;
	/* local state */
	int header;		/* whether we've written the file header */
	int dirty;		/* whether we need to rewrite the <g> element */
	int mark;		/* <g> nesting level */
	int start_clip_mark; /* <g> nesting level at start of clip, -1 if we didn't add groups */
	int page_count;	/* how many output_page calls we've seen */
	gx_color_index strokecolor, fillcolor;
	double linewidth;
	gs_line_cap linecap;
	gs_line_join linejoin;
	double miterlimit;
	gs_string class_string;
	int class_number;
	bool from_stroke_path;
	int usedIds;
	int highestUsedId;
	bool validClipPath;
	gx_clip_path *current_clip_path;
	gx_clip_path *current_image_clip_path;
	bool writing_clip;
} gx_device_svg;

#define svg_device_body(dname, depth)\
	std_device_dci_type_body(gx_device_svg, 0, dname, &st_device_svg, \
	DEFAULT_WIDTH_10THS * X_DPI / 10, \
	DEFAULT_HEIGHT_10THS * Y_DPI / 10, \
	X_DPI, Y_DPI, \
	(depth > 8 ? 3 : 1), depth, \
	(depth > 1 ? 255 : 1), (depth > 8 ? 255 : 0), \
	(depth > 1 ? 256 : 2), (depth > 8 ? 256 : 1))

static dev_proc_open_device(svg_open_device);
static dev_proc_output_page(svg_output_page);
static dev_proc_close_device(svg_close_device);

static dev_proc_get_params(svg_get_params);
static dev_proc_put_params(svg_put_params);

static dev_proc_fill_path(gdev_svg_fill_path);
static dev_proc_stroke_path(gdev_svg_stroke_path);

static dev_proc_begin_typed_image(svg_begin_typed_image);
static dev_proc_begin_image(svg_begin_image);
static dev_proc_fillpage(svg_fillpage);

static enum COLOR_TYPE svg_get_color_type(gx_device_svg * svg, const gx_drawing_color *pdc);

//static dev_proc_copy_alpha(svg_copy_alpha);
#define svg_device_procs \
{ \
	svg_open_device, \
	NULL,                   /* get_initial_matrix */\
	NULL,                   /* sync_output */\
	svg_output_page, \
	svg_close_device, \
	gx_default_rgb_map_rgb_color, \
	gx_default_rgb_map_color_rgb, \
	gdev_vector_fill_rectangle, \
	NULL,                   /* tile_rectangle */\
	NULL,                   /* copy_mono */\
	NULL,			        /* copy_color */\
	NULL,                   /* draw_line */\
	NULL,                   /* get_bits */\
	svg_get_params, \
	svg_put_params, \
	NULL,                   /* map_cmyk_color */\
	NULL,                   /* get_xfont_procs */\
	NULL,                   /* get_xfont_device */\
	NULL,                   /* map_rgb_alpha_color */\
	gx_page_device_get_page_device, \
	NULL,                   /* get_alpha_bits */\
	NULL,                   /* copy_alpha */\
	NULL,                   /* get_band */\
	NULL,                   /* copy_rop */\
	gdev_svg_fill_path, \
	gdev_svg_stroke_path, \
	NULL,			        /* fill_mask */\
	gdev_vector_fill_trapezoid, \
	gdev_vector_fill_parallelogram, \
	gdev_vector_fill_triangle, \
	NULL,			        /* draw_thin_line */\
	svg_begin_image,        /* begin_image */\
	NULL,                   /* image_data */\
	NULL,                   /* end_image */\
	NULL,                   /* strip_tile_rectangle */\
	NULL,					/* strip_copy_rop */\
	NULL,					/* get_clipping_box */\
	svg_begin_typed_image,  /* begin_typed_image*/\
	NULL,					/* get_bits_rectangle */\
	NULL,					/* map_color_rgb_alpha */\
	NULL,					/* create_compositor */\
	NULL,					/* get_hardware_params */\
	NULL,				    /* text_begin */\
	NULL,		            /* finish_copydevice */\
	NULL,				    /* begin_transparency_group - Deprecated */\
	NULL,		            /* end_transparency_group - Deprecated */\
	NULL,				    /* begin_transparency_mask - Deprecated */\
	NULL,					/* end_transparency_mask - Deprecated */\
	NULL,					/* discard_transparency_layer - Deprecated */\
	NULL,					/* get_color_mapping_procs */\
	NULL,					/* get_color_comp_index */\
	NULL,	                /* encode_color */\
	NULL,	                /* decode_color */\
	NULL,					/* pattern_manage */\
	NULL,					/* fill_rectangle_hl_color */\
	NULL,					/* include_color_space */\
	NULL,					/* fill_linear_color_scanline */\
	NULL,					/* fill_linear_color_trapezoid */\
	NULL,					/* fill_linear_color_triangle */\
	NULL,					/* update_spot_equivalent_colors */\
	NULL,					/* ret_devn_params */\
	svg_fillpage,			/* fillpage */\
	NULL,					/* push_transparency_state */\
	NULL,					/* pop_transparency_state */\
	NULL					/* put_image */\
}

gs_public_st_suffix_add0_final(st_device_svg, gx_device_svg,
	"gx_device_svg",
	device_svg_enum_ptrs, device_svg_reloc_ptrs,
	gx_device_finalize, st_device_vector);

/* The output device is named 'svg' but we're referred to as the
   'svgwrite' device by the build system to avoid conflicts with
   the svg interpreter */
const gx_device_svg gs_svgwrite_device = {
	svg_device_body("svg", 24),
	svg_device_procs
};

/* Vector device procedures */

static int
svg_beginpage(gx_device_vector *vdev);
static int
svg_setlinewidth(gx_device_vector *vdev, double width);
static int
svg_setlinecap(gx_device_vector *vdev, gs_line_cap cap);
static int
svg_setlinejoin(gx_device_vector *vdev, gs_line_join join);
static int
svg_setmiterlimit(gx_device_vector *vdev, double limit);
static int
svg_setdash(gx_device_vector *vdev, const float *pattern,
uint count, double offset);
static int
svg_setlogop(gx_device_vector *vdev, gs_logical_operation_t lop,
gs_logical_operation_t diff);

static int
svg_can_handle_hl_color(gx_device_vector *vdev, const gs_imager_state *pis,
const gx_drawing_color * pdc);
static int
svg_setfillcolor(gx_device_vector *vdev, const gs_imager_state *pis,
const gx_drawing_color *pdc);
static int
svg_setstrokecolor(gx_device_vector *vdev, const gs_imager_state *pis,
const gx_drawing_color *pdc);

static int
svg_writeclip(gx_device_svg *svg, gx_clip_path *pcpath, gs_matrix matrix);
static void
close_clip_groups(gx_device_svg *svg);
static int
svg_dorect(gx_device_vector *vdev, fixed x0, fixed y0,
fixed x1, fixed y1, gx_path_type_t type);
static int
svg_beginpath(gx_device_vector *vdev, gx_path_type_t type);

static int
svg_moveto(gx_device_vector *vdev, double x0, double y0,
double x, double y, gx_path_type_t type);
static int
svg_lineto(gx_device_vector *vdev, double x0, double y0,
double x, double y, gx_path_type_t type);
static int
svg_curveto(gx_device_vector *vdev, double x0, double y0,
double x1, double y1, double x2, double y2,
double x3, double y3, gx_path_type_t type);
static int
svg_closepath(gx_device_vector *vdev, double x, double y,
double x_start, double y_start, gx_path_type_t type);
static int
svg_endpath(gx_device_vector *vdev, gx_path_type_t type);


/* PNG related functions and definitions */

/* structure to store PNG image bytes */
struct mem_encode
{
	char *buffer;
	size_t size;
	gs_memory_t *memory;
};

typedef struct svg_image_enum_s {
	gx_image_enum_common;
	int rows_left;
	int width;
	int height;
	bool invert;
	png_struct *png_ptr;
	png_info *info_ptr;
	struct mem_encode state;
	gs_matrix_fixed ctm;
	gs_matrix ImageMatrix;
} svg_image_enum_t;


struct png_setup_s{
	gs_memory_t *memory;
	png_struct *png_ptr;
	png_info *info_ptr;
	int depth;
	bool *external_invert;
	float HWResolution[2];
	uint width_in;
	uint height_in;
};

int setup_png_from_struct(gx_device * pdev, struct png_setup_s* setup);
int setup_png(gx_device * pdev, svg_image_enum_t  *pie, const gs_color_space *pcs);
int make_png(gx_device_memory *mdev);
void my_png_write_data(png_structp png_ptr, png_bytep data, png_size_t length);
void my_png_flush(png_structp png_ptr);
static int write_base64_png(gx_device* dev,
struct mem_encode *state,
	gs_matrix_fixed ctm,
	gs_matrix ImageMatrix,
	uint width,
	uint height);
static int make_png_from_mdev(gx_device_memory *mdev, float tx, float ty);

/* Vector device function table */

static const gx_device_vector_procs svg_vector_procs = {
	/* Page management */
	svg_beginpage,
	/* Imager state */
	svg_setlinewidth,
	svg_setlinecap,
	svg_setlinejoin,
	svg_setmiterlimit,
	svg_setdash,
	gdev_vector_setflat,
	svg_setlogop,
	/* Other state */
	svg_can_handle_hl_color,
	svg_setfillcolor,
	svg_setstrokecolor,
	/* Paths */
	gdev_vector_dopath,
	svg_dorect,
	svg_beginpath,
	svg_moveto,
	svg_lineto,
	svg_curveto,
	svg_closepath,
	svg_endpath
};

/* local utility prototypes */

static int svg_write_bytes(gx_device_svg *svg,
	const char *string, uint length);
static int svg_write(gx_device_svg *svg, const char *string);

static int svg_write_header(gx_device_svg *svg);

static void close_groups(gx_device_svg *svg, const int mark)
{
	while (svg->mark > mark)
	{
		--svg->mark;
		svg_write(svg, "</g>\n");
	}
}

/* Driver procedure implementation */

/* Open the device */
static int
svg_open_device(gx_device *dev)
{
	gx_device_vector *const vdev = (gx_device_vector*)dev;
	gx_device_svg *const svg = (gx_device_svg*)dev;
	int code = 0;

	vdev->v_memory = dev->memory;
	vdev->vec_procs = &svg_vector_procs;
	gdev_vector_init(vdev);
	code = gdev_vector_open_file_options(vdev, 512,
		VECTOR_OPEN_FILE_SEQUENTIAL);
	if (code < 0)
		return gs_rethrow_code(code);

	/* svg-specific initialization goes here */
	svg->header = 0;
	svg->dirty = 0;
	svg->mark = 0;
	svg->start_clip_mark = -1; // Note: -1 is invalid
	svg->page_count = 0;
	svg->strokecolor = gx_no_color_index;
	svg->fillcolor = gx_no_color_index;
	/* these should be the graphics library defaults instead? */
	svg->linewidth = SVG_DEFAULT_LINEWIDTH;
	svg->linecap = SVG_DEFAULT_LINECAP;
	svg->linejoin = SVG_DEFAULT_LINEJOIN;
	svg->miterlimit = SVG_DEFAULT_MITERLIMIT;
	svg->class_string.data = 0;
	svg->class_string.size = 0;
	svg->from_stroke_path = false;
	svg->usedIds = 0;
	svg->highestUsedId = 0;
	svg->validClipPath = false;
	svg->current_clip_path = NULL;
	svg->current_image_clip_path = NULL;
	svg->writing_clip = false;

	return code;
}

/* Complete a page */
static int
svg_output_page(gx_device *dev, int num_copies, int flush)
{
	gx_device_svg *const svg = (gx_device_svg*)dev;
	int code;
	uint used;
	char line[300];

	svg->page_count++;
	/* close any open group elements */
	close_groups(svg, 0);
	svg_write(svg, "</page>\n");
	svg_write(svg, "<page clip-path='url(#clip0)'>\n");
	svg->dirty = true; // Rewrite attributes for new svg items
	/* Scale drawing so our coordinates are in pixels */
	gs_sprintf(line, "<g transform='scale(%lf,%lf)'>\n",
		72.0 / svg->HWResolution[0],
		72.0 / svg->HWResolution[1]);
	/* svg_write(svg, line); */
	svg_write(svg, line);
	svg->mark++;

	svg_write(svg, "\n<!-- svg_output_page -->\n");
	if (ferror(svg->file))
		return gs_throw_code(gs_error_ioerror);

	if ((code = gx_finish_output_page(dev, num_copies, flush)) < 0)
		return code;

	/* Check if we need to change the output file for separate pages */
	if (gx_outputfile_is_separate_pages(((gx_device_vector *)dev)->fname, dev->memory)) {
		if ((code = svg_close_device(dev)) < 0)
			return code;
		code = svg_open_device(dev);
	}
	return code;
}

/* Close the device */
static int
svg_close_device(gx_device *dev)
{
	gx_device_svg *const svg = (gx_device_svg*)dev;
	/* close any open group elements */
	close_groups(svg, 0);
	svg_write(svg, "\n<!-- svg_close_device -->\n");

	if (svg->header) {
		svg_write(svg, "</page>\n");
		svg_write(svg, "</pageSet>\n");
		svg_write(svg, "</svg>\n");
		svg->header = 0;
	}

	if (ferror(svg->file))
		return gs_throw_code(gs_error_ioerror);

	return gdev_vector_close_file((gx_device_vector*)dev);
}

/* Respond to a device parameter query from the client */
static int
svg_get_params(gx_device *dev, gs_param_list *plist)
{
	int code = 0;

	if_debug0m('_', dev->memory, "svg_get_params\n");

	/* call our superclass to add its standard set */
	code = gdev_vector_get_params(dev, plist);
	if (code < 0)
		return gs_rethrow_code(code);

	/* svg specific parameters are added to plist here */

	return code;
}

/* Read the device parameters passed to us by the client */
static int
svg_put_params(gx_device *dev, gs_param_list *plist)
{
	int code = 0;

	if_debug0m('_', dev->memory, "svg_put_params\n");

	/* svg specific parameters are parsed here */

	/* call our superclass to get its parameters, like OutputFile */
	code = gdev_vector_put_params(dev, plist);
	if (code < 0)
		return gs_rethrow_code(code);

	return code;
}


/* RGB mapping for 32-bit RGBA color devices */

static gx_color_index
svgalpha_encode_color(gx_device * dev, const gx_color_value cv[])
{
	/* low 7 are alpha, stored inverted to avoid white/opaque
	* being 0xffffffff which is also gx_no_color_index.
	* So 0xff is transparent and 0x00 is opaque.
	* We always return opaque colors (bits 0-7 = 0).
	* Return value is 0xRRGGBB00.
	*/
	return
		((uint)gx_color_value_to_byte(cv[2]) << 8) +
		((ulong)gx_color_value_to_byte(cv[1]) << 16) +
		((ulong)gx_color_value_to_byte(cv[0]) << 24);
}

/* Map a color index to a r-g-b color. */
static int
svgalpha_decode_color(gx_device * dev, gx_color_index color,
gx_color_value prgb[3])
{
	prgb[0] = gx_color_value_from_byte((color >> 24) & 0xff);
	prgb[1] = gx_color_value_from_byte((color >> 16) & 0xff);
	prgb[2] = gx_color_value_from_byte((color >> 8) & 0xff);
	return 0;
}

static int make_alpha_mdev(gx_device *dev, gx_device_memory **ppmdev, gs_fixed_rect bbox,
	int depth)
{
	int i;
	gx_device_memory proto;
	const gx_device_memory *mdproto = gdev_mem_device_for_bits(depth);
	memcpy(&proto, mdproto, sizeof(gx_device_memory));
	/* Duplicate pngalpha */
	proto.color_info.max_components = 3;
	proto.color_info.num_components = 3;
	proto.color_info.polarity = GX_CINFO_POLARITY_ADDITIVE;
	proto.color_info.depth = depth;
	proto.color_info.gray_index = -1;
	proto.color_info.max_gray = 255;
	proto.color_info.max_color = 255;
	proto.color_info.dither_grays = 256;
	proto.color_info.dither_colors = 256;
	proto.color_info.anti_alias.graphics_bits = 4;
	proto.color_info.anti_alias.text_bits = 4;
	proto.color_info.separable_and_linear = GX_CINFO_SEP_LIN_NONE;
	for (i = 0; i < GX_DEVICE_COLOR_MAX_COMPONENTS; ++i)
	{
		proto.color_info.comp_bits[i] = 0;
		proto.color_info.comp_shift[i] = 0;
		proto.color_info.comp_mask[i] = 0;
	}
	proto.color_info.cm_name = "DeviceRGB";
	proto.color_info.opmode = GX_CINFO_OPMODE_UNKNOWN;
	proto.color_info.process_comps = 0;
	proto.color_info.black_component = 0;

	gs_make_mem_device_with_copydevice(ppmdev, &proto, dev->memory, -1, dev);
	(*ppmdev)->width = fixed2int(bbox.q.x - bbox.p.x);
	(*ppmdev)->height = fixed2int(bbox.q.y - bbox.p.y);
	(*ppmdev)->band_offset_x = 0;
	(*ppmdev)->band_offset_y = 0;
	(*ppmdev)->mapped_x = fixed2int(bbox.p.x);
	(*ppmdev)->mapped_y = fixed2int(bbox.p.y);
	(*ppmdev)->bitmap_memory = dev->memory;
	dev_proc((*ppmdev), encode_color) = svgalpha_encode_color;
	dev_proc((*ppmdev), decode_color) = svgalpha_decode_color;
}

static void
print_path(const gx_path *path)
{
	gs_path_enum penum;

	gx_path_enum_init(&penum, path);
	for (;;) {
		gs_fixed_point pts[3];

		switch (gx_path_enum_next(&penum, pts)) {
		case gs_pe_moveto:
			dmprintf2(path->memory, "    %g %g moveto\n", fixed2float(pts[0].x),
				fixed2float(pts[0].y));
			continue;
		case gs_pe_lineto:
			dmprintf2(path->memory, "    %g %g lineto\n", fixed2float(pts[0].x),
				fixed2float(pts[0].y));
			continue;
		case gs_pe_gapto:
			dmprintf2(path->memory, "    %g %g gapto\n", fixed2float(pts[0].x),
				fixed2float(pts[0].y));
			continue;
		case gs_pe_curveto:
			dmprintf6(path->memory, "    %g %g %g %g %g %g curveto\n", fixed2float(pts[0].x),
				fixed2float(pts[0].y), fixed2float(pts[1].x),
				fixed2float(pts[1].y), fixed2float(pts[2].x),
				fixed2float(pts[2].y));
			continue;
		case gs_pe_closepath:
			dmputs(path->memory, "    closepath\n");
			continue;
		default:
			break;
		}
		break;
	}
}

/* Stroke a path. */
static int
gdev_svg_stroke_path(gx_device * dev, const gs_imager_state * pis,
gx_path * ppath, const gx_stroke_params * params,
const gx_drawing_color * pdcolor, const gx_clip_path * pcpath)
{
	gx_device_svg *svg = (gx_device_svg *)dev;
	gx_drawing_color color;
	color_unset(&color);
	int code = 0;
	int mark = 0;
	int i;
	switch (svg_get_color_type((gx_device_svg *)dev, pdcolor))
	{
	case COLOR_PURE:
		// Record the clip path so that we can clip the shape if necessary
		svg->current_clip_path = pcpath;
		code = gdev_vector_stroke_path(dev, pis, ppath, params, pdcolor, pcpath);
		svg->current_clip_path = NULL;
		return code;
	default:
		svg_write(svg, "<g class='pathstrokeimage'>\n");
		mark = svg->mark;
		svg->from_stroke_path = true;
		code = gdev_vector_stroke_path(dev, pis, ppath, params, &color, pcpath);
		close_groups(svg, mark);
		if (code >= 0)
		{
			svg_write(svg, "<g class='image'>\n");
			/* Make image from stroked path */
			gs_fixed_rect bbox;
			gx_device_memory *pmdev;

			code = gx_path_bbox(ppath, &bbox);
			make_alpha_mdev(dev, &pmdev, bbox, 32);

			//pmdev->color_info = dev->color_info;
			code = (*dev_proc(pmdev, open_device))((gx_device *)pmdev);
			code = (*dev_proc(pmdev, fill_rectangle))((gx_device *)pmdev, 0, 0, 
				pmdev->width, pmdev->height, 0xffffffff);
			/* Translate the paths */
			gx_path_translate(ppath, -bbox.p.x, -bbox.p.y);
			gx_path_translate(pcpath, -bbox.p.x, -bbox.p.y);
			code = (*dev_proc(pmdev, stroke_path))((gx_device *)pmdev, pis, ppath, params, pdcolor, pcpath);
			/* Restore the paths to their original locations. Maybe not needed */
			gx_path_translate(ppath, bbox.p.x, bbox.p.y);
			gx_path_translate(pcpath, bbox.p.x, bbox.p.y);
			make_png_from_mdev(pmdev, fixed2float(bbox.p.x), fixed2float(bbox.p.y));
			code = (*dev_proc(pmdev, close_device))((gx_device *)pmdev);
			while (svg->mark > mark) {
				svg_write(svg, "</g>\n");
				svg->mark--;
			}
			svg_write(svg, "</g> <!-- End of image -->\n");
		}
		svg_write(svg, "</g> <!-- End of pathstrokeimage -->\n");
		svg->from_stroke_path = false;
		return code;
	}
}

/* Fill a path */
int
gdev_svg_fill_path(gx_device * dev, 
const gs_imager_state * pis,
gx_path * ppath,
const gx_fill_params * params,
const gx_drawing_color * pdcolor, 
const gx_clip_path * pcpath)
{
	gx_device_svg *svg = (gx_device_svg *)dev;
	gx_drawing_color color;
	color_unset(&color);
	int code = 0;

	int ctype = svg_get_color_type((gx_device_svg *)dev, pdcolor);
	switch (ctype)
	{
	case COLOR_PURE:
		// Record the clip path so that we can clip the shape if necessary
		svg->current_clip_path = pcpath;
		code = gdev_vector_fill_path(dev, pis, ppath, params, pdcolor, pcpath);
		svg->current_clip_path = NULL;
		return code;
	case COLOR_PATTERN1:
	if(true){

		gs_fixed_rect bbox;
		gx_device_memory *pmdev;
		int mark = svg->mark;
		gs_matrix m;
		gx_drawing_color dc = *pdcolor;
		gs_pattern1_instance_t pi = *(gs_pattern1_instance_t *)dc.ccolor.pattern;
		gs_state *pgs = gs_state_copy(pi.saved, gs_state_memory(pi.saved));

		gs_make_identity(&m);

		if (pgs == NULL)
			return_error(gs_error_VMerror);
		dc.ccolor.pattern = (gs_pattern_instance_t *)&pi;
		pi.saved = pgs;
		svg_write(svg, "<g class='pathfillimage'>\n");

		// First we need to fill the bounding box with the pattern
		code = gx_path_bbox(ppath, &bbox);

		make_alpha_mdev(dev, &pmdev, bbox, pdcolor->colors.pattern.p_tile->depth);
		pmdev->color_info = dev->color_info;


		code = (*dev_proc(pmdev, open_device))((gx_device *)pmdev);
		code = (*dev_proc(pmdev, fill_rectangle))((gx_device *)pmdev, 0, 0,
			pmdev->width, pmdev->height, 0xffffffff);


		int sx = fixed2int(bbox.p.x);
		int sy = fixed2int(bbox.p.y);
		gs_int_point rect_size;
		rect_size.x = fixed2int(bbox.q.x + fixed_half) - sx;
		rect_size.y = fixed2int(bbox.q.y + fixed_half) - sy;
		if (rect_size.x == 0 || rect_size.y == 0) return 0;


		/* Translate the paths */
		gx_path_translate(ppath, -bbox.p.x, -bbox.p.y);
		gx_path_translate(pcpath, -bbox.p.x, -bbox.p.y);

		/* Translate the pattern as well */
		dc.mask.m_phase.x += fixed2int(bbox.p.x);
		dc.mask.m_phase.y += fixed2int(bbox.p.y);

		code = (*dev_proc(pmdev, fill_path))((gx_device *)pmdev, 
			pis, ppath, params, /*pdcolor*/ &dc, pcpath);

		/* Restore the paths to their original locations. Maybe not needed */
		gx_path_translate(ppath, bbox.p.x, bbox.p.y);
		gx_path_translate(pcpath, bbox.p.x, bbox.p.y);

		svg->current_image_clip_path = pcpath;

		/* Restore the paths to their original locations. Maybe not needed */
		make_png_from_mdev(pmdev, fixed2float(bbox.p.x), fixed2float(bbox.p.y));
		code = (*dev_proc(pmdev, close_device))((gx_device *)pmdev);

		close_clip_groups(svg);
		svg_write(svg, "</g> <!-- pathfillimage -->\n");

		gs_state_free(pgs);

		return code;
	}
	default:
		if (svg->from_stroke_path || !gx_dc_is_pattern2_color(pdcolor))
		{
			return gx_default_fill_path(dev, pis, ppath, params, pdcolor, pcpath);
		}
		else
		{
			gs_fixed_rect bbox, bbox1;
			gx_device_memory *pmdev;
			int mark = svg->mark;
			gs_imager_state *pis_noconst = (gs_imager_state *)pis; /* Break const. */
			gs_matrix_fixed oldCTM = pis_noconst->ctm;
			gs_matrix save_ctm = ctm_only(pis);
			gs_matrix m;
			gx_drawing_color dc = *pdcolor;
			gs_pattern2_instance_t pi = *(gs_pattern2_instance_t *)dc.ccolor.pattern;

			// Note: Do not use gs_state_memory(pi.saved)
			// This strips the pointer to 32 bits and causes a crash
			gs_state *pgs = gs_state_copy(pi.saved, pi.saved->memory /*gs_state_memory(pi.saved)*/);

			gs_make_identity(&m);

			if (pgs == NULL)
				return_error(gs_error_VMerror);
			dc.ccolor.pattern = (gs_pattern_instance_t *)&pi;
			pi.saved = pgs;
			svg_write(svg, "<g class='pathfillimage'>\n");

			// First we need to fill the bounding box with the pattern
			code = gx_path_bbox(ppath, &bbox);
			make_alpha_mdev(dev, &pmdev, bbox, 32);
			code = (*dev_proc(pmdev, open_device))((gx_device *)pmdev);
			code = (*dev_proc(pmdev, fill_rectangle))((gx_device *)pmdev, 0, 0, 
				pmdev->width, pmdev->height, 0xffffffff);


			code = gx_dc_pattern2_get_bbox(pdcolor, &bbox1);
			if (code)
				rect_intersect(bbox, bbox1);


			int sx = fixed2int(bbox.p.x);
			int sy = fixed2int(bbox.p.y);
			gs_int_point rect_size;
			rect_size.x = fixed2int(bbox.q.x + fixed_half) - sx;
			rect_size.y = fixed2int(bbox.q.y + fixed_half) - sy;
			if (rect_size.x == 0 || rect_size.y == 0) return 0;

			// Adjust for offset of the mattern
			m.tx = -pmdev->mapped_x;
			m.ty = -pmdev->mapped_y;
			gs_matrix_multiply(&pis_noconst->ctm, &m, &pis_noconst->ctm);

			// Use non-fixed tx and ty since we adjusted it
			pis_noconst->ctm.tx_fixed = 0;
			pis_noconst->ctm.ty_fixed = 0;
			pis_noconst->ctm.txy_fixed_valid = false;

			code = gs_shading_do_fill_rectangle(pi.templat.Shading,
				NULL, (gx_device *)pmdev, (gs_imager_state *)pis, !pi.shfill);

			pis_noconst->ctm = oldCTM;

			svg->current_image_clip_path = pcpath;

			/* Restore the paths to their original locations. Maybe not needed */
			make_png_from_mdev(pmdev, fixed2float(bbox.p.x), fixed2float(bbox.p.y));
			code = (*dev_proc(pmdev, close_device))((gx_device *)pmdev);

			close_clip_groups(svg);
			svg_write(svg, "</g> <!-- pathfillimage -->\n");

			gs_setmatrix((gs_state *)pis, &save_ctm);
			gs_state_free(pgs);
		}
		return code;
	}
}

/* write a length-limited char buffer */
static int
svg_write_bytes(gx_device_svg *svg, const char *string, uint length)
{
	/* calling the accessor ensures beginpage is called */
	stream *s = gdev_vector_stream((gx_device_vector*)svg);
	uint used;

	sputs(s, (const byte *)string, length, &used);

	return !(length == used);
}

/* write a null terminated string */
static int
svg_write(gx_device_svg *svg, const char *string)
{
	return svg_write_bytes(svg, string, strlen(string));
}

static int
svg_write_header(gx_device_svg *svg)
{
	/* we're called from beginpage, so we can't use
	   svg_write() which calls gdev_vector_stream()
	   which calls beginpage! */
	stream *s = svg->strm;
	uint used;
	char line[300];

	if_debug0m('_', svg->memory, "svg_write_header\n");

	/* only write the header once */
	if (svg->header)
		return 1;

	/* write the initial boilerplate */
	gs_sprintf(line, "%s\n", XML_DECL);
	/* svg_write(svg, line); */
	sputs(s, (byte *)line, strlen(line), &used);

	gs_sprintf(line, "<svg xmlns='%s' version='%s' xmlns:xlink='http://www.w3.org/1999/xlink'",
		SVG_XMLNS, SVG_VERSION);
	/* svg_write(svg, line); */
	sputs(s, (byte *)line, strlen(line), &used);
	gs_sprintf(line, "\n\twidth='%.3fin' height='%.3fin' viewBox='0 0 %d %d'>\n",
		(double)svg->MediaSize[0] / 72.0, (double)svg->MediaSize[1] / 72.0,
		(int)svg->MediaSize[0], (int)svg->MediaSize[1]);
	sputs(s, (byte *)line, strlen(line), &used);

	/*
	* TODO: Add definitions into the defs tag. This has not been done because
	* it is not possible to insert data into the middle of a stream. Perhaps
	* there simply is no way to do this and thus no way to create a truely
	* properly formed SVG file.
	*/

	gs_sprintf(line, "<defs>\n");
	sputs(s, (byte *)line, strlen(line), &used);

	gs_sprintf(line, "<clipPath id='clip0'>\n");
	sputs(s, (byte *)line, strlen(line), &used);

	gs_sprintf(line, "<rect x='%d' y='%d' width='%d' height='%d' stroke='none' fill='none'/>\n",
		0, 0, (int)svg->MediaSize[0], (int)svg->MediaSize[1]);
	sputs(s, (byte *)line, strlen(line), &used);

	gs_sprintf(line, "</clipPath>\n");
	sputs(s, (byte *)line, strlen(line), &used);



	gs_sprintf(line, "<!-- Move all clipPaths into here for a properly formatted svg file -->\n");
	sputs(s, (byte *)line, strlen(line), &used);
	gs_sprintf(line, "</defs>\n");
	sputs(s, (byte *)line, strlen(line), &used);

	/* Enable multipule page output*/
	gs_sprintf(line, "<pageSet>\n");
	sputs(s, (byte *)line, strlen(line), &used);



	/* mark that we've been called */
	svg->header = 1;

	return 0;
}
static enum COLOR_TYPE svg_get_color_type(gx_device_svg * svg, const gx_drawing_color *pdc)
{
	const gx_device_color *pdevc = (gx_device_color *)pdc;
	if (gx_dc_is_pure(pdc))
	{
		return COLOR_PURE;
	}
	else if (gx_dc_is_null(pdc))
	{
		// We are null
		return COLOR_NULL;
	}
	else if (gx_dc_is_devn(pdc))
	{
		// We are devn
		return COLOR_DEV;
	}
	else if (gx_dc_is_binary_halftone(pdc))
	{
		// Binary halftone
		return COLOR_BINARY_HT;
	}
	else if (gx_dc_is_colored_halftone(pdc))
	{
		// Colored halftone
		return COLOR_COLOR_HT;
	}
	else if (gx_dc_is_pattern2_color(pdevc))
	{
		return COLOR_PATTERN2;
	}
	else if (gx_dc_is_pattern1_color(pdevc) ||
		gx_dc_is_pattern1_color_clist_based(pdevc) ||
		gx_dc_is_pattern1_color_with_trans(pdevc))
	{
		return COLOR_PATTERN1;
	}
	return COLOR_UNKNOWN;

}
static gx_color_index
svg_get_color(gx_device_svg *svg, const gx_drawing_color *pdc)
{
	gx_color_index color = gx_no_color_index;

	if (gx_dc_is_pure(pdc))
		color = gx_dc_pure_color(pdc);
	return color;
}

static bool pen_is_empty(gx_device_svg *svg, const gx_path_type_t type)
{
	return (!(type & gx_path_type_stroke) && (svg->strokecolor != gx_no_color_index)) 
		|| (type & gx_path_type_clip);
}

static bool fill_is_empty(gx_device_svg *svg, const gx_path_type_t type)
{
	return (!(type & gx_path_type_fill) && (svg->fillcolor != gx_no_color_index)) 
		|| (type & gx_path_type_clip);
}

static void svg_write_state_to_svg(gx_device_svg *svg, const bool emptyPen, const bool emptyFill)
{
	char line[SVG_LINESIZE];

	/* write out the new current state */
	if ((svg->strokecolor != gx_no_color_index) && !emptyPen)
	{
		gs_sprintf(line, "stroke='#%06x' ", (uint)(svg->strokecolor & 0xffffffL));
		svg_write(svg, line);
	}
	else 
	{
		svg_write(svg, "stroke='none' ");
	}

	if ((svg->fillcolor != gx_no_color_index) && !emptyFill)
	{
		gs_sprintf(line, "fill='#%06x' ", (uint)(svg->fillcolor & 0xffffffL));
		svg_write(svg, line);
	}
	else 
	{
		svg_write(svg, "fill='none' ");
	}

	if (svg->linewidth != 1.0) 
	{
		gs_sprintf(line, "stroke-width='%lf' ", svg->linewidth);
		svg_write(svg, line);
	}

	if (svg->linecap != SVG_DEFAULT_LINECAP) 
	{
		switch (svg->linecap) {
		case gs_cap_round:
			svg_write(svg, "stroke-linecap='round' ");
			break;
		case gs_cap_square:
			svg_write(svg, "stroke-linecap='square' ");
			break;
		case gs_cap_butt:
		default:
			/* treat all the other options as the default */
			svg_write(svg, "stroke-linecap='butt' ");
			break;
		}
	}

	if (svg->linejoin != SVG_DEFAULT_LINEJOIN) 
	{
		switch (svg->linejoin) {
		case gs_join_round:
			svg_write(svg, "stroke-linejoin='round' ");
			break;
		case gs_join_bevel:
			svg_write(svg, "stroke-linejoin='bevel' ");
			break;
		case gs_join_miter:
		default:
			/* SVG doesn't support any other variants */
			svg_write(svg, "stroke-linejoin='miter' ");
			break;
		}
	}

	if (svg->miterlimit != SVG_DEFAULT_MITERLIMIT) 
	{
		gs_sprintf(line, "stroke-miterlimit='%lf' ", svg->miterlimit);
		svg_write(svg, line);
	}
}

static int
svg_write_state(gx_device_svg *svg)
{
	/* has anything changed? */
	if (!svg->dirty)
	{
		return 0;
	}

	/* close the current graphics state element, if any */
	if (svg->mark > 1) 
	{
		svg_write(svg, "</g>\n");
		svg->mark--;
	}

	svg->dirty = 0;
	return 0;
}

/* vector device implementation */

/* Page management */
static int
svg_beginpage(gx_device_vector *vdev)
{
	gx_device_svg *svg = (gx_device_svg *)vdev;
	uint used;
	char line[300];

	const byte * pg = (byte*)"<page clip-path='url(#clip0)'>\n";
	if_debug1m('_', svg->memory, "svg_beginpage (page count %d)\n", svg->page_count);

	svg_write_header(svg);
	/* we may be called from beginpage, so we can't use
	svg_write() which calls gdev_vector_stream()
	which calls beginpage! */
	sputs(svg->strm, pg, strlen(pg), &used);
	/* Scale drawing so our coordinates are in pixels */
	gs_sprintf(line, "<g transform='scale(%lf,%lf)'>\n",
		72.0 / svg->HWResolution[0],
		72.0 / svg->HWResolution[1]);
	/* svg_write(svg, line); */
	sputs(svg->strm, (byte *)line, strlen(line), &used);
	svg->mark++;

	return 0;
}

/* Imager state */
static int
svg_setlinewidth(gx_device_vector *vdev, double width)
{
	gx_device_svg *svg = (gx_device_svg *)vdev;

	if_debug1m('_', svg->memory, "svg_setlinewidth(%lf)\n", width);

	svg->linewidth = width;
	svg->dirty++;

	return 0;
}
static int
svg_setlinecap(gx_device_vector *vdev, gs_line_cap cap)
{
	gx_device_svg *svg = (gx_device_svg *)vdev;
	const char *linecap_names[] = { "butt", "round", "square",
		"triangle", "unknown" };

	if (cap < 0 || cap > gs_cap_unknown)
		return gs_throw_code(gs_error_rangecheck);
	if_debug1m('_', svg->memory, "svg_setlinecap(%s)\n", linecap_names[cap]);

	svg->linecap = cap;
	svg->dirty++;

	return 0;
}
static int
svg_setlinejoin(gx_device_vector *vdev, gs_line_join join)
{
	gx_device_svg *svg = (gx_device_svg *)vdev;
	const char *linejoin_names[] = { "miter", "round", "bevel",
		"none", "triangle", "unknown" };

	if (join < 0 || join > gs_join_unknown)
		return gs_throw_code(gs_error_rangecheck);
	if_debug1m('_', svg->memory, "svg_setlinejoin(%s)\n", linejoin_names[join]);

	svg->linejoin = join;
	svg->dirty++;

	return 0;
}
static int
svg_setmiterlimit(gx_device_vector *vdev, double limit)
{
	if_debug1m('_', vdev->memory, "svg_setmiterlimit(%lf)\n", limit);
	return 0;
}
static int
svg_setdash(gx_device_vector *vdev, const float *pattern,
uint count, double offset)
{
	if_debug0m('_', vdev->memory, "svg_setdash\n");
	return 0;
}
static int
svg_setlogop(gx_device_vector *vdev, gs_logical_operation_t lop,
gs_logical_operation_t diff)
{
	if_debug2m('_', vdev->memory, "svg_setlogop(%u,%u) set logical operation\n",
		lop, diff);
	/* SVG can fake some simpler modes, but we ignore this for now. */
	return 0;
}

/* Other state */

static int
svg_can_handle_hl_color(gx_device_vector *vdev, const gs_imager_state *pis,
const gx_drawing_color * pdc)
{
	if_debug0m('_', vdev->memory, "svg_can_handle_hl_color\n");
	return 0;
}

static int
svg_setfillcolor(gx_device_vector *vdev, const gs_imager_state *pis,
const gx_drawing_color *pdc)
{
	gx_device_svg *svg = (gx_device_svg*)vdev;
	const gx_device_color *pdevc = (gx_device_color *)pdc;
	gx_color_index color = gx_no_color_index;
	if (gx_dc_is_pure(pdc))
		color = gx_dc_pure_color(pdc);
	else if (gx_dc_is_null(pdc))
	{
		// We are null
		color++;
	}
	else if (gx_dc_is_devn(pdc))
	{
		// We are devn
		color++;
	}
	else if (gx_dc_is_binary_halftone(pdc))
	{
		// Binary halftone
		color++;
	}
	else if (gx_dc_is_colored_halftone(pdc))
	{
		// Colored halftone
		color++;
	}
	else if (!color_is_set(pdc))
	{
		// Other color
		color++;
	}
	else if (gx_dc_is_pattern2_color(pdevc))
	{
		color--;
	}
	else if (gx_dc_is_pattern1_color(pdevc) ||
		gx_dc_is_pattern1_color_clist_based(pdevc) ||
		gx_dc_is_pattern1_color_with_trans(pdevc))
	{
		color++;
	}
	else
	{
		color++;
	}

	if_debug0m('_', svg->memory, "svg_setfillcolor\n");

	if (svg->fillcolor == color)
		return 0; /* not a new color */
	/* update our state with the new color */
	svg->fillcolor = color;
	/* request a new group element */
	svg->dirty++;
	return 0;
}

static int
svg_setstrokecolor(gx_device_vector *vdev, const gs_imager_state *pis,
const gx_drawing_color *pdc)
{
	gx_device_svg *svg = (gx_device_svg*)vdev;
	gx_color_index stroke = svg_get_color(svg, pdc);

	if_debug0m('_', svg->memory, "svg_setstrokecolor\n");

	if (svg->strokecolor == stroke)
		return 0; /* not a new color */

	/* update our state with the new color */
	svg->strokecolor = stroke;
	/* request a new group element */
	svg->dirty++;

	return 0;
}

/* Paths */
/*    gdev_vector_dopath */

static int svg_print_path_type(gx_device_svg *svg, gx_path_type_t type)
{
	const char *path_type_names[] = { "winding number", "fill", "stroke",
		"fill and stroke", "clip" };

	if (type <= 4)
		if_debug2m('_', svg->memory, "type %d (%s)", type, path_type_names[type]);
	else
		if_debug1m('_', svg->memory, "type %d", type);

	return 0;
}

static int svg_write_clip_start(gx_device_svg *svg, gs_matrix matrix)
{
	char clippathStr[200];

	gs_sprintf(clippathStr, "<clipPath id='clip%i' transform='matrix(%f,%f,%f,%f,%f,%f)'>\n",
		++svg->highestUsedId,
		matrix.xx, matrix.xy, matrix.yx, matrix.yy, matrix.tx, matrix.ty
		);
	svg_write(svg, clippathStr);

	return 0;
}

static int svg_write_clip_end(gx_device_svg *svg)
{
	svg_write(svg, "</clipPath>\n");

	return 0;
}

static int
svg_writeclip(gx_device_svg *svg, gx_clip_path *pcpath, gs_matrix matrix)
{
	gs_fixed_rect bbox;
	float bboxArea;
	float minBboxArea = MAX_FLOAT;
	int code = 0, mainBboxCode;
	gx_cpath_path_list *path_list;
	int clipPathIndex = -1, index = 0;

	if (pcpath == NULL)
	{
		svg->validClipPath = false;
		return 0;
	}

	// Count how many paths there are
	int path_list_size = 0;
	if (pcpath->path_list != NULL)
	{
		path_list = pcpath->path_list;
		do
		{
			code = gx_path_bbox(&path_list->path, &bbox);
			if (code >= 0)
			{
				bboxArea =
					abs(fixed2float(bbox.q.x - bbox.p.x)) *
					abs(fixed2float(bbox.q.y - bbox.p.y));

				if (bboxArea < minBboxArea)
				{
					minBboxArea = bboxArea;
				}

				++path_list_size;
			}
		} while ((path_list = path_list->next));
	}
	++path_list_size; // Add once for the path contained in pcpath->path

	// Fill in an array of path pointers, with the 1st being the main path
	gx_path **all_paths = (gx_path**)malloc(path_list_size * sizeof(gx_path*));
	svg->usedIds = svg->highestUsedId + 1;
	int original_clip_path_id = svg->usedIds;
	all_paths[0] = &pcpath->path;
	if (pcpath->path_list != NULL)
	{
		int i = 0;
		path_list = pcpath->path_list;
		do
		{
			all_paths[i + 1] = &path_list->path;
			++i;
		} while ((path_list = path_list->next));
	}
	
	// Write each clip path
	svg->validClipPath = false;
	int null_path_count = 0;
	for (int i = 0; i < path_list_size; ++i)
	{
		gx_path *path = all_paths[i];

		if (path->subpath_count <= 0)
		{
			// No subpaths means that it is an empty path
			// (even though it might have a non-empty bounding area)
			all_paths[i] = NULL;
			++null_path_count;
			continue;
		}

		// Double check the bounds
		code = gx_path_bbox(path, &bbox);
		if (code < 0)
		{
			// Couldn't get the bounds, therefore not a path that we want to use
			all_paths[i] = NULL;
			++null_path_count;
			continue;
		}

		// Write the path
		svg_write_clip_start(svg, matrix);
		gdev_vector_dopath(svg, path, gx_path_type_stroke, NULL);
		svg_write_clip_end(svg);

		svg->validClipPath = true;
	}

	if (!svg->validClipPath)
	{
		svg->start_clip_mark = -1;

		--svg->usedIds; // Return to original value
		svg->highestUsedId = svg->usedIds;
		return 1;
	}
	else
	{
		svg->start_clip_mark = svg->mark;
	}

	// begin groups when there is more than one clip path
	for (int i = 1; i < path_list_size - null_path_count; ++i)
	{
		++svg->mark;

		char line[SVG_LINESIZE];
		gs_sprintf(line, "<g clip-path='url(#clip%i)'>\n", original_clip_path_id + i);
		svg_write(svg, line);
	}

	return 0;

}

/*
Because svg_writeclip(...) may nest the current item into more groups, we must
close thos additional groups once the item has been written to the output
stream
*/
static void close_clip_groups(gx_device_svg *svg)
{
	if (svg->start_clip_mark >= 0)
	{
		close_groups(svg, svg->start_clip_mark);
		svg->start_clip_mark = -1;
	}
}

static int
svg_dorect(gx_device_vector *vdev, fixed x0, fixed y0,
fixed x1, fixed y1, gx_path_type_t type)
{
	gx_device_svg *svg = (gx_device_svg *)vdev;
	char line[300];

	// Check if the pen or brush is empty
	const bool emptyPen = pen_is_empty(svg, type);
	const bool emptyBrush = fill_is_empty(svg, type);

	if_debug0m('_', svg->memory, "svg_dorect");
	svg_print_path_type(svg, type);
	if_debug0m('_', svg->memory, "\n");

	svg_write_state(svg);

	if ((svg->current_clip_path != NULL) && !svg->writing_clip)
	{
		svg->writing_clip = true;

		gs_matrix mIdent;
		gs_make_identity(&mIdent);
		svg_writeclip(svg, svg->current_clip_path, mIdent);

		char clip_path_id[SVG_LINESIZE];
		gs_sprintf(clip_path_id, "clip-path='url(#clip%i)' ", svg->usedIds);

		svg_write(svg, "<rect ");

		svg_write_state_to_svg(svg, emptyPen, emptyBrush);

		gs_sprintf(line, "%sx='%lf' y='%lf' width='%lf' height='%lf'", 
			(svg->validClipPath ? clip_path_id : ""),
			fixed2float(x0), fixed2float(y0),
			fixed2float(x1 - x0), fixed2float(y1 - y0));
		svg_write(svg, line);

		svg->writing_clip = false;
	}
	else
	{
		svg_write(svg, "<rect ");

		gs_sprintf(line, "x='%lf' y='%lf' width='%lf' height='%lf'", 
			fixed2float(x0), fixed2float(y0),
			fixed2float(x1 - x0), fixed2float(y1 - y0));
		svg_write(svg, line);
	}

	if ((svg->current_clip_path == NULL) || svg->writing_clip)
	{
		/* override the inherited stroke attribute if we're not stroking */
		if (emptyPen)
		{
			svg_write(svg, " stroke='none'");
		}
		/* override the inherited fill attribute if we're not filling */
		if (emptyBrush)
		{
			svg_write(svg, " fill='none'");
		}
	}

	svg_write(svg, "/>\n");

	return 0;
}

static int
svg_beginpath(gx_device_vector *vdev, gx_path_type_t type)
{
	gx_device_svg *svg = (gx_device_svg *)vdev;

	/* skip non-drawing paths for now */
	if (!(type & gx_path_type_fill) && !(type & gx_path_type_stroke))
	{
		return 0;
	}

	if_debug0m('_', svg->memory, "svg_beginpath ");
	svg_print_path_type(svg, type);
	if_debug0m('_', svg->memory, "\n");

	svg_write_state(svg);

	char path_fill_rule[SVG_LINESIZE];
	gx_path_type_t rule = type & gx_path_type_rule;
	gs_sprintf(path_fill_rule, "%s", 
		rule == gx_path_type_even_odd ?
		"fill-rule='evenodd' " : "fill-rule='nonzero' ");

	if ((svg->current_clip_path != NULL) && !svg->writing_clip)
	{
		svg->writing_clip = true;
		
		gs_matrix mIdent;
		gs_make_identity(&mIdent);
		svg_writeclip(svg, svg->current_clip_path, mIdent);

		char clip_path_id[SVG_LINESIZE];
		gs_sprintf(clip_path_id, "clip-path='url(#clip%i)' ", svg->usedIds);

		svg_write(svg, "<path ");

		const bool emptyPen = pen_is_empty(svg, type);
		const bool emptyBrush = fill_is_empty(svg, type);

		svg_write_state_to_svg(svg, emptyPen, emptyBrush);

		char line[SVG_LINESIZE * 2];
		gs_sprintf(line, "%s%sd='", path_fill_rule, svg->validClipPath ? clip_path_id : "");
		svg_write(svg, line);

		svg->writing_clip = false;
	}
	else
	{
		svg_write(svg, "<path ");

		char line[SVG_LINESIZE * 2];
		gs_sprintf(line, "%sd='", path_fill_rule);
		svg_write(svg, line);
	}

	return 0;
}

static int
svg_moveto(gx_device_vector *vdev, double x0, double y0,
double x, double y, gx_path_type_t type)
{
	gx_device_svg *svg = (gx_device_svg *)vdev;
	char line[SVG_LINESIZE];

	/* skip non-drawing paths for now */
	if (!(type & gx_path_type_fill) && !(type & gx_path_type_stroke))
		return 0;

	if_debug4m('_', svg->memory, "svg_moveto(%lf,%lf,%lf,%lf) ", x0, y0, x, y);
	svg_print_path_type(svg, type);
	if_debug0m('_', svg->memory, "\n");

	gs_sprintf(line, " M%lf,%lf", x, y);
	svg_write(svg, line);

	return 0;
}

static int
svg_lineto(gx_device_vector *vdev, double x0, double y0,
double x, double y, gx_path_type_t type)
{
	gx_device_svg *svg = (gx_device_svg *)vdev;
	char line[SVG_LINESIZE];

	/* skip non-drawing paths for now */
	if (!(type & gx_path_type_fill) && !(type & gx_path_type_stroke))
		return 0;

	if_debug4m('_', svg->memory, "svg_lineto(%lf,%lf,%lf,%lf) ", x0, y0, x, y);
	svg_print_path_type(svg, type);
	if_debug0m('_', svg->memory, "\n");

	gs_sprintf(line, " L%lf,%lf", x, y);
	svg_write(svg, line);

	return 0;
}

static int
svg_curveto(gx_device_vector *vdev, double x0, double y0,
double x1, double y1, double x2, double y2,
double x3, double y3, gx_path_type_t type)
{
	gx_device_svg *svg = (gx_device_svg *)vdev;
	char line[SVG_LINESIZE];

	/* skip non-drawing paths for now */
	if (!(type & gx_path_type_fill) && !(type & gx_path_type_stroke))
		return 0;

	if_debug8m('_', svg->memory, "svg_curveto(%lf,%lf, %lf,%lf, %lf,%lf, %lf,%lf) ",
		x0, y0, x1, y1, x2, y2, x3, y3);
	svg_print_path_type(svg, type);
	if_debug0m('_', svg->memory, "\n");

	gs_sprintf(line, " C%lf,%lf %lf,%lf %lf,%lf", x1, y1, x2, y2, x3, y3);
	svg_write(svg, line);

	return 0;
}

static int
svg_closepath(gx_device_vector *vdev, double x, double y,
double x_start, double y_start, gx_path_type_t type)
{
	gx_device_svg *svg = (gx_device_svg *)vdev;

	/* skip non-drawing paths for now */
	if (!(type & gx_path_type_fill) && !(type & gx_path_type_stroke))
		return 0;

	if_debug0m('_', svg->memory, "svg_closepath ");
	svg_print_path_type(svg, type);
	if_debug0m('_', svg->memory, "\n");

	svg_write(svg, " z");

	return 0;
}

static int
svg_endpath(gx_device_vector *vdev, gx_path_type_t type)
{
	gx_device_svg *svg = (gx_device_svg *)vdev;

	/* skip non-drawing paths for now */
	if (!(type & gx_path_type_fill) && !(type & gx_path_type_stroke))
	{
		return 0;
	}

	if_debug0m('_', svg->memory, "svg_endpath ");
	svg_print_path_type(svg, type);
	if_debug0m('_', svg->memory, "\n");

	/* close the path data attribute */
	svg_write(svg, "'");

	if ((svg->current_clip_path == NULL) || svg->writing_clip)
	{
		// Check if the pen or brush is empty
		const bool emptyPen = pen_is_empty(svg, type);
		const bool emptyBrush = fill_is_empty(svg, type);

		/* override the inherited stroke attribute if we're not stroking */
		if (emptyPen)
		{
			svg_write(svg, " stroke='none'");
		}

		/* override the inherited fill attribute if we're not filling */
		if (emptyBrush)
		{
			svg_write(svg, " fill='none'");
		}
	}

	svg_write(svg, "/>\n");

	if ((svg->current_clip_path != NULL) && !svg->writing_clip)
	{
		close_clip_groups(svg);
	}

	return 0;
}

gs_private_st_suffix_add0(st_svg_image_enum, svg_image_enum_t,
	"svg_image_enum_t", svg_image_enum_enum_ptrs,
	svg_image_enum_reloc_ptrs,
	st_gx_image_enum_common);
static int
svg_plane_data(gx_image_enum_common_t * info,
const gx_image_plane_t * planes, int height,
int *rows_used)
{
	svg_image_enum_t *pie = (svg_image_enum_t *)info;
	int i, y, k, raster, ind, bytes;
	unsigned char rgba[4];
	unsigned char cmyk[4];
	double black;
	byte *row;
	byte *invRow;
	raster = pie->num_planes * pie->plane_widths[0] * pie->plane_depths[0] / 8;
	row = gs_alloc_bytes(pie->memory, raster, "png raster buffer");
	if (row == 0)
		return gs_note_error(gs_error_VMerror);
	// Clear memory
	memset(row, 0xff, raster);


	// Get the planes copied
	for (y = 0; y < height; y++)
	{
		if (pie->num_planes == 1 && (pie->plane_depths[0] == 24 || pie->plane_depths[0] == 8))
		{
			memcpy(row, planes[0].data + planes[0].data_x + (y * raster), raster);
		}
		else
		{
			for (i = 0; i < pie->plane_widths[0]; i++) {
				for (k = 0; k < pie->num_planes; ++k) {

					if (planes[k].data && pie->num_planes < 4)
					{
						// This hasn't been tested yet
						bytes = pie->plane_depths[k] / 8;
						ind = i * pie->num_planes * bytes + k + (raster * y);

						memcpy(&row[ind], &planes[k].data[i * bytes], bytes);

						if (pie->num_planes == 1 && pie->plane_depths[k] == 32)
						{
							invRow = &row[ind];
							*(invRow + 0) ^= 0xff;
							*(invRow + 1) ^= 0xff;
							*(invRow + 2) ^= 0xff;
							*(invRow + 3) ^= 0xff;
						}
					}
					else if (pie->num_planes == 4)
					{
						//
						cmyk[k] = planes[k].data[i];
					}
					else
					{
						dmprintf(info->memory, " no data or too many planes");
						memset(&row[i + k], 0xff, pie->plane_depths[k] / 8);
					}
				}
				if (pie->num_planes == 4)
				{
					// CMYK to RGBA
					black = (1.0 - cmyk[3] / 255.0);
					/*R*/ row[i * 4 + 0] = (unsigned char)(255 * (1.0 - cmyk[0] / 255.0)*black);
					/*G*/ row[i * 4 + 1] = (unsigned char)(255 * (1.0 - cmyk[1] / 255.0)*black);
					/*B*/ row[i * 4 + 2] = (unsigned char)(255 * (1.0 - cmyk[2] / 255.0)*black);
					/*A*/ row[i * 4 + 3] = 0; // Alpha is inverted, 0 -> Full Opacity
				}
			}
		}
		png_write_rows(pie->png_ptr, &row, 1);
	}

	*rows_used = height;
	gs_free_object(pie->memory, row, "png raster buffer");
	return (pie->rows_left -= height) <= 0;
}


static int write_base64_png(gx_device* dev,
struct mem_encode *state,
	gs_matrix_fixed ctm,
	gs_matrix ImageMatrix,
	uint width,
	uint height)
{
	char line[SVG_LINESIZE];
	size_t outputSize = 0;
	byte *buffer = 0;
	float tx, ty;
	/* Flush the buffer as base 64 */
	buffer = base64_encode(state->memory, state->buffer, state->size, &outputSize);

	tx = (ImageMatrix.yy) > 0 ? 0 : ctm.yx;
	ty = (ImageMatrix.yy) > 0 ? 0 : ctm.yy;

	gs_matrix m3;
	m3.xx = ctm.xx / width * ImageMatrix.xx / width;
	m3.xy = ctm.xy / width * ImageMatrix.xx / width;
	m3.yx = ctm.yx / height * ImageMatrix.yy / height;
	m3.yy = ctm.yy / height * ImageMatrix.yy / height;
	m3.tx = ctm.tx + tx;
	m3.ty = ctm.ty + ty;

	gs_sprintf(line, "<g transform='matrix(%f,%f,%f,%f,%f,%f)'>",
		m3.xx, m3.xy, m3.yx, m3.yy, m3.tx, m3.ty
		);
	svg_write(dev, line);

	gx_device_svg *svg = (gx_device_svg *)dev;

	// Calculate inverse matrix for cliping path
	gs_matrix mInvert;
	gs_matrix_invert(&m3, &mInvert);

	if (svg->current_image_clip_path != NULL)
	{
		svg_writeclip(svg, svg->current_image_clip_path, mInvert);
		svg->current_image_clip_path = NULL;
	}

	char clip_path_id[SVG_LINESIZE];
	gs_sprintf(clip_path_id, "clip-path='url(#clip%i)' ", svg->usedIds);

	gs_sprintf(line, "<image %swidth='%d' height='%d' xlink:href=\"data:image/png;base64,",
		svg->validClipPath ? clip_path_id : "", width, height);
	svg_write(dev, line);
	svg_write_bytes(dev, buffer, outputSize);
	svg_write(dev, "\"/>");

	close_clip_groups(svg);
	
	svg_write(dev, "</g>");
	gs_free_object(state->memory, buffer, "base64 buffer");

	return 0;
}

static int
svg_end_image(gx_image_enum_common_t * info, bool draw_last)
{
	svg_image_enum_t *pie = (svg_image_enum_t *)info;
	char line[SVG_LINESIZE];
	size_t outputSize = 0;
	byte *buffer = 0;
	float ty;

	// Finalize the image
	png_write_end(pie->png_ptr, pie->info_ptr);

	/* Do some cleanup */
	if (pie->png_ptr && pie->info_ptr)
		png_destroy_write_struct(&pie->png_ptr, &pie->info_ptr);

	/* Flush the buffer as base 64 */
	write_base64_png(info->dev, &pie->state, pie->ctm, pie->ImageMatrix, 
		pie->width, pie->height);

	gs_free_object(pie->memory, pie->state.buffer, "png img buf");
	//gs_free_object(pie->memory, buffer, "base64 buffer");

	gx_image_free_enum(&info);
	return 0;
}

static const gx_image_enum_procs_t svg_image_enum_procs = {
	svg_plane_data,
	svg_end_image
};

static int
svg_begin_image(gx_device * dev,
const gs_imager_state * pis, const gs_image_t * pim,
gs_image_format_t format, const gs_int_rect * prect,
const gx_drawing_color * pdcolor, const gx_clip_path * pcpath,
gs_memory_t * memory, gx_image_enum_common_t ** pinfo)
{
	return 0;
}

static int
svg_begin_typed_image(gx_device * dev, const gs_imager_state * pis,
const gs_matrix * pmat,
const gs_image_common_t * pim,
const gs_int_rect * prect,
const gx_drawing_color * pdcolor,
const gx_clip_path * pcpath,
gs_memory_t * memory,
gx_image_enum_common_t ** pinfo)
{
	gx_device_svg *svg = (gx_device_svg *)dev;
	svg_image_enum_t *pie;

	const gs_pixel_image_t *ppi = (const gs_pixel_image_t *)pim;
	int ncomp;
	int code;			/* return code */

	dmprintf7(dev->memory, "begin_typed_image(type=%d, ImageMatrix=[%g %g %g %g %g %g]\n",
		pim->type->index, 
		pim->ImageMatrix.xx, pim->ImageMatrix.xy,
		pim->ImageMatrix.yx, pim->ImageMatrix.yy,
		pim->ImageMatrix.tx, pim->ImageMatrix.ty);

	switch (pim->type->index) {
	case 1:
		if (((const gs_image1_t *)ppi)->ImageMask) {
			ncomp = 1;
			break;
		}
		/* falls through */
	case 4:
		ncomp = gs_color_space_num_components(ppi->ColorSpace);
		break;
	case 3:
		ncomp = gs_color_space_num_components(ppi->ColorSpace) + 1;
		break;
	case 2:			/* no data */
		dmputs(dev->memory, ")\n");
		return 1;
	default:
		goto dflt;
	}
	pie = gs_alloc_struct(memory, svg_image_enum_t, &st_svg_image_enum,
		"svg_begin_typed_image");
	if (pie == 0)
		goto dflt;
	memset(pie, 0, sizeof(*pie)); /* cleanup entirely for GC to work in all cases. */
	*pinfo = (gx_image_enum_common_t *)pie;
	if (gx_image_enum_common_init(*pinfo,
		(const gs_data_image_t *)pim,
		&svg_image_enum_procs, dev, ncomp,
		ppi->format) < 0
		)
		goto dflt;
	dmprintf4(dev->memory, "\n    Width=%d, Height=%d, BPC=%d, num_components=%d)\n",
		ppi->Width, ppi->Height, ppi->BitsPerComponent, ncomp);

	/* This is where we add in the code to set up the PNG data copying */
	pie->state.buffer = NULL;
	pie->state.size = 0;
	pie->state.memory = /*dev->*/memory;
	pie->width = ppi->Width;
	pie->height = ppi->Height;
	pie->ctm = pis->ctm;
	pie->ImageMatrix = pim->ImageMatrix;
	/* Initialize PNG structures */
	pie->png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	pie->info_ptr = png_create_info_struct(pie->png_ptr);
	pie->invert = false;

	if (pie->png_ptr == 0 || pie->info_ptr == 0) {
		code = gs_note_error(gs_error_VMerror);
		goto done;
	}
	code = setjmp(png_jmpbuf(pie->png_ptr));
	if (code) {
		/* If we get here, we had a problem reading the file */
		code = gs_note_error(gs_error_VMerror);
		goto done;
	}
	code = 0;			/* for normal path */

	png_set_write_fn(pie->png_ptr, &pie->state, my_png_write_data, my_png_flush);
	code = setup_png(dev, pie, ppi->ColorSpace);

	if (code) {
		goto done;
	}

	svg->current_image_clip_path = pcpath;

	pie->memory = memory;
	pie->rows_left = ppi->Height;
	*pinfo = (gx_image_enum_common_t *)pie;
	return 0;
dflt:
	dmputs(dev->memory, ") DEFAULTED\n");
	return gx_default_begin_typed_image(dev, pis, pmat, pim, prect, pdcolor,
		pcpath, memory, pinfo);
done:
	/* free the structures */
	png_destroy_write_struct(&pie->png_ptr, &pie->info_ptr);
	pie->png_ptr = NULL;
	pie->info_ptr = NULL;
	return code;
}

static
int svg_fillpage(gx_device *dev,
gs_imager_state * pis,
gx_device_color *pdevc)
{
	gx_device_svg *const svg = (gx_device_svg*)dev;

	if_debug0m('_', svg->memory, "svg_fillpage\n");

	// Do nothing in this function to prevent white background rects from appearing

	return 0;
}

/* PNG writing algorithems*/

void
my_png_write_data(png_structp png_ptr, png_bytep data, png_size_t length)
{
	/* with libpng15 next line causes pointer deference error; use libpng12 */
	struct mem_encode* p = (struct mem_encode*)png_get_io_ptr(png_ptr); /* was png_ptr->io_ptr */
	size_t nsize = p->size + length;
	char *buffer = 0;

	/* allocate or grow buffer */
	if (p->buffer)
		buffer = gs_resize_object(p->memory, p->buffer, nsize, "png img buf");
	/*p->buffer = realloc(p->buffer, nsize);*/
	else
		buffer = gs_alloc_bytes(p->memory, nsize, "png img buf");
	/*p->buffer = malloc(nsize);*/

	if (!buffer)
	{
		gs_free_object(p->memory, p->buffer, "png img buf");
		png_error(png_ptr, "Write Error");
	}
	/* Validated buffer so we can use it */
	p->buffer = buffer;
	/* copy new bytes to end of buffer */
	memcpy(p->buffer + p->size, data, length);
	p->size += length;
}

/* This is optional but included to show how png_set_write_fn() is called */
void
my_png_flush(png_structp png_ptr)
{

}

int setup_png(gx_device * pdev, svg_image_enum_t  *pie, const gs_color_space *pcs)
{
	gs_memory_t *mem = pdev->memory;
	//int raster = gdev_prn_raster(pdev);
	//gx_downscaler_t ds;

	/* PNG structures */
	//byte *row = gs_alloc_bytes(mem, raster, "png raster buffer");
	png_struct *png_ptr = pie->png_ptr;
	//png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_info *info_ptr = pie->info_ptr;
	//png_create_info_struct(png_ptr);
	int depth = pdev->color_info.depth;
	int planeDepth = 0, i;
	int y;
	int code;			/* return code */
	char software_key[80];
	char software_text[256];
	png_text text_png;
	int dst_bpc, src_bpc;
	bool errdiff = 0;
	int factor = 1;

	bool invert = false, endian_swap = false, bg_needed = false;
	png_byte bit_depth = 0;
	png_byte color_type = 0;
	png_uint_32 x_pixels_per_unit;
	png_uint_32 y_pixels_per_unit;
	png_byte phys_unit_type;
	png_color_16 background;
	png_uint_32 width, height;
#if PNG_LIBPNG_VER_MINOR >= 5
	//png_color palette[256];
	png_color *palette;
#endif
	png_color *palettep;
	png_uint_16 num_palette;
	png_uint_32 valid = 0;

	palette = (void *)gs_alloc_bytes(mem, 256 * sizeof(png_color), "png palette");
	if (palette == 0) {
		return gs_note_error(gs_error_VMerror);
	}

	for (i = 0; i < pie->num_planes; ++i)
	{
		planeDepth += pie->plane_depths[i];
	}
	depth = planeDepth;

	/* Sanity check params */
	if (factor < 1) factor = 1;

	/* Slightly nasty, but it saves us duplicating this entire routine. */
	//if (monod) {
	//	errdiff = 1;
	//	depth = 1;
	//}

	if (/*row == 0 || */png_ptr == 0 || info_ptr == 0) {
		code = gs_note_error(gs_error_VMerror);
		return code;
		//goto done;
	}

	code = 0;			/* for normal path */

	///* set up the output control */
	//png_init_io(png_ptr, file);

	/* set the file information here */
	/* resolution is in pixels per meter vs. dpi */
	x_pixels_per_unit =
		(png_uint_32)(pdev->HWResolution[0] * (100.0 / 2.54) / factor + 0.5);
	y_pixels_per_unit =
		(png_uint_32)(pdev->HWResolution[1] * (100.0 / 2.54) / factor + 0.5);

	phys_unit_type = PNG_RESOLUTION_METER;
	valid |= PNG_INFO_pHYs;

	switch (depth) {
	case 32:
		bit_depth = 8;
		color_type = PNG_COLOR_TYPE_RGB_ALPHA;
		invert = true;

		{
			//gx_device_svg *ppdev = (gx_device_svg *)pdev;
			//background.index = 0;
			//background.red = (ppdev->background >> 16) & 0xff;
			//background.green = (ppdev->background >> 8) & 0xff;
			//background.blue = (ppdev->background) & 0xff;
			//background.gray = 0;
			//bg_needed = true;


			//gx_device_pngalpha *ppdev = (gx_device_pngalpha *)pdev;
			background.index = 0;
			background.red = 0xff /*(ppdev->background >> 16) & 0xff*/;
			background.green = 0xff /*(ppdev->background >> 8) & 0xff*/;
			background.blue = 0xff /*(ppdev->background) & 0xff*/;
			background.gray = 0;
			bg_needed = false /*true*/;
		}
		errdiff = 1;
		break;
	case 48:
		bit_depth = 16;
		color_type = PNG_COLOR_TYPE_RGB;
#if defined(ARCH_IS_BIG_ENDIAN) && (!ARCH_IS_BIG_ENDIAN)
		endian_swap = true;
#endif
		break;
	case 24:
		bit_depth = 8;
		color_type = PNG_COLOR_TYPE_RGB;
		errdiff = 1;
		break;
	case 8:
		bit_depth = 8;
		if (gx_device_has_color(pdev)) {
			color_type = PNG_COLOR_TYPE_PALETTE;
			errdiff = 0;
		}
		else {
			color_type = PNG_COLOR_TYPE_GRAY;
			errdiff = 1;
		}
		break;
	case 4:
		bit_depth = 4;
		color_type = PNG_COLOR_TYPE_PALETTE;
		break;
	case 1:
		bit_depth = 1;
		color_type = PNG_COLOR_TYPE_GRAY;
		invert = true;
		break;
	}

	/* set the palette if there is one */
	if (color_type == PNG_COLOR_TYPE_PALETTE) {
		int i;
		int num_colors = 1 << depth;
		gx_color_value rgb[3];

#if PNG_LIBPNG_VER_MINOR >= 5
		palettep = palette;
#else
		palettep =
			(void *)gs_alloc_bytes(mem, 256 * sizeof(png_color),
			"png palette");
		if (palettep == 0) {
			code = gs_note_error(gs_error_VMerror);
			goto done;
		}
#endif
		num_palette = num_colors;
		valid |= PNG_INFO_PLTE;
		for (i = 0; i < num_colors; i++) {
			(*dev_proc(pdev, map_color_rgb)) ((gx_device *)pdev,
				(gx_color_index)i, rgb);

			/*
			This crap kept me busy for 4 days straight. Needed to apply the palette
			to the image when the image is an 8-bit indexed image.
			*/
			if (depth == 8)
			{
				gs_client_color cc;
				gs_cspace_indexed_lookup(pcs, i, &cc);

				const int base_num_comp = cs_num_components(pcs->base_space);
				if (base_num_comp == 4)
				{
					// Things get a bit funky when the base color space has an alpha channel.
					// Since the indexed colors map to colors with an alpha channel and the palette
					// is in the RGB space, we have a problem.

					const double alpha = cc.paint.values[3];
					const double oneMinusAlpha = 1.0 - alpha;

					palettep[i].red = (int)(((cc.paint.values[0] * alpha + oneMinusAlpha) * 255) + 0.5); //  Assume white background
					palettep[i].green = (int)(((cc.paint.values[1] * alpha + oneMinusAlpha) * 255) + 0.5); //  Assume white background
					palettep[i].blue = (int)(((cc.paint.values[2] * alpha + oneMinusAlpha) * 255) + 0.5); //  Assume white background
				}
				else
				{
					palettep[i].red = (int)((cc.paint.values[0] * 255) + 0.5);
					palettep[i].green = (int)((cc.paint.values[1] * 255) + 0.5);
					palettep[i].blue = (int)((cc.paint.values[2] * 255) + 0.5);
				}
			}
			/*
			This is for normal 24-bit images
			*/
			else
			{
				palettep[i].red = gx_color_value_to_byte(rgb[0]);
				palettep[i].green = gx_color_value_to_byte(rgb[1]);
				palettep[i].blue = gx_color_value_to_byte(rgb[2]);
			}
		}
	}
	else {
		palettep = NULL;
		num_palette = 0;
	}
	/* add comment */
	strncpy(software_key, "Software", sizeof(software_key));
	gs_sprintf(software_text, "%s %d.%02d", gs_product,
		(int)(gs_revision / 100), (int)(gs_revision % 100));
	text_png.compression = -1;	/* uncompressed */
	text_png.key = software_key;
	text_png.text = software_text;
	text_png.text_length = strlen(software_text);

	dst_bpc = bit_depth;
	src_bpc = dst_bpc;
	if (errdiff)
		src_bpc = 8;
	else
		factor = 1;

	/* THIS FUCKING SHIT MAKES THE IMAGE SIZE WRONG*/
	/*width = pdev->width / factor;
	height = pdev->height / factor;*/
	/* This makes the image the actual image size */
	width = pie->width /*setup->width_in*/;
	height = pie->height /*setup->height_in*/;

#if PNG_LIBPNG_VER_MINOR >= 5
	png_set_pHYs(png_ptr, info_ptr,
		x_pixels_per_unit, y_pixels_per_unit, phys_unit_type);

	png_set_IHDR(png_ptr, info_ptr,
		width, height, bit_depth,
		color_type, PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT);
	if (palettep)
		png_set_PLTE(png_ptr, info_ptr, palettep, num_palette);

	png_set_text(png_ptr, info_ptr, &text_png, 1);
#else
	info_ptr->bit_depth = bit_depth;
	info_ptr->color_type = color_type;
	info_ptr->width = width;
	info_ptr->height = height;
	info_ptr->x_pixels_per_unit = x_pixels_per_unit;
	info_ptr->y_pixels_per_unit = y_pixels_per_unit;
	info_ptr->phys_unit_type = phys_unit_type;
	info_ptr->palette = palettep;
	info_ptr->num_palette = num_palette;
	info_ptr->valid |= valid;
	info_ptr->text = &text_png;
	info_ptr->num_text = 1;
	/* Set up the ICC information */
	if (pdev->icc_struct != NULL && pdev->icc_struct->device_profile[0] != NULL) {
		cmm_profile_t *icc_profile = pdev->icc_struct->device_profile[0];
		/* PNG can only be RGB or gray.  No CIELAB :(  */
		if (icc_profile->data_cs == gsRGB || icc_profile->data_cs == gsGRAY) {
			if (icc_profile->num_comps == pdev->color_info.num_components &&
				!(pdev->icc_struct->usefastcolor)) {
				info_ptr->iccp_name = icc_profile->name;
				info_ptr->iccp_profile = icc_profile->buffer;
				info_ptr->iccp_proflen = icc_profile->buffer_size;
				info_ptr->valid |= PNG_INFO_iCCP;
			}
		}
	}
#endif
	if (invert) {
		if (depth == 32)
			png_set_invert_alpha(png_ptr);
		else
			png_set_invert_mono(png_ptr);
	}
	if (bg_needed) {
		png_set_bKGD(png_ptr, info_ptr, &background);
	}
#if defined(ARCH_IS_BIG_ENDIAN) && (!ARCH_IS_BIG_ENDIAN)
	if (endian_swap) {
		png_set_swap(png_ptr);
	}
#endif

	/* write the file information */
	png_write_info(png_ptr, info_ptr);
















	// /* write the image rows */
	// raster = pie->num_planes * pie->plane_widths[0] * pie->plane_depths[0] / 8;
	// row = gs_alloc_bytes(/*pdev->memory*/ pie->dev->memory, raster, "png raster buffer");

	// if (row == 0) return gs_note_error(gs_error_VMerror);

	// // Clear memory
	// memset(row, 0xff, raster);

	// if (pie->num_planes == 1 && (pie->plane_depths[0] == 24 || pie->plane_depths[0] == 8))
	// {
	//     for (y = 0; y < height; y++) {

	//         /* Get factor rows worth of data */
	//         //row = planes[0].data;
	//code = (*dev_proc(/*pdev*/ pie->dev, get_bits))(/*pdev*/ pie->dev, y, row, NULL);
	//         if (code < 0) break; //return code;

	//         png_write_rows(png_ptr, &row, 1);
	//     }
	// }










	//#if PNG_LIBPNG_VER_MINOR >= 5
	//#else
	//	/* don't write the comments twice */
	//	info_ptr->num_text = 0;
	//	info_ptr->text = NULL;
	//#endif
	//
	//	/* For simplicity of code, we always go through the downscaler. For
	//	* non-supported depths, it will pass through with minimal performance
	//	* hit. So ensure that we only trigger downscales when we need them.
	//	*/
	//	code = gx_downscaler_init(&ds, (gx_device *)pdev, src_bpc, dst_bpc,
	//		depth / dst_bpc, factor, mfs, NULL, 0);
	//	if (code >= 0)
	//	{
	//		/* Write the contents of the image. */
	//		for (y = 0; y < height; y++) {
	//			gx_downscaler_copy_scan_lines(&ds, y, row, raster);
	//			png_write_rows(png_ptr, &row, 1);
	//		}
	//		gx_downscaler_fin(&ds);
	//	}
	//
	//	/* write the rest of the file */
	//	png_write_end(png_ptr, info_ptr);
	//
	//#if PNG_LIBPNG_VER_MINOR >= 5
	//#else
	//	/* if you alloced the palette, free it here */
	//	gs_free_object(mem, palettep, "png palette");
	//#endif
	//
	//done:
	//	/* free the structures */
	//	png_destroy_write_struct(&png_ptr, &info_ptr);
	//	gs_free_object(mem, row, "png raster buffer");

	return code;



































	//int code,i;			/* return code */
	//int factor = 1;
	//int depth = 0;
	//gs_memory_t *mem = pdev->memory;
	//bool invert = false, endian_swap = false, bg_needed = false;
	//png_byte bit_depth = 0;
	//png_byte color_type = 0;
	//png_uint_32 x_pixels_per_unit;
	//png_uint_32 y_pixels_per_unit;
	//png_byte phys_unit_type;
	//png_color_16 background;
	//png_uint_32 width, height;
	//png_color *palettep;
	//png_uint_16 num_palette;
	//png_uint_32 valid = 0;
	//bool errdiff = 0;
	//char software_key[80];
	//char software_text[256];
	//png_text text_png;
	//int dst_bpc, src_bpc;
	//png_color *palette;


	///* New stuff */
	//struct png_setup_s setup;
	//setup.depth = 0;
	//setup.external_invert = &pie->invert;

	//for (i = 0; i < pie->num_planes; ++i)
	//{
	//	setup.depth += pie->plane_depths[i];
	//}
	//
	//setup.height_in = pie->height;
	//setup.width_in = pie->width;
	//setup.HWResolution[0] = pdev->HWResolution[0];
	//setup.HWResolution[1] = pdev->HWResolution[1];
	//setup.info_ptr = pie->info_ptr;
	//setup.memory = pdev->memory;
	//setup.png_ptr = pie->png_ptr;

	//code = setup_png_from_struct(pdev, &setup);
	//return code;











	//	png_struct *png_ptr = pie->png_ptr;
	//	png_info *info_ptr = pie->info_ptr;
	//
	//	palette = (void *)gs_alloc_bytes(mem, 256 * sizeof(png_color),
	//		"png palette");
	//	if (palette == 0) {
	//		return gs_note_error(gs_error_VMerror);
	//	}
	//
	//	for (i = 0; i < pie->num_planes; ++i)
	//	{
	//		depth += pie->plane_depths[i];
	//	}
	//
	//	pie->invert = false;
	//	/* set the file information here */
	//	/* resolution is in pixels per meter vs. dpi */
	//	x_pixels_per_unit =
	//		(png_uint_32)(pdev->HWResolution[0] * (100.0 / 2.54) / factor + 0.5);
	//	y_pixels_per_unit =
	//		(png_uint_32)(pdev->HWResolution[1] * (100.0 / 2.54) / factor + 0.5);
	//
	//	phys_unit_type = PNG_RESOLUTION_METER;
	//	valid |= PNG_INFO_pHYs;
	//
	//	switch (depth) {
	//	case 32:
	//		bit_depth = 8;
	//		color_type = PNG_COLOR_TYPE_RGB_ALPHA;
	//		invert = true;
	//
	//		{
	//		background.index = 0;
	//		background.red =  0xff;
	//		background.green = 0xff;
	//		background.blue = 0xff;
	//		background.gray = 0;
	//		bg_needed = true;
	//		}
	//		errdiff = 1;
	//		break;
	//	case 48:
	//		bit_depth = 16;
	//		color_type = PNG_COLOR_TYPE_RGB;
	//#if defined(ARCH_IS_BIG_ENDIAN) && (!ARCH_IS_BIG_ENDIAN)
	//		endian_swap = true;
	//#endif
	//		break;
	//	case 24:
	//		bit_depth = 8;
	//		color_type = PNG_COLOR_TYPE_RGB;
	//		errdiff = 1;
	//		break;
	//	case 8:
	//		bit_depth = 8;
	//		//if (gx_device_has_color(pdev)) {
	//		//	color_type = PNG_COLOR_TYPE_PALETTE;
	//		//	errdiff = 0;
	//		//}
	//		//else {
	//		/* high level images don't have a color palette */
	//			color_type = PNG_COLOR_TYPE_GRAY;
	//			errdiff = 1;
	//			pie->invert = true;
	//		//}
	//		break;
	//	case 4:
	//		bit_depth = 4;
	//		color_type = PNG_COLOR_TYPE_PALETTE;
	//		break;
	//	case 1:
	//		bit_depth = 1;
	//		color_type = PNG_COLOR_TYPE_GRAY;
	//		/* invert monocrome pixels */
	//		invert = true;
	//		break;
	//	}
	//
	//	/* set the palette if there is one */
	//	if (color_type == PNG_COLOR_TYPE_PALETTE) {
	//		int i;
	//		int num_colors = 1 << depth;
	//		gx_color_value rgb[3];
	//
	//#if PNG_LIBPNG_VER_MINOR >= 5
	//		palettep = palette;
	//#else
	//		palettep =
	//			(void *)gs_alloc_bytes(mem, 256 * sizeof(png_color),
	//			"png palette");
	//		if (palettep == 0) {
	//			code = gs_note_error(gs_error_VMerror);
	//			goto done;
	//		}
	//#endif
	//		num_palette = num_colors;
	//		valid |= PNG_INFO_PLTE;
	//		for (i = 0; i < num_colors; i++) {
	//			(*dev_proc(pdev, map_color_rgb)) ((gx_device *)pdev,
	//				(gx_color_index)i, rgb);
	//			palettep[i].red = gx_color_value_to_byte(rgb[0]);
	//			palettep[i].green = gx_color_value_to_byte(rgb[1]);
	//			palettep[i].blue = gx_color_value_to_byte(rgb[2]);
	//		}
	//	}
	//	else {
	//		palettep = NULL;
	//		num_palette = 0;
	//	}
	//	/* add comment */
	//	strncpy(software_key, "Software", sizeof(software_key));
	//	gs_sprintf(software_text, "%s %d.%02d", gs_product,
	//		(int)(gs_revision / 100), (int)(gs_revision % 100));
	//	text_png.compression = -1;	/* uncompressed */
	//	text_png.key = software_key;
	//	text_png.text = software_text;
	//	text_png.text_length = strlen(software_text);
	//
	//	dst_bpc = bit_depth;
	//	src_bpc = dst_bpc;
	//	if (errdiff)
	//		src_bpc = 8;
	//	else
	//		factor = 1;
	//
	//	/* THIS FUCKING SHIT MAKES THE IMAGE SIZE WRONG*/
	//	/*width = pdev->width / factor;
	//	height = pdev->height / factor;*/
	//	/* This makes the image the actual image size */
	//	width = pie->width;
	//	height = pie->height;
	//
	//#if PNG_LIBPNG_VER_MINOR >= 5
	//	png_set_pHYs(png_ptr, info_ptr,
	//		x_pixels_per_unit, y_pixels_per_unit, phys_unit_type);
	//
	//	png_set_IHDR(png_ptr, info_ptr,
	//		width, height, bit_depth,
	//		color_type, PNG_INTERLACE_NONE,
	//		PNG_COMPRESSION_TYPE_DEFAULT,
	//		PNG_FILTER_TYPE_DEFAULT);
	//	if (palettep)
	//		png_set_PLTE(png_ptr, info_ptr, palettep, num_palette);
	//
	//	png_set_text(png_ptr, info_ptr, &text_png, 1);
	//#else
	//	info_ptr->bit_depth = bit_depth;
	//	info_ptr->color_type = color_type;
	//	info_ptr->width = width;
	//	info_ptr->height = height;
	//	info_ptr->x_pixels_per_unit = x_pixels_per_unit;
	//	info_ptr->y_pixels_per_unit = y_pixels_per_unit;
	//	info_ptr->phys_unit_type = phys_unit_type;
	//	info_ptr->palette = palettep;
	//	info_ptr->num_palette = num_palette;
	//	info_ptr->valid |= valid;
	//	info_ptr->text = &text_png;
	//	info_ptr->num_text = 1;
	//	/* Set up the ICC information */
	//	if (pdev->icc_struct != NULL && pdev->icc_struct->device_profile[0] != NULL) {
	//		cmm_profile_t *icc_profile = pdev->icc_struct->device_profile[0];
	//		/* PNG can only be RGB or gray.  No CIELAB :(  */
	//		if (icc_profile->data_cs == gsRGB || icc_profile->data_cs == gsGRAY) {
	//			if (icc_profile->num_comps == pdev->color_info.num_components &&
	//				!(pdev->icc_struct->usefastcolor)) {
	//				info_ptr->iccp_name = icc_profile->name;
	//				info_ptr->iccp_profile = icc_profile->buffer;
	//				info_ptr->iccp_proflen = icc_profile->buffer_size;
	//				info_ptr->valid |= PNG_INFO_iCCP;
	//			}
	//		}
	//	}
	//#endif
	//	if (invert) {
	//		if (depth == 32)
	//			png_set_invert_alpha(png_ptr);
	//		else
	//			png_set_invert_mono(png_ptr);
	//	}
	//	if (bg_needed) {
	//		png_set_bKGD(png_ptr, info_ptr, &background);
	//	}
	//#if defined(ARCH_IS_BIG_ENDIAN) && (!ARCH_IS_BIG_ENDIAN)
	//	if (endian_swap) {
	//		png_set_swap(png_ptr);
	//	}
	//#endif
	//
	//	/* write the file information */
	//	png_write_info(png_ptr, info_ptr);
	//
	//#if PNG_LIBPNG_VER_MINOR >= 5
	//#else
	//	/* don't write the comments twice */
	//	info_ptr->num_text = 0;
	//	info_ptr->text = NULL;
	//#endif
	//	return 0;
}

//int init_png(gs_memory_t *memory,
//	png_struct **png_ptrp,
//	png_info **info_ptrp,
//struct mem_encode *state,
//struct png_setup_s *setup)
int init_png(gx_device * pdev, struct mem_encode *state,
struct png_setup_s *setup)
{
	int code = 0;
	state->buffer = 0;
	state->memory = setup->memory;
	state->size = 0;
	png_struct *png_ptr;
	png_info *info_ptr;
	/* Initialize PNG structures */
	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	info_ptr = png_create_info_struct(png_ptr);

	if (png_ptr && info_ptr && !setjmp(png_jmpbuf(png_ptr))) {
		/* Setup the writing function */
		png_set_write_fn(png_ptr, state, my_png_write_data, my_png_flush);
		setup->png_ptr = png_ptr;
		setup->info_ptr = info_ptr;
		code = setup_png_from_struct(pdev, setup);
		if (!code)
		{
			return 0;
		}
	}
	png_destroy_write_struct(&png_ptr, &info_ptr);
	setup->png_ptr = 0;
	setup->info_ptr = 0;
	return code ? code : gs_note_error(gs_error_VMerror);
}

static int make_png_from_mdev(gx_device_memory *mdev, float tx, float ty)
{
	int code = 0;
	struct mem_encode state;
	struct png_setup_s setup;
	bool invert;
	int y, i, j, k;
	double black;
	unsigned char cmyk[4];
	setup.depth = mdev->color_info.depth;
	//setup.external_invert = &invert;
	setup.height_in = mdev->height;
	setup.HWResolution[0] = mdev->HWResolution[0];
	setup.HWResolution[1] = mdev->HWResolution[1];
	setup.memory = mdev->memory;
	setup.width_in = mdev->width;
	png_bytep row;
	char *buffer;
	gs_matrix m;
	gs_matrix_fixed ctm;
	gs_make_identity(&m);
	m.xx = mdev->width;
	m.yy = mdev->height;
	m.tx = tx;
	m.ty = ty;
	gs_matrix_fixed_from_matrix(&ctm, &m);

#undef fopen

	/* The png structures are set up and ready to use after init_png */
	code = init_png(mdev, &state, &setup);
	if (!code)
	{
		// The png data should all be ready for dumping

		// First we construct the binary buffer of PNG data
		for (y = 0; y < mdev->height; ++y)
		{
			if (setup.depth == 32)
			{
				buffer = gs_alloc_bytes(mdev->memory, mdev->raster, "png raster buffer");
				row = mdev->base + y * mdev->raster;
				j = 0;
				k = 0;
				for (i = 0; i < mdev->width; ++i)
				{
					//buffer[j + 0] = i % 256;
					//buffer[j + 1] = y % 256;
					//buffer[j + 2] = 0;
					////buffer[j + 3] = row[k + 0];
					//buffer[j + 3] = 0x80; // Full opacity
					
					buffer[j + 0] = row[k + 0];
					buffer[j + 1] = row[k + 1];
					buffer[j + 2] = row[k + 2];
					buffer[j + 3] = row[k + 3];
					//buffer[j + 3] = 0x00; // Full opacity
					
					j += 4;
					//k += 3;
					k += 4;
				}
				row = buffer;
			}
			else
			{
				row = mdev->base + y * mdev->raster;
			}


			png_write_rows(setup.png_ptr, &row, 1);

			if (setup.depth == 32)
			{
				gs_free_object(mdev->memory, buffer, "png raster buffer");
			}
		}

		png_write_end(setup.png_ptr, setup.info_ptr);

		write_base64_png(mdev->target, &state, ctm, m, mdev->width, mdev->height);
		gs_free_object(mdev->memory, state.buffer, "png img buf");
		png_destroy_write_struct(&setup.png_ptr, &setup.info_ptr);
		code = 0;
	}

	return code;
}

int setup_png_from_struct(gx_device * pdev, struct png_setup_s* setup)
{
	gs_memory_t *mem = pdev->memory;
	//int raster = gdev_prn_raster(pdev);
	//gx_downscaler_t ds;

	/* PNG structures */
	//byte *row = gs_alloc_bytes(mem, raster, "png raster buffer");
	png_struct *png_ptr = setup->png_ptr;
	//png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_info *info_ptr = setup->info_ptr;
	//png_create_info_struct(png_ptr);
	int y;
	int code;			/* return code */
	char software_key[80];
	char software_text[256];
	png_text text_png;
	int dst_bpc, src_bpc;
	bool errdiff = 0;
	int factor = 1;

	bool invert = false, endian_swap = false, bg_needed = false;
	png_byte bit_depth = 0;
	png_byte color_type = 0;
	png_uint_32 x_pixels_per_unit;
	png_uint_32 y_pixels_per_unit;
	png_byte phys_unit_type;
	png_color_16 background;
	png_uint_32 width, height;
#if PNG_LIBPNG_VER_MINOR >= 5
	//png_color palette[256];
	png_color *palette;
#endif
	png_color *palettep;
	png_uint_16 num_palette;
	png_uint_32 valid = 0;

	palette = (void *)gs_alloc_bytes(mem, 256 * sizeof(png_color), "png palette");
	if (palette == 0) {
		return gs_note_error(gs_error_VMerror);
	}

	/* Sanity check params */
	if (factor < 1) factor = 1;

	/* Slightly nasty, but it saves us duplicating this entire routine. */
	//if (monod) {
	//	errdiff = 1;
	//	depth = 1;
	//}

	if (/*row == 0 || */png_ptr == 0 || info_ptr == 0) {
		code = gs_note_error(gs_error_VMerror);
		return code;
		//goto done;
	}

	code = 0;			/* for normal path */

	///* set up the output control */
	//png_init_io(png_ptr, file);

	/* set the file information here */
	/* resolution is in pixels per meter vs. dpi */
	x_pixels_per_unit =
		(png_uint_32)(pdev->HWResolution[0] * (100.0 / 2.54) / factor + 0.5);
	y_pixels_per_unit =
		(png_uint_32)(pdev->HWResolution[1] * (100.0 / 2.54) / factor + 0.5);

	phys_unit_type = PNG_RESOLUTION_METER;
	valid |= PNG_INFO_pHYs;

	switch (setup->depth) {
	case 32:
		bit_depth = 8;
		color_type = PNG_COLOR_TYPE_RGB_ALPHA;
		invert = true;

		{
			background.index = 0;
			background.red = 0xff;
			background.green = 0xff;
			background.blue = 0xff;
			background.gray = 0;
			bg_needed = false;
		}
		errdiff = 1;
		break;
	case 48:
		bit_depth = 16;
		color_type = PNG_COLOR_TYPE_RGB;
#if defined(ARCH_IS_BIG_ENDIAN) && (!ARCH_IS_BIG_ENDIAN)
		endian_swap = true;
#endif
		break;
	case 24:
		bit_depth = 8;
		color_type = PNG_COLOR_TYPE_RGB;
		errdiff = 1;
		break;
	case 8:
		bit_depth = 8;
		if (gx_device_has_color(pdev)) {
			color_type = PNG_COLOR_TYPE_PALETTE;
			errdiff = 0;
		}
		else {
			color_type = PNG_COLOR_TYPE_GRAY;
			errdiff = 1;
		}
		break;
	case 4:
		bit_depth = 4;
		color_type = PNG_COLOR_TYPE_PALETTE;
		break;
	case 1:
		bit_depth = 1;
		color_type = PNG_COLOR_TYPE_GRAY;
		invert = true;
		break;
	}

	/* set the palette if there is one */
	if (color_type == PNG_COLOR_TYPE_PALETTE) {
		int i;
		int num_colors = 1 << setup->depth;
		gx_color_value rgb[3];

#if PNG_LIBPNG_VER_MINOR >= 5
		palettep = palette;
#else
		palettep =
			(void *)gs_alloc_bytes(mem, 256 * sizeof(png_color),
			"png palette");
		if (palettep == 0) {
			code = gs_note_error(gs_error_VMerror);
			goto done;
		}
#endif
		num_palette = num_colors;
		valid |= PNG_INFO_PLTE;
		for (i = 0; i < num_colors; i++) {
			(*dev_proc(pdev, map_color_rgb)) ((gx_device *)pdev,
				(gx_color_index)i, rgb);

			/*
			This crap kept me busy for 4 days straight. Needed to apply the palette
			to the image when the image is an 8-bit indexed image.
			*/
			if (setup->depth == 8)
			{
				//gs_client_color cc;
				//gs_cspace_indexed_lookup(pcs, i, &cc);

				//palettep[i].red = (int)((cc.paint.values[0] * 255) + 0.5);
				//palettep[i].green = (int)((cc.paint.values[1] * 255) + 0.5);
				//palettep[i].blue = (int)((cc.paint.values[2] * 255) + 0.5);
			}
			/*
			This is for normal 24-bit images
			*/
			else
			{
				palettep[i].red = gx_color_value_to_byte(rgb[0]);
				palettep[i].green = gx_color_value_to_byte(rgb[1]);
				palettep[i].blue = gx_color_value_to_byte(rgb[2]);
			}
		}
	}
	else {
		palettep = NULL;
		num_palette = 0;
	}
	/* add comment */
	strncpy(software_key, "Software", sizeof(software_key));
	gs_sprintf(software_text, "%s %d.%02d", gs_product,
		(int)(gs_revision / 100), (int)(gs_revision % 100));
	text_png.compression = -1;	/* uncompressed */
	text_png.key = software_key;
	text_png.text = software_text;
	text_png.text_length = strlen(software_text);

	dst_bpc = bit_depth;
	src_bpc = dst_bpc;
	if (errdiff)
		src_bpc = 8;
	else
		factor = 1;

	/* THIS FUCKING SHIT MAKES THE IMAGE SIZE WRONG*/
	/*width = pdev->width / factor;
	height = pdev->height / factor;*/
	/* This makes the image the actual image size */
	width = setup->width_in;
	height = setup->height_in;

#if PNG_LIBPNG_VER_MINOR >= 5
	png_set_pHYs(png_ptr, info_ptr,
		x_pixels_per_unit, y_pixels_per_unit, phys_unit_type);

	png_set_IHDR(png_ptr, info_ptr,
		width, height, bit_depth,
		color_type, PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT);
	if (palettep)
		png_set_PLTE(png_ptr, info_ptr, palettep, num_palette);

	png_set_text(png_ptr, info_ptr, &text_png, 1);
#else
	info_ptr->bit_depth = bit_depth;
	info_ptr->color_type = color_type;
	info_ptr->width = width;
	info_ptr->height = height;
	info_ptr->x_pixels_per_unit = x_pixels_per_unit;
	info_ptr->y_pixels_per_unit = y_pixels_per_unit;
	info_ptr->phys_unit_type = phys_unit_type;
	info_ptr->palette = palettep;
	info_ptr->num_palette = num_palette;
	info_ptr->valid |= valid;
	info_ptr->text = &text_png;
	info_ptr->num_text = 1;
	/* Set up the ICC information */
	if (pdev->icc_struct != NULL && pdev->icc_struct->device_profile[0] != NULL) {
		cmm_profile_t *icc_profile = pdev->icc_struct->device_profile[0];
		/* PNG can only be RGB or gray.  No CIELAB :(  */
		if (icc_profile->data_cs == gsRGB || icc_profile->data_cs == gsGRAY) {
			if (icc_profile->num_comps == pdev->color_info.num_components &&
				!(pdev->icc_struct->usefastcolor)) {
				info_ptr->iccp_name = icc_profile->name;
				info_ptr->iccp_profile = icc_profile->buffer;
				info_ptr->iccp_proflen = icc_profile->buffer_size;
				info_ptr->valid |= PNG_INFO_iCCP;
			}
		}
	}
#endif
	if (invert) {
		if (setup->depth == 32)
			png_set_invert_alpha(png_ptr);
		else
			png_set_invert_mono(png_ptr);
	}
	if (bg_needed) {
		png_set_bKGD(png_ptr, info_ptr, &background);
	}
#if defined(ARCH_IS_BIG_ENDIAN) && (!ARCH_IS_BIG_ENDIAN)
	if (endian_swap) {
		png_set_swap(png_ptr);
	}
#endif

	/* write the file information */
	png_write_info(png_ptr, info_ptr);

	return 0;
}
