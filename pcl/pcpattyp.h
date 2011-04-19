/* Portions Copyright (C) 2001 artofcode LLC.
   Portions Copyright (C) 1996, 2001 Artifex Software Inc.
   Portions Copyright (C) 1988, 2000 Aladdin Enterprises.
   This software is based in part on the work of the Independent JPEG Group.
   All Rights Reserved.

   This software is distributed under license and may not be copied, modified
   or distributed except as expressly authorized under the terms of that
   license.  Refer to licensing information at http://www.artifex.com/ or
   contact Artifex Software, Inc., 101 Lucas Valley Road #110,
   San Rafael, CA  94903, (415)492-9861, for further information. */
/*$Id$ */

/* pcpattyp.h - pattern type enumerations and related information */

#ifndef pcpattyp_INCLUDED
#define pcpattyp_INCLUDED

/*
 * Pattern source identifiers. There are three of these, one for PCL and two
 * for GL (one for FT, the other for SV). Though GL types are not usually
 * defined in PCL files, they are in this case so that the pattern code
 * can be kept together.
 */
typedef enum {
    pcl_pattern_solid_frgrnd = 0,   /* solid foreground / current pen */
    pcl_pattern_solid_white,
    pcl_pattern_shading,
    pcl_pattern_cross_hatch,
    pcl_pattern_user_defined,
    pcl_pattern_current_pattern,   /* for rectangle fill only */
    pcl_pattern_raster_cspace      /* internal - used for rasters only */
} pcl_pattern_source_t;

typedef enum {
    hpgl_FT_pattern_solid_pen1 = 1,
    hpgl_FT_pattern_solid_pen2 = 2,
    hpgl_FT_pattern_one_line = 3,
    hpgl_FT_pattern_two_lines = 4,
    hpgl_FT_pattern_shading = 10,
    hpgl_FT_pattern_RF = 11,
    hpgl_FT_pattern_cross_hatch = 21,
    hpgl_FT_pattern_user_defined = 22
} hpgl_FT_pattern_source_t;

typedef enum {
    hpgl_SV_pattern_solid_pen = 0,
    hpgl_SV_pattern_shade = 1,
    hpgl_SV_pattern_RF = 2,
    hpgl_SV_pattern_cross_hatch = 21,
    hpgl_SV_pattern_user_defined = 22
} hpgl_SV_pattern_source_t;

/*
 * Opaque definitions of palettes, foregrounds, client colors, halftones,
 * and backgrounds.
 */
#ifndef pcl_palette_DEFINED
#define pcl_palette_DEFINED
typedef struct pcl_palette_s    pcl_palette_t;
#endif

#ifndef pcl_frgrnd_DEFINED
#define pcl_frgrnd_DEFINED
typedef struct pcl_frgrnd_s     pcl_frgrnd_t;
#endif

#ifndef pcl_ccolor_DEFINED
#define pcl_ccolor_DEFINED
typedef struct pcl_ccolor_s     pcl_ccolor_t;
#endif

#ifndef pcl_ht_DEFINED
#define pcl_ht_DEFINED
typedef struct pcl_ht_s         pcl_ht_t;
#endif

#ifndef pcl_crd_DEFINED
#define pcl_crd_DEFINED
typedef struct pcl_crd_s        pcl_crd_t;
#endif

/*
 * Structure to track what has been installed in the graphic state. This is
 * used to avoid unnecessary re-installation, and to avoid memory handling
 * problems that would arise if various objects were released while the
 * graphic state still retained pointes to them.
 *
 * The PCL client color structure (pcl_ccolor_t) incorporates both color and
 * color space information.
 */
typedef struct pcl_gstate_ids_s {
    struct pcl_gstate_ids_s *   prev;       /* stack back pointer */
    pcl_ccolor_t *              pccolor;
    pcl_ht_t *                  pht;
    pcl_crd_t *                 pcrd;
} pcl_gstate_ids_t;

#define private_st_gstate_ids_t()   /* in pcdraw.c */   \
    gs_private_st_ptrs4( st_gstate_ids_t,               \
                         pcl_gstate_ids_t,              \
                         "PCL graphics state tracker",  \
                         gstate_ids_enum_ptrs,          \
                         gstate_ids_reloc_ptrs,         \
                         prev,                          \
                         pccolor,                       \
                         pht,                           \
                         pcrd                           \
                         )

#endif			/* pcpattyp_INCLUDED */
