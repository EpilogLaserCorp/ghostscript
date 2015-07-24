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

#include "string_.h"
#include "gx.h"
#include "gserrors.h"
#include "gdevvec.h"
#include "stream.h"
#include "gxpath.h"
#include "gzcpath.h"
#include "gxdownscale.h"

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
	int page_count;	/* how many output_page calls we've seen */
	gx_color_index strokecolor, fillcolor;
	double linewidth;
	gs_line_cap linecap;
	gs_line_join linejoin;
	double miterlimit;
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


static dev_proc_begin_typed_image(svg_begin_typed_image);
static dev_proc_begin_image(svg_begin_image);

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
	NULL,			/* copy_mono */\
	NULL,			/* copy_color */\
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
	gdev_vector_fill_path, \
	gdev_vector_stroke_path, \
	NULL,			/* fill_mask */\
	gdev_vector_fill_trapezoid, \
	gdev_vector_fill_parallelogram, \
	gdev_vector_fill_triangle, \
	NULL,			/* draw_thin_line */\
	NULL, /*svg_begin_image,			/* begin_image */\
	NULL,                   /* image_data */\
	NULL,                   /* end_image */\
	NULL,                   /* strip_tile_rectangle */\
	NULL,			/* strip_copy_rop */\
	NULL,			/* get_clipping_box */\
	svg_begin_typed_image, /**/\
	NULL,			/* get_bits_rectangle */\
	NULL,			/* map_color_rgb_alpha */\
	NULL,			/* create_compositor */\
	NULL			/* get_hardware_params */\
	/*epilog_text_begin*/\
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
	png_struct *png_ptr;
	png_info *info_ptr;
	png_color *palettep;
	gx_downscaler_t *ds;
	struct mem_encode state;
	gs_matrix_fixed ctm;
	gs_matrix ImageMatrix;
} svg_image_enum_t;

int setup_png(gx_device * pdev, svg_image_enum_t  *pie);
void my_png_write_data(png_structp png_ptr, png_bytep data, png_size_t length);
void my_png_flush(png_structp png_ptr);

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
	svg->page_count = 0;
	svg->strokecolor = gx_no_color_index;
	svg->fillcolor = gx_no_color_index;
	/* these should be the graphics library defaults instead? */
	svg->linewidth = SVG_DEFAULT_LINEWIDTH;
	svg->linecap = SVG_DEFAULT_LINECAP;
	svg->linejoin = SVG_DEFAULT_LINEJOIN;
	svg->miterlimit = SVG_DEFAULT_MITERLIMIT;

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
	while (svg->mark > 0) {
		svg_write(svg, "</g>\n");
		svg->mark--;
	}
	svg_write(svg, "</page>");
	svg_write(svg, "<page>");
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
	while (svg->mark > 0) {
		svg_write(svg, "</g>\n");
		svg->mark--;
	}
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
	gs_sprintf(line, "\n\twidth='%dpt' height='%dpt'>\n",
		(int)svg->MediaSize[0], (int)svg->MediaSize[1]);
	sputs(s, (byte *)line, strlen(line), &used);

	/* Enable multipule page output*/
	gs_sprintf(line, "<pageSet>\n");
	sputs(s, (byte *)line, strlen(line), &used);

	

	/* mark that we've been called */
	svg->header = 1;

	return 0;
}

static gx_color_index
svg_get_color(gx_device_svg *svg, const gx_drawing_color *pdc)
{

	gx_color_index color = gx_no_color_index;

	if (gx_dc_is_pure(pdc))
		color = gx_dc_pure_color(pdc);
	return color;
}

static int
svg_write_state(gx_device_svg *svg)
{
	char line[SVG_LINESIZE];

	/* has anything changed? */
	if (!svg->dirty)
		return 0;

	/* close the current graphics state element, if any */
	if (svg->mark > 1) {
		svg_write(svg, "</g>\n");
		svg->mark--;
	}
	/* write out the new current state */
	svg_write(svg, "<g ");
	if (svg->strokecolor != gx_no_color_index) {
		gs_sprintf(line, " stroke='#%06x'", (uint)(svg->strokecolor & 0xffffffL));
		svg_write(svg, line);
	}
	else {
		svg_write(svg, " stroke='none'");
	}
	if (svg->fillcolor != gx_no_color_index) {
		gs_sprintf(line, " fill='#%06x'", (uint)(svg->fillcolor & 0xffffffL));
		svg_write(svg, line);
	}
	else {
		svg_write(svg, " fill='none'");
	}
	if (svg->linewidth != 1.0) {
		gs_sprintf(line, " stroke-width='%lf'", svg->linewidth);
		svg_write(svg, line);
	}
	if (svg->linecap != SVG_DEFAULT_LINECAP) {
		switch (svg->linecap) {
		case gs_cap_round:
			svg_write(svg, " stroke-linecap='round'");
			break;
		case gs_cap_square:
			svg_write(svg, " stroke-linecap='square'");
			break;
		case gs_cap_butt:
		default:
			/* treat all the other options as the default */
			svg_write(svg, " stroke-linecap='butt'");
			break;
		}
	}
	if (svg->linejoin != SVG_DEFAULT_LINEJOIN) {
		switch (svg->linejoin) {
		case gs_join_round:
			svg_write(svg, " stroke-linejoin='round'");
			break;
		case gs_join_bevel:
			svg_write(svg, " stroke-linejoin='bevel'");
			break;
		case gs_join_miter:
		default:
			/* SVG doesn't support any other variants */
			svg_write(svg, " stroke-linejoin='miter'");
			break;
		}
	}
	if (svg->miterlimit != SVG_DEFAULT_MITERLIMIT) {
		gs_sprintf(line, " stroke-miterlimit='%lf'", svg->miterlimit);
		svg_write(svg, line);
	}
	svg_write(svg, ">\n");
	svg->mark++;

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
	const byte * pg = (byte*)"<page>\n";
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
	gx_color_index fill = svg_get_color(svg, pdc);

	if_debug0m('_', svg->memory, "svg_setfillcolor\n");

	if (svg->fillcolor == fill)
		return 0; /* not a new color */
	/* update our state with the new color */
	svg->fillcolor = fill;
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

static int
svg_dorect(gx_device_vector *vdev, fixed x0, fixed y0,
fixed x1, fixed y1, gx_path_type_t type)
{
	gx_device_svg *svg = (gx_device_svg *)vdev;
	char line[300];

	if_debug0m('_', svg->memory, "svg_dorect");
	svg_print_path_type(svg, type);
	if_debug0m('_', svg->memory, "\n");

	svg_write_state(svg);

	if (type & gx_path_type_clip) {
		svg_write(svg, "<clipPath>\n");
	}

	gs_sprintf(line, "<rect x='%lf' y='%lf' width='%lf' height='%lf'",
		fixed2float(x0), fixed2float(y0),
		fixed2float(x1 - x0), fixed2float(y1 - y0));
	svg_write(svg, line);
	/* override the inherited stroke attribute if we're not stroking */
	if (!(type & gx_path_type_stroke) && (svg->strokecolor != gx_no_color_index))
		svg_write(svg, " stroke='none'");
	/* override the inherited fill attribute if we're not filling */
	if (!(type & gx_path_type_fill) && (svg->fillcolor != gx_no_color_index))
		svg_write(svg, " fill='none'");
	svg_write(svg, "/>\n");

	if (type & gx_path_type_clip) {
		svg_write(svg, "</clipPath>\n");
	}

	return 0;
}

static int
svg_beginpath(gx_device_vector *vdev, gx_path_type_t type)
{
	gx_device_svg *svg = (gx_device_svg *)vdev;

	/* skip non-drawing paths for now */
	if (!(type & gx_path_type_fill) && !(type & gx_path_type_stroke))
		return 0;

	if_debug0m('_', svg->memory, "svg_beginpath ");
	svg_print_path_type(svg, type);
	if_debug0m('_', svg->memory, "\n");

	svg_write_state(svg);
	svg_write(svg, "<path d='");

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
		return 0;

	if_debug0m('_', svg->memory, "svg_endpath ");
	svg_print_path_type(svg, type);
	if_debug0m('_', svg->memory, "\n");

	/* close the path data attribute */
	svg_write(svg, "'");

	/* override the inherited stroke attribute if we're not stroking */
	if (!(type & gx_path_type_stroke) && (svg->strokecolor != gx_no_color_index))
		svg_write(svg, " stroke='none'");

	/* override the inherited fill attribute if we're not filling */
	if (!(type & gx_path_type_fill) && (svg->fillcolor != gx_no_color_index))
		svg_write(svg, " fill='none'");

	svg_write(svg, "/>\n");

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
	unsigned char rgb[3];
	unsigned char cmyk[4];
	double black;
	byte *row;
	raster = pie->num_planes*pie->plane_widths[0] * pie->plane_depths[0] / 8;
	row = gs_alloc_bytes(pie->memory, raster, "png raster buffer");
	if (row == 0)
		return gs_note_error(gs_error_VMerror);
	// Clear memory
	memset(row, 0xff, raster);

	// Get the planes copied
	for (y = 0; y < height; y++)
	{
		if (pie->num_planes == 1 && pie->plane_depths[0] == 24)
			memcpy(row, planes[0].data+planes[0].data_x, raster);
		else
		{
			for (i = 0; i < pie->plane_widths[0]; i++) {
				for (k = 0; k < pie->num_planes; ++k) {

					if (planes[k].data && pie->num_planes < 4)
					{
						bytes = pie->plane_depths[k] / 8;
						ind = i*pie->num_planes*bytes + k;
						memcpy(&row[ind], &planes[k].data[i*bytes], bytes);
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
					black = (1.0 - cmyk[3] / 255.0);
					rgb[2] = (unsigned char)(255 * (1.0 - cmyk[0] / 255.0)*black);
					rgb[1] = (unsigned char)(255 * (1.0 - cmyk[1] / 255.0)*black);
					rgb[0] = (unsigned char)(255 * (1.0 - cmyk[2] / 255.0)*black);
					memcpy(&row[i * 3], rgb, 3);
				}
			}
		}
		png_write_rows(pie->png_ptr, &row, 1);
	}
	*rows_used = height;
	if ((pie->rows_left % (pie->height/100)) == 0)
		dmprintf1(info->memory, "%g complete\n", (float)(pie->height - pie->rows_left) / pie->height * 100);
	gs_free_object(pie->memory, row, "png raster buffer");
	return (pie->rows_left -= height) <= 0;
}
static int
svg_end_image(gx_image_enum_common_t * info, bool draw_last)
{
	svg_image_enum_t *pie = (svg_image_enum_t *)info;
	char line[SVG_LINESIZE];
	size_t outputSize = 0;
	byte *buffer = 0;
	float ty;
	/* Do some cleanup */
	if (pie->palettep)
		gs_free_object(pie->memory, pie->palettep, "png palette");
	if (pie->ds)
	{
		gx_downscaler_fin(pie->ds);
		gs_free_object(pie->memory, pie->ds, "png downscaler");
	}
	if (pie->png_ptr && pie->info_ptr)
		png_destroy_write_struct(&pie->png_ptr, &pie->info_ptr);

	/* Flush the buffer as base 64 */
	
	buffer = base64_encode(pie->memory, pie->state.buffer, pie->state.size, &outputSize);
	ty = (pie->ImageMatrix.yy > 0 ? 0 : pie->ctm.yy) + pie->ctm.ty;
	gs_sprintf(line, "<g transform='matrix(%f,%f,%f,%f,%f,%f)'>",
		pie->ImageMatrix.xx/pie->width*pie->ctm.xx/pie->width,
		pie->ctm.xy / pie->width,
		pie->ImageMatrix.yy / pie->height * pie->ctm.yx / pie->height,
		pie->ImageMatrix.yy/pie->height*pie->ctm.yy/pie->height,
		pie->ctm.tx,
		ty);
	svg_write(info->dev, line);
	gs_sprintf(line, "<image width='%d' height='%d' xlink:href=\"data:image/png;base64,",
		pie->width,pie->height);
	svg_write(info->dev, line);
	svg_write_bytes(info->dev, buffer, outputSize);
	svg_write(info->dev, "\"/>");
	svg_write(info->dev, "</g>");

	gs_free_object(pie->memory, pie->state.buffer, "png img buf");
	gs_free_object(pie->memory, buffer, "base64 buffer");

	gx_image_free_enum(&info);
	return 0;
}
static const gx_image_enum_procs_t svg_image_enum_procs = {
	svg_plane_data, svg_end_image
};

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
	svg_image_enum_t *pie;
	const gs_pixel_image_t *ppi = (const gs_pixel_image_t *)pim;
	int ncomp;
	int code;			/* return code */

	dmprintf7(dev->memory, "begin_typed_image(type=%d, ImageMatrix=[%g %g %g %g %g %g]",
		pim->type->index, pim->ImageMatrix.xx, pim->ImageMatrix.xy,
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
	if (gx_image_enum_common_init((gx_image_enum_common_t *)pie,
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
	pie->state.memory = dev->memory;
	pie->width = ppi->Width;
	pie->height = ppi->Height;
	pie->ctm = pis->ctm;
	pie->ImageMatrix = ppi->ImageMatrix;

	/* Initialize PNG structures */
	pie->png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	pie->info_ptr = png_create_info_struct(pie->png_ptr);

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
	
	code = setup_png(dev, pie);
	if (code) {
		goto done;
	}

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

int setup_png(gx_device * pdev , svg_image_enum_t  *pie)
{
	int code;			/* return code */
	int factor = 1;
	int depth = pdev->color_info.depth;
	gs_memory_t *mem = pdev->memory;
	bool invert = false, endian_swap = false, bg_needed = false;
	png_byte bit_depth = 0;
	png_byte color_type = 0;
	png_uint_32 x_pixels_per_unit;
	png_uint_32 y_pixels_per_unit;
	png_byte phys_unit_type;
	png_color_16 background;
	png_uint_32 width, height;
	png_color *palettep;
	png_uint_16 num_palette;
	png_uint_32 valid = 0;
	bool errdiff = 0;
	char software_key[80];
	char software_text[256];
	png_text text_png;
	int dst_bpc, src_bpc;
	png_color palette[256];

	png_struct *png_ptr = pie->png_ptr;
	png_info *info_ptr = pie->info_ptr;

	pie->ds = 0;
	pie->palettep = 0;
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
		background.index = 0;
		background.red =  0xff;
		background.green = 0xff;
		background.blue = 0xff;
		background.gray = 0;
		bg_needed = true;
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
		/* invert monocrome pixels */
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
			palettep[i].red = gx_color_value_to_byte(rgb[0]);
			palettep[i].green = gx_color_value_to_byte(rgb[1]);
			palettep[i].blue = gx_color_value_to_byte(rgb[2]);
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
	width = pie->width;
	height = pie->height;

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

#if PNG_LIBPNG_VER_MINOR >= 5
#else
	/* don't write the comments twice */
	info_ptr->num_text = 0;
	info_ptr->text = NULL;
#endif
	
	
	gx_downscaler_t *ds = 0;

	/* Save for later cleanup */
	pie->ds = ds;
	pie->palettep = palettep;
	return 0;
}
