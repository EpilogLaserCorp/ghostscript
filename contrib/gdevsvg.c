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


static dev_proc_begin_typed_image(svg_begin_image);

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
	svg_begin_image,			/* begin_image */\
	NULL,                   /* image_data */\
	NULL,                   /* end_image */\
	NULL,                   /* strip_tile_rectangle */\
	NULL			/* strip_copy_rop */\
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
	gs_sprintf(line, "<svg xmlns='%s' version='%s'",
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
	if_debug1m('_', svg->memory, "svg_beginpage (page count %d)\n", svg->page_count);
	svg_write_header(svg);
	/* we may be called from beginpage, so we can't use
	svg_write() which calls gdev_vector_stream()
	which calls beginpage! */
	sputs(svg->strm, "<page>\n", strlen("<page>\n"), &used);
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


/* Image handling */
static image_enum_proc_plane_data(svg_image_data);
static image_enum_proc_end_image(svg_image_end_image);
static const gx_image_enum_procs_t svg_image_enum_procs = {
	svg_image_data, svg_image_end_image
};



typedef struct svg_image_enum_s {
	gdev_vector_image_enum_common;
	gs_matrix mat;
	byte *buffer; /* Needed for unpacking/decoding of image data */
	byte *devc_buffer; /* Needed for case where we are mapping to device colors */
	gs_color_space *pcs;     /* Needed for Sep, DeviceN, Indexed */
	gsicc_link_t *icc_link;  /* Needed for CIELAB */
	const gs_imager_state *pis;    /* Needed for color conversions of DeviceN etc */
	FILE *fid;
} svg_image_enum_t;

gs_private_st_suffix_add3(st_svg_image_enum, svg_image_enum_t,
	"svg_image_enum_t", svg_image_enum_enum_ptrs,
	svg_image_enum_reloc_ptrs, st_vector_image_enum,
	buffer, devc_buffer, pis);

static int
svg_begin_image(gx_device *dev, const gs_imager_state *pis,
const gs_image_t *pim, gs_image_format_t format,
const gs_int_rect *prect, const gx_drawing_color *pdcolor,
const gx_clip_path *pcpath, gs_memory_t *mem,
gx_image_enum_common_t **pinfo)
{
	gx_device_vector *vdev = (gx_device_vector *)dev;
	gx_device_svg *xdev = (gx_device_svg *)dev;
	gs_color_space *pcs = pim->ColorSpace;
	svg_image_enum_t *pie = NULL;
	gs_matrix mat;
	int code;
	gx_clip_path cpath;
	gs_fixed_rect bbox;
	int bits_per_pixel;
	int num_components;
	int bsize;
	cmm_profile_t *icc_profile = NULL;
	gs_color_space_index csindex;
	float index_decode[2];
	gsicc_rendering_param_t rendering_params;
	bool force8bit = false;

//	/* No image mask yet.  Also, need a color space */
//	if (pcs == NULL || ((const gs_image1_t *)pim)->ImageMask)
//		goto use_default;
//
//	/* No indexed images that are not 8 bit. */
//	csindex = gs_color_space_get_index(pcs);
//	if (csindex == gs_color_space_index_Indexed && pim->BitsPerComponent != 8)
//		goto use_default;
//
//	/* Also need imager state for these color spaces */
//	if (pis == NULL && (csindex == gs_color_space_index_Indexed ||
//		csindex == gs_color_space_index_Separation ||
//		csindex == gs_color_space_index_DeviceN))
//		goto use_default;
//
//	gs_matrix_invert(&pim->ImageMatrix, &mat);
//	gs_matrix_multiply(&mat, &ctm_only(pis), &mat);
//
//	pie = gs_alloc_struct(mem, svg_image_enum_t, &st_svg_image_enum,
//		"svg_begin_image");
//	if (pie == 0)
//		return_error(gs_error_VMerror);
//	pie->buffer = NULL;
//	pie->devc_buffer = NULL;
//	pie->pis = NULL;
//
//	/* Set the brush types to image */
//	svg_setstrokebrush(xdev, svg_imagebrush);
//	svg_setfillbrush(xdev, svg_imagebrush);
//	pie->mat = mat;
//	xdev->svg_pie = pie;
//	/* We need this set a bit early for the ICC relationship writing */
//	pie->dev = (gx_device*)xdev;
//
//	/* If the color space is DeviceN, Sep or indexed these end up getting
//	mapped to the color space defined by the device profile.  XPS only
//	support RGB indexed images so we just expand if for now. ICC link
//	creation etc is handled during the remap/concretization of the colors */
//	if (csindex == gs_color_space_index_Indexed ||
//		csindex == gs_color_space_index_Separation ||
//		csindex == gs_color_space_index_DeviceN) {
//		cmm_dev_profile_t *dev_profile;
//		pie->pcs = pcs;
//		rc_increment(pcs);
//		code = dev_proc(dev, get_profile)(dev, &(dev_profile));
//		/* Just use the "default" profile for now */
//		icc_profile = dev_profile->device_profile[0];
//		force8bit = true; /* Output image is 8 bit regardless of source */
//	}
//	else {
//		/* An ICC, RGB, CMYK, Gray color space */
//		pie->pcs = NULL;
//		/* Get the ICC profile */
//		if (gs_color_space_is_PSCIE(pcs)) {
//			if (pcs->icc_equivalent == NULL) {
//				bool is_lab;
//				gs_colorspace_set_icc_equivalent(pcs, &is_lab, pis->memory);
//			}
//			icc_profile = pcs->icc_equivalent->cmm_icc_profile_data;
//		}
//		else {
//			icc_profile = pcs->cmm_icc_profile_data;
//		}
//	}
//
//	/* Set up for handling case where we are in CIELAB. In this case, we are
//	going out to the default RGB color space */
//	if (icc_profile->islab) {
//		/* Create the link */
//		rendering_params.black_point_comp = gsBLACKPTCOMP_ON;
//		rendering_params.graphics_type_tag = GS_IMAGE_TAG;
//		rendering_params.override_icc = false;
//		rendering_params.preserve_black = gsBKPRESNOTSPECIFIED;
//		rendering_params.rendering_intent = gsPERCEPTUAL;
//		rendering_params.cmm = gsCMM_DEFAULT;
//		pie->icc_link = gsicc_get_link_profile(pis, dev, icc_profile,
//			pis->icc_manager->default_rgb, &rendering_params, pis->memory, false);
//		icc_profile = pis->icc_manager->default_rgb;
//	}
//	else {
//		pie->icc_link = NULL;
//	}
//
//	/* Now we actually write out the image and icc profile data to the zip
//	package. Test if profile is already here. If not, add it. */
//	if (svg_find_icc(xdev, icc_profile) == NULL) {
//		icc_data = (svg_icc_data_t*)gs_alloc_bytes(dev->memory->non_gc_memory,
//			sizeof(svg_icc_data_t), "svg_begin_image");
//		if (icc_data == NULL)
//			gs_throw(gs_error_VMerror, "Allocation of icc_data failed");
//
//		icc_data->hash = gsicc_get_hash(icc_profile);
//		if (xdev->icc_data == NULL) {
//			icc_data->index = 0;
//			xdev->icc_data = icc_data;
//			xdev->icc_data->next = NULL;
//		}
//		else {
//			icc_data->next = xdev->icc_data;
//			icc_data->index = icc_data->next->index + 1;
//			xdev->icc_data = icc_data;
//		}
//
//		/* Get name for mark up and for relationship. Have to wait and do
//		this after it is added to the package */
//		code = svg_create_icc_name(xdev, icc_profile, &(pie->icc_name[0]));
//		if (code < 0)
//			return gs_rethrow_code(code);
//
//		/* Add profile to the package. Here like images we are going to write
//		the data now.  Rather than later. */
//		code = svg_write_profile(pis, &(pie->icc_name[0]), icc_profile, xdev);
//		if (code < 0)
//			return gs_rethrow_code(code);
//
//		/* Add ICC relationship */
//		svg_add_icc_relationship(pie);
//	}
//	else {
//		/* Get name for mark up.  We already have it in the relationship and list */
//		code = svg_create_icc_name(xdev, icc_profile, &(pie->icc_name[0]));
//		if (code < 0)
//			return gs_rethrow_code(code);
//	}
//
//	/* Get image name for mark up */
//	svg_create_image_name(dev, &(pie->file_name[0]));
//	/* Set width and height here */
//	pie->width = pim->Width;
//	pie->height = pim->Height;
//
//	if (pcpath == NULL) {
//		(*dev_proc(dev, get_clipping_box)) (dev, &bbox);
//		gx_cpath_init_local(&cpath, dev->memory);
//		code = gx_cpath_from_rectangle(&cpath, &bbox);
//		pcpath = &cpath;
//	}
//	else {
//		/* Force vector device to do new path as the clip path is the image
//		path.  I had a case where the clip path ids were the same but the
//		CTM was changing which resulted in subsequent images coming up
//		missing on the page. i.e. only the first one was shown. */
//		((gx_device_vector*)vdev)->clip_path_id = vdev->no_clip_path_id;
//	}
//
//	code = gdev_vector_begin_image(vdev, pis, pim, format, prect,
//		pdcolor, pcpath, mem, &svg_image_enum_procs,
//		(gdev_vector_image_enum_t *)pie);
//	if (code < 0)
//		return code;
//
//	if ((pie->tif = tiff_from_name(xdev, pie->file_name, false, false)) == NULL)
//		return_error(gs_error_VMerror);
//
//	/* Null out pie.  Only needed for the above vector command and tiff set up */
//	xdev->svg_pie = NULL;
//	svg_tiff_set_handlers();
//	code = tiff_set_values(pie, pie->tif, icc_profile, force8bit);
//	if (code < 0)
//		return gs_rethrow_code(code);
//	code = TIFFCheckpointDirectory(pie->tif);
//
//	num_components = gs_color_space_num_components(pcs);
//	bits_per_pixel = pim->BitsPerComponent * num_components;
//	pie->decode_st.bps = bits_per_pixel / num_components;
//	pie->bytes_comp = (pie->decode_st.bps > 8 ? 2 : 1);
//	pie->decode_st.spp = num_components;
//	pie->decode_st.unpack = NULL;
//	get_unpack_proc((gx_image_enum_common_t*)pie, &(pie->decode_st), pim->format,
//		pim->Decode);
//
//	/* The decode mapping for index colors needs an adjustment */
//	if (csindex == gs_color_space_index_Indexed) {
//		if (pim->Decode[0] == 0 &&
//			pim->Decode[1] == 255) {
//			index_decode[0] = 0;
//			index_decode[1] = 1.0;
//		}
//		else {
//			index_decode[0] = pim->Decode[0];
//			index_decode[1] = pim->Decode[1];
//		}
//		get_map(&(pie->decode_st), pim->format, index_decode);
//	}
//	else {
//		get_map(&(pie->decode_st), pim->format, pim->Decode);
//	}
//
//	/* Allocate our decode buffer. */
//	bsize = ((pie->decode_st.bps > 8 ? (pim->Width) * 2 : pim->Width) + 15) * num_components;
//	pie->buffer = gs_alloc_bytes(mem, bsize, "svg_begin_typed_image(buffer)");
//	if (pie->buffer == 0) {
//		gs_free_object(mem, pie, "svg_begin_typed_image");
//		*pinfo = NULL;
//		return_error(gs_error_VMerror);
//	}
//
//	/* If needed, allocate our device color buffer.  We will always do 8 bit here */
//	if (csindex == gs_color_space_index_Indexed ||
//		csindex == gs_color_space_index_Separation ||
//		csindex == gs_color_space_index_DeviceN) {
//		bsize = (pim->Width + 15) * icc_profile->num_comps;
//		pie->devc_buffer = gs_alloc_bytes(mem, bsize, "svg_begin_typed_image(devc_buffer)");
//		if (pie->devc_buffer == 0) {
//			gs_free_object(mem, pie, "svg_begin_typed_image");
//			*pinfo = NULL;
//			return_error(gs_error_VMerror);
//		}
//		/* Also, the color remaps need the imager state */
//		pie->pis = pis;
//	}
//
//	*pinfo = (gx_image_enum_common_t *)pie;
//	return 0;
//use_default:
	//if (pie != NULL && pie->buffer != NULL)
	//	gs_free_object(mem, pie->buffer, "svg_begin_image");
	//if (pie != NULL && pie->devc_buffer != NULL)
	//	gs_free_object(mem, pie->devc_buffer, "svg_begin_image");
	//if (pie != NULL)
	//	gs_free_object(mem, pie, "svg_begin_image");

	return gx_default_begin_image(dev, pis, pim, format, prect,
		pdcolor, pcpath, mem, pinfo);
}


/* Chunky or planar in and chunky out */
static int
svg_image_data(gx_image_enum_common_t *info,
const gx_image_plane_t *planes, int height, int *rows_used)
{
	svg_image_enum_t *pie = (svg_image_enum_t *)info;
	//int data_bit = planes[0].data_x * info->plane_depths[0];
	//int width_bits = pie->width * info->plane_depths[0];
	//int bytes_comp = pie->bytes_comp;
	//int i, plane;
	//int code = 0;
	//int width = pie->width;
	//int num_planes = pie->num_planes;
	//int dsize = (((width + (planes[0]).data_x) * pie->decode_st.spp *
	//	pie->decode_st.bps / num_planes + 7) >> 3);
	//void *bufend = (void*)(pie->buffer + width * bytes_comp * pie->decode_st.spp);
	//byte *outbuffer;

	//if (width_bits != pie->bits_per_row || (data_bit & 7) != 0)
	//	return_error(gs_error_rangecheck);
	//if (height > pie->height - pie->y)
	//	height = pie->height - pie->y;

	//for (i = 0; i < height; pie->y++, i++) {
	//	int pdata_x;
	//	/* Plane zero done here to get the pointer to the data */
	//	const byte *data_ptr = planes[0].data + planes[0].raster * i + (data_bit >> 3);
	//	byte *des_ptr = pie->buffer;
	//	byte *buffer = (byte *)(*pie->decode_st.unpack)(des_ptr, &pdata_x,
	//		data_ptr, 0, dsize, &(pie->decode_st.map[0]),
	//		pie->decode_st.spread, pie->decode_st.spp);

	//	/* Step through the planes having decode do the repack to chunky as
	//	well as any decoding needed */
	//	for (plane = 1; plane < num_planes; plane++) {
	//		data_ptr = planes[plane].data + planes[plane].raster * i + (data_bit >> 3);
	//		des_ptr = pie->buffer + plane * pie->bytes_comp;
	//		/* This does the planar to chunky conversion */
	//		(*pie->decode_st.unpack)(des_ptr, &pdata_x,
	//			data_ptr, 0, dsize, &(pie->decode_st.map[plane]),
	//			pie->decode_st.spread, pie->decode_st.spp);
	//	}

	//	/* CIELAB does not get mapped.  Handled in color management */
	//	if (pie->icc_link == NULL) {
	//		pie->decode_st.applymap(pie->decode_st.map, (void*)buffer,
	//			pie->decode_st.spp, (void*)pie->buffer, bufend);
	//		/* Index, Sep and DeviceN are mapped to color space defined by
	//		device profile */
	//		if (pie->pcs != NULL) {
	//			/* In device color space */
	//			code = set_device_colors(pie);
	//			if (code < 0)
	//				return gs_rethrow_code(code);
	//			outbuffer = pie->devc_buffer;
	//		}
	//		else {
	//			/* In source color space */
	//			outbuffer = pie->buffer;
	//		}
	//	}
	//	else {
	//		/* CIELAB to default RGB */
	//		gsicc_bufferdesc_t input_buff_desc;
	//		gsicc_bufferdesc_t output_buff_desc;
	//		gsicc_init_buffer(&input_buff_desc, 3, bytes_comp,
	//			false, false, false, 0, width * bytes_comp * 3,
	//			1, width);
	//		gsicc_init_buffer(&output_buff_desc, 3, bytes_comp,
	//			false, false, false, 0, width * bytes_comp * 3,
	//			1, width);
	//		(pie->icc_link->procs.map_buffer)(pie->dev, pie->icc_link,
	//			&input_buff_desc, &output_buff_desc, (void*)buffer,
	//			(void*)pie->buffer);
	//		outbuffer = pie->buffer;
	//	}
	//	code = TIFFWriteScanline(pie->tif, outbuffer, pie->y, 0);
	//	if (code < 0)
	//		return code;
	//}
	*rows_used = height;
	return pie->y >= pie->height;
}

/* Clean up by releasing the buffers. */
static int
svg_image_end_image(gx_image_enum_common_t * info, bool draw_last)
{
	svg_image_enum_t *pie = (svg_image_enum_t *)info;
	int code = 0;

//	/* N.B. Write the final strip, if any. */
//
//	code = TIFFWriteDirectory(pie->tif);
//	TIFFCleanup(pie->tif);
//
//	/* Stuff the image into the zip archive and close the file */
//	code = svg_add_tiff_image(pie);
//	if (code < 0)
//		goto exit;
//
//	/* Reset the brush type to solid */
//	svg_setstrokebrush((gx_device_svg *)(pie->dev), svg_solidbrush);
//	svg_setfillbrush((gx_device_svg *)(pie->dev), svg_solidbrush);
//
//	/* Add the image relationship */
//	code = svg_add_image_relationship(pie);
//
//exit:
//	if (pie->pcs != NULL)
//		rc_decrement(pie->pcs, "svg_image_end_image (pcs)");
//	if (pie->buffer != NULL)
//		gs_free_object(pie->memory, pie->buffer, "svg_image_end_image");
//	if (pie->devc_buffer != NULL)
//		gs_free_object(pie->memory, pie->devc_buffer, "svg_image_end_image");
//
//	/* ICC clean up */
//	if (pie->icc_link != NULL)
//		gsicc_release_link(pie->icc_link);

	return code;
}