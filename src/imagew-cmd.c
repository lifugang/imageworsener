// imagew.c
// Part of ImageWorsener, Copyright (c) 2011 by Jason Summers.
// For more information, see the readme.txt file.

// This file implements a command-line application, and is not
// part of the ImageWorsener library.

// Note that applications that are not distributed with ImageWorsener are
// not expected to include imagew-config.h.
#include "imagew-config.h"

#ifdef IW_WINDOWS
#define IW_NO_LOCALE
#endif

#ifdef IW_WINDOWS
#include <tchar.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef IW_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

#ifndef IW_NO_LOCALE
#include <locale.h>
#include <langinfo.h>
#endif

#include "imagew.h"

#define IWCMD_FMT_UNKNOWN 0
#define IWCMD_FMT_PNG  1
#define IWCMD_FMT_JPEG 2
#define IWCMD_FMT_BMP  3

struct rgb_color {
	double r,g,b;
};

struct resize_alg {
	int family;
	double blur;
	int lobes;
	double b,c;
};

struct params_struct {
	const TCHAR *infn;
	const TCHAR *outfn;
	int quiet;
	int new_width;
	int new_height;
	struct resize_alg resize_alg_x;
	struct resize_alg resize_alg_y;
	struct resize_alg resize_alg_alpha;
	int bestfit;
	int depth;
	int grayscale, condgrayscale;
	double offset_r_h, offset_g_h, offset_b_h;
	double offset_r_v, offset_g_v, offset_b_v;
	int dither_type_all, dither_type_nonalpha, dither_type_alpha;
	int dither_type_red, dither_type_green, dither_type_blue, dither_type_gray;
	int color_count_all, color_count_nonalpha, color_count_alpha;
	int color_count_red, color_count_green, color_count_blue, color_count_gray;
	int apply_bkgd;
	int bkgd_checkerboard;
	int bkgd_check_size;
	int bkgd_check_origin_x, bkgd_check_origin_y;
	int use_crop, crop_x, crop_y, crop_w, crop_h;
	struct rgb_color bkgd;
	struct rgb_color bkgd2;
	int jpeg_quality;
	int jpeg_samp_factor_h, jpeg_samp_factor_v;
	int pngcmprlevel;
	int interlace;
	int randomize;
	int random_seed;
	int infmt;
	int outfmt;
	int no_gamma;
	int intclamp;
	int edge_policy;
	int grayscale_formula;
	int no_cslabel;
	int no_binarytrns;
	int cs_in_set, cs_out_set;
	struct iw_csdescr cs_in;
	struct iw_csdescr cs_out;
	int unicode_output;
	const TCHAR *symbol_arrow;
	const TCHAR *symbol_times;
	const TCHAR *symbol_ldquo, *symbol_rdquo;
	int density_code;
	double xdens, ydens;
};

static void my_warning_handler(struct iw_context *ctx, const TCHAR *msg)
{
	struct params_struct *p;
	p = (struct params_struct *)iw_get_userdata(ctx);
	if(!p->quiet) {
		_tprintf(_T("Warning: %s\n"),msg);
	}
}

static int get_fmt_from_name(const TCHAR *s)
{
	if(!_tcscmp(s,_T("png"))) return IWCMD_FMT_PNG;
	if(!_tcscmp(s,_T("jpg"))) return IWCMD_FMT_JPEG;
	if(!_tcscmp(s,_T("jpeg"))) return IWCMD_FMT_JPEG;
	if(!_tcscmp(s,_T("bmp"))) return IWCMD_FMT_BMP;
	return IWCMD_FMT_UNKNOWN;
}

static int detect_fmt_from_filename(const TCHAR *fn)
{
	TCHAR *s;
	s=_tcsrchr(fn,'.');
	if(s) {
		if(s[1]=='j' || s[1]=='J') return IWCMD_FMT_JPEG;
		if(s[1]=='b' || s[1]=='B') return IWCMD_FMT_BMP;
	}
	return IWCMD_FMT_PNG;
}

// Updates p->new_width and p->new_height
static void do_bestfit(struct params_struct *p, int old_width, int old_height)
{
	int x;
	double exp_factor;

	// If we fit-width, what would the height be?
	exp_factor = ((double)p->new_width) / old_width;
	exp_factor *= (p->xdens/p->ydens);
	x = (int)(0.5+ ((double)old_height) * exp_factor);
	if(x<=p->new_height) {
		// It fits. Use it.
		p->new_height = x;
		goto done;
	}
	
	// Fit to height instead.
	exp_factor = ((double)p->new_height) / old_height;
	exp_factor *= (p->ydens/p->xdens);
	x = (int)(0.5+ ((double)old_width) * exp_factor);
	if(x<p->new_width) {
		p->new_width = x;
	}

done:
	if(p->new_width<1) p->new_width=1;
	if(p->new_height<1) p->new_height=1;
}

static void iwcmd_set_resize(struct iw_context *ctx, int channel, int dimension, struct resize_alg *alg)
{
	switch(alg->family) {
	case IW_RESIZETYPE_CUBIC:
		iw_set_resize_alg(ctx,channel,dimension,alg->family,alg->blur,alg->b,alg->c);
		break;
	case IW_RESIZETYPE_LANCZOS: case IW_RESIZETYPE_HANNING:
	case IW_RESIZETYPE_BLACKMAN:
		iw_set_resize_alg(ctx,channel,dimension,alg->family,alg->blur,alg->lobes,0.0);
		break;
	default:
		iw_set_resize_alg(ctx,channel,dimension,alg->family,alg->blur,0.0,0.0);
	}
}

static int run(struct params_struct *p)
{
	int retval = 0;
	struct iw_context *ctx = NULL;
	int imgtype_read;
	int input_depth;
	int output_depth;
	int old_width,old_height;
	TCHAR errmsg[200];

	if(!p->quiet) _tprintf(_T("%s %s %s\n"),p->infn,p->symbol_arrow,p->outfn);

	ctx = iw_create_context();
	if(!ctx) goto done;

	iw_set_userdata(ctx,(void*)p);
	iw_set_warning_fn(ctx,my_warning_handler);
	if(p->unicode_output)
		iw_set_value(ctx,IW_VAL_CHARSET,1);

	if(p->random_seed!=0 || p->randomize) {
		iw_set_random_seed(ctx,p->randomize, p->random_seed);
	}

	if(p->no_gamma) iw_set_value(ctx,IW_VAL_DISABLE_GAMMA,1);
	if(p->intclamp) iw_set_value(ctx,IW_VAL_INT_CLAMP,1);
	if(p->no_cslabel) iw_set_value(ctx,IW_VAL_NO_CSLABEL,1);
	if(p->no_binarytrns) iw_set_value(ctx,IW_VAL_NO_BINARYTRNS,1);
	if(p->edge_policy>=0) iw_set_value(ctx,IW_VAL_EDGE_POLICY,p->edge_policy);
	if(p->grayscale_formula>0) iw_set_value(ctx,IW_VAL_GRAYSCALE_FORMULA,p->grayscale_formula);

	if(p->infmt==IWCMD_FMT_UNKNOWN)
		p->infmt=detect_fmt_from_filename(p->infn);

	if(p->infmt==IWCMD_FMT_JPEG) {
		if(!iw_read_jpeg_file(ctx,p->infn)) goto done;
	}
	else {
		if(!iw_read_png_file(ctx,p->infn)) goto done;
	}

	imgtype_read = iw_get_value(ctx,IW_VAL_INPUT_IMAGE_TYPE);
	input_depth = iw_get_value(ctx,IW_VAL_INPUT_DEPTH);
	output_depth = input_depth;

	if(p->outfmt==IWCMD_FMT_UNKNOWN)
		p->outfmt=detect_fmt_from_filename(p->outfn);

	// We have to tell the library the output format, so it can know what
	// kinds of images are allowed (e.g. whether transparency is allowed).
	if(p->outfmt==IWCMD_FMT_JPEG) {
		iw_set_output_profile(ctx,IW_PROFILE_JPEG);
	}
	else if(p->outfmt==IWCMD_FMT_BMP) {
		iw_set_output_profile(ctx,IW_PROFILE_BMP);
	}
	else {
		iw_set_output_profile(ctx,IW_PROFILE_PNG);
	}

	if(p->depth != -1) {
		output_depth = p->depth;
		iw_set_output_depth(ctx,output_depth);
	}

	if(p->cs_in_set) {
		iw_set_input_colorspace(ctx,&p->cs_in);
		// The default output colorspace is normally derived from the input
		// file's colorspace. If the caller wants to pretend the input file
		// is in a different colorspace, then to be consistent we also
		// use it for the default output colorspace.
		iw_set_output_colorspace(ctx,&p->cs_in,0);
	}
	if(p->cs_out_set) {
		iw_set_output_colorspace(ctx,&p->cs_out,1);
	}

	if(p->resize_alg_x.family) {
		iwcmd_set_resize(ctx,IW_CHANNELTYPE_ALL,IW_DIMENSION_H,&p->resize_alg_x);
	}
	if(p->resize_alg_y.family) {
		iwcmd_set_resize(ctx,IW_CHANNELTYPE_ALL,IW_DIMENSION_V,&p->resize_alg_y);
	}
	if(p->resize_alg_alpha.family) {
		iwcmd_set_resize(ctx,IW_CHANNELTYPE_ALPHA,IW_DIMENSION_V,&p->resize_alg_alpha);
	}

	if(p->dither_type_all)   iw_set_dither_type(ctx,IW_CHANNELTYPE_ALL  ,p->dither_type_all);
	if(p->dither_type_nonalpha) iw_set_dither_type(ctx,IW_CHANNELTYPE_NONALPHA,p->dither_type_nonalpha);
	if(p->dither_type_red)   iw_set_dither_type(ctx,IW_CHANNELTYPE_RED  ,p->dither_type_red);
	if(p->dither_type_green) iw_set_dither_type(ctx,IW_CHANNELTYPE_GREEN,p->dither_type_green);
	if(p->dither_type_blue)  iw_set_dither_type(ctx,IW_CHANNELTYPE_BLUE ,p->dither_type_blue);
	if(p->dither_type_gray)  iw_set_dither_type(ctx,IW_CHANNELTYPE_GRAY ,p->dither_type_gray);
	if(p->dither_type_alpha) iw_set_dither_type(ctx,IW_CHANNELTYPE_ALPHA,p->dither_type_alpha);

	if(p->color_count_all) iw_set_color_count  (ctx,IW_CHANNELTYPE_ALL  ,p->color_count_all);
	if(p->color_count_nonalpha) iw_set_color_count(ctx,IW_CHANNELTYPE_NONALPHA,p->color_count_nonalpha);
	if(p->color_count_red)   iw_set_color_count(ctx,IW_CHANNELTYPE_RED  ,p->color_count_red);
	if(p->color_count_green) iw_set_color_count(ctx,IW_CHANNELTYPE_GREEN,p->color_count_green);
	if(p->color_count_blue)  iw_set_color_count(ctx,IW_CHANNELTYPE_BLUE ,p->color_count_blue);
	if(p->color_count_gray)  iw_set_color_count(ctx,IW_CHANNELTYPE_GRAY ,p->color_count_gray);
	if(p->color_count_alpha) iw_set_color_count(ctx,IW_CHANNELTYPE_ALPHA,p->color_count_alpha);

	if(p->grayscale) {
		iw_set_value(ctx,IW_VAL_CVT_TO_GRAYSCALE,1);
	}
	else if(p->condgrayscale) {
		if(iw_get_value(ctx,IW_VAL_INPUT_NATIVE_GRAYSCALE)) {
			iw_set_value(ctx,IW_VAL_CVT_TO_GRAYSCALE,1);
		}
	}

	if(p->offset_r_h!=0.0) iw_set_channel_offset(ctx,IW_CHANNELTYPE_RED,  IW_DIMENSION_H,p->offset_r_h);
	if(p->offset_g_h!=0.0) iw_set_channel_offset(ctx,IW_CHANNELTYPE_GREEN,IW_DIMENSION_H,p->offset_g_h);
	if(p->offset_b_h!=0.0) iw_set_channel_offset(ctx,IW_CHANNELTYPE_BLUE, IW_DIMENSION_H,p->offset_b_h);
	if(p->offset_r_v!=0.0) iw_set_channel_offset(ctx,IW_CHANNELTYPE_RED,  IW_DIMENSION_V,p->offset_r_v);
	if(p->offset_g_v!=0.0) iw_set_channel_offset(ctx,IW_CHANNELTYPE_GREEN,IW_DIMENSION_V,p->offset_g_v);
	if(p->offset_b_v!=0.0) iw_set_channel_offset(ctx,IW_CHANNELTYPE_BLUE, IW_DIMENSION_V,p->offset_b_v);

	if(p->apply_bkgd) {
		iw_set_applybkgd(ctx,IW_BKGDCOLORSPACE_SRGB,p->bkgd.r,p->bkgd.g,p->bkgd.b);
		if(p->bkgd_checkerboard) {
			iw_set_bkgd_checkerboard(ctx,p->bkgd_check_size,p->bkgd2.r,p->bkgd2.g,p->bkgd2.b);
			iw_set_bkgd_checkerboard_origin(ctx,p->bkgd_check_origin_x,p->bkgd_check_origin_y);
		}
	}

	iw_get_input_image_density(ctx,&p->xdens,&p->ydens,&p->density_code);

	old_width=iw_get_value(ctx,IW_VAL_INPUT_WIDTH);
	old_height=iw_get_value(ctx,IW_VAL_INPUT_HEIGHT);

	if(p->use_crop) {
		// If we're cropping, adjust some things so that "bestfit" works.
		if(p->crop_x<0) p->crop_x=0;
		if(p->crop_y<0) p->crop_y=0;
		if(p->crop_x>old_width-1) p->crop_x=old_width-1;
		if(p->crop_y>old_height-1) p->crop_y=old_height-1;
		if(p->crop_w<0 || p->crop_w>old_width-p->crop_x) p->crop_w=old_width-p->crop_x;
		if(p->crop_h<0 || p->crop_h>old_height-p->crop_y) p->crop_h=old_height-p->crop_y;
		if(p->crop_w<1) p->crop_w=1;
		if(p->crop_h<1) p->crop_h=1;

		old_width = p->crop_w;
		old_height = p->crop_h;
	}

	if(p->new_width<0) p->new_width = -1;
	if(p->new_height<0) p->new_height = -1;
	if(p->new_width==0) p->new_width = 1;
	if(p->new_height==0) p->new_height = 1;

	if(p->new_width == -1 && p->new_height == -1) {
		// Neither -width nor -height specified. Keep image the same size.
		p->new_width=old_width;
		p->new_height=old_height;
	}
	else if(p->new_height == -1) {
		// -width given but not -height. Fit to width.
		p->new_height=1000000;
		do_bestfit(p,old_width,old_height);
	}
	else if(p->new_width == -1) {
		// -height given but not -width. Fit to height.
		p->new_width=1000000;
		do_bestfit(p,old_width,old_height);
	}
	else if(p->bestfit) {
		// -width and -height and -bestfit all given. Best-fit into the given dimensions.
		do_bestfit(p,old_width,old_height);
	}
	else {
		// -width and -height given but not -bestfit. Use the exact dimensions given.
		;
	}

	if(p->new_width<1) p->new_width=1;
	if(p->new_height<1) p->new_height=1;

	if(p->quiet) {
		;
	}
	else if(p->new_width==old_width && p->new_height==old_height) {
		_tprintf(_T("Processing (%d%s%d)\n"),p->new_width,p->symbol_times,p->new_height);
	}
	else {
		_tprintf(_T("Resizing (%d%s%d) %s (%d%s%d)\n"),old_width,p->symbol_times,old_height,
			p->symbol_arrow,p->new_width,p->symbol_times,p->new_height);
	}

	iw_set_output_canvas_size(ctx,p->new_width,p->new_height);
	if(p->use_crop) {
		iw_set_input_crop(ctx,p->crop_x,p->crop_y,p->crop_w,p->crop_h);
	}

	if(!iw_process_image(ctx)) goto done;

	if(p->interlace) {
		iw_set_value(ctx,IW_VAL_OUTPUT_INTERLACED,1);
	}

	if(p->outfmt==IWCMD_FMT_JPEG) {
		if(p->jpeg_quality>0) iw_set_value(ctx,IW_VAL_JPEG_QUALITY,p->jpeg_quality);
		if(p->jpeg_samp_factor_h>0)
			iw_set_value(ctx,IW_VAL_JPEG_SAMP_FACTOR_H,p->jpeg_samp_factor_h);
		if(p->jpeg_samp_factor_v>0)
			iw_set_value(ctx,IW_VAL_JPEG_SAMP_FACTOR_V,p->jpeg_samp_factor_v);
		if(!iw_write_jpeg_file(ctx,p->outfn)) goto done;
	}
	else if(p->outfmt==IWCMD_FMT_BMP) {
		if(!iw_write_bmp_file(ctx,p->outfn)) goto done;
	}
	else {
		if(p->pngcmprlevel >= 0)
			iw_set_value(ctx,IW_VAL_PNG_CMPR_LEVEL,p->pngcmprlevel);
		if(!iw_write_png_file(ctx,p->outfn)) goto done;
	}

	retval = 1;

done:
	if(ctx) {
		if(iw_get_errorflag(ctx)) {
			_tprintf(_T("imagew error: %s\n"),iw_get_errormsg(ctx,errmsg,200));
		}
	}

	iw_destroy_context(ctx);
	return retval;
}

// Parse two integers separated by a comma.
static void iwcmd_parse_int_pair(const TCHAR *s, int *i1, int *i2)
{
	TCHAR *cpos;

	*i1 = _tstoi(s);
	*i2 = 0;
	cpos = _tcschr(s,',');
	if(!cpos) return;
	*i2 = _tstoi(cpos+1);
}

// Parse four integers separated by commas.
static void iwcmd_parse_int_4(const TCHAR *s, int *i1, int *i2, int *i3, int *i4)
{
	TCHAR *cpos;

	*i1 = _tstoi(s);
	*i2 = 0;
	*i3 = -1;
	*i4 = -1;
	cpos = _tcschr(s,',');
	if(!cpos) return;
	*i2 = _tstoi(cpos+1);

	cpos = _tcschr(cpos+1,',');
	if(!cpos) return;
	*i3 = _tstoi(cpos+1);

	cpos = _tcschr(cpos+1,',');
	if(!cpos) return;
	*i4 = _tstoi(cpos+1);
}

static int hexdigit_value(TCHAR d)
{
	if(d>='0' && d<='9') return ((int)d)-'0';
	if(d>='a' && d<='f') return ((int)d)+10-'a';
	if(d>='A' && d<='F') return ((int)d)+10-'A';
	return 0;
}

static double hexvalue1(TCHAR d1)
{
	return ((double)hexdigit_value(d1))/15.0;
}

static double hexvalue2(TCHAR d1, TCHAR d2)
{
	return ((double)(16*hexdigit_value(d1) + hexdigit_value(d2)))/255.0;
}

static double hexvalue4(TCHAR d1, TCHAR d2, TCHAR d3, TCHAR d4)
{
	return ((double)(4096*hexdigit_value(d1) + 256*hexdigit_value(d2) +
		16*hexdigit_value(d3) + hexdigit_value(d4)))/65535.0;
}

// Allowed formats: 3 hex digits, 6 hex digits, or 12 hex digits.
static void parse_bkgd_color(struct rgb_color *c, const TCHAR *s, size_t s_len)
{
	if(s_len==3) {
		c->r = hexvalue1(s[0]);
		c->g = hexvalue1(s[1]);
		c->b = hexvalue1(s[2]);
	}
	else if(s_len==6) {
		c->r = hexvalue2(s[0],s[1]);
		c->g = hexvalue2(s[2],s[3]);
		c->b = hexvalue2(s[4],s[5]);
	}
	else if(s_len==12) {
		c->r = hexvalue4(s[0],s[1],s[2] ,s[3]);
		c->g = hexvalue4(s[4],s[5],s[6] ,s[7]);
		c->b = hexvalue4(s[8],s[9],s[10],s[11]);
	}
	else {
		// Invalid color description.
		c->r = 1.0;
		c->g = 0.0;
		c->b = 1.0;
	}
}

// 's' is either a single color, or two colors separated with a comma.
static void parse_bkgd(struct params_struct *p, const TCHAR *s)
{
	TCHAR *cpos;
	cpos = _tcschr(s,',');
	if(!cpos) {
		// Just a single color
		parse_bkgd_color(&p->bkgd,s,_tcslen(s));
		return;
	}

	// Two colors
	p->bkgd_checkerboard=1;
	parse_bkgd_color(&p->bkgd,s,cpos-s);
	parse_bkgd_color(&p->bkgd2,cpos+1,_tcslen(cpos+1));
}

// Find where the "name" ends and the parameters (numbers) begin.
static int iwcmd_get_name_len(const TCHAR *s)
{
	int i;
	for(i=0;s[i];i++) {
		if(s[i]>='a' && s[i]<='z') continue;
		if(s[i]>='A' && s[i]<='Z') continue;
		return i;
	}
	return i;
}

static int iwcmd_string_to_resizetype(struct params_struct *p,
	const TCHAR *s, struct resize_alg *alg)
{
	int i;
	int len, namelen;
	double blur;
	struct resizetable_struct {
		const TCHAR *name;
		int resizetype;
	};
	static const struct resizetable_struct resizetable[] = {
		{_T("mix"),IW_RESIZETYPE_MIX},
		{_T("nearest"),IW_RESIZETYPE_NEAREST},
		{_T("point"),IW_RESIZETYPE_NEAREST},
		{_T("linear"),IW_RESIZETYPE_LINEAR},
		{_T("triangle"),IW_RESIZETYPE_LINEAR},
		{_T("quadratic"),IW_RESIZETYPE_QUADRATIC},
		{_T("hermite"),IW_RESIZETYPE_HERMITE},
		{_T("box"),IW_RESIZETYPE_BOX},
		{_T("gaussian"),IW_RESIZETYPE_GAUSSIAN},
		{_T("auto"),IW_RESIZETYPE_AUTO},
		{_T("null"),IW_RESIZETYPE_NULL},
		{NULL,0}
	};

	blur = alg->blur;
	memset(alg,0,sizeof(struct resize_alg));
	// Hack: The 'blur' should be really be part of the string, but for now
	// it is a separate parameter, so we must not modify it here.
	alg->blur = blur;

	for(i=0; resizetable[i].name!=NULL; i++) {
		if(!_tcscmp(s,resizetable[i].name)) {
			alg->family = resizetable[i].resizetype;
			return 1;
		}
	}

	len=(int)_tcslen(s);
	namelen=iwcmd_get_name_len(s);

	if(namelen==7 && !_tcsncmp(s,_T("lanczos"),namelen)) {
		if(len>namelen)
			alg->lobes = _tstoi(&s[namelen]);
		else
			alg->lobes = 3;
		alg->family = IW_RESIZETYPE_LANCZOS;
		return 1;
	}
	else if((namelen==4 && !_tcsncmp(s,_T("hann"),namelen)) ||
		    (namelen==7 && !_tcsncmp(s,_T("hanning"),namelen)) )
	{
		if(len>namelen)
			alg->lobes = _tstoi(&s[namelen]);
		else
			alg->lobes = 4;
		alg->family = IW_RESIZETYPE_HANNING;
		return 1;
	}
	else if(namelen==8 && !_tcsncmp(s,_T("blackman"),namelen)) {
		if(len>namelen)
			alg->lobes = _tstoi(&s[namelen]);
		else
			alg->lobes = 4;
		alg->family = IW_RESIZETYPE_BLACKMAN;
		return 1;
	}
	else if(!_tcscmp(s,_T("catrom"))) {
		alg->family = IW_RESIZETYPE_CUBIC;
		alg->b = 0.0; alg->c = 0.5;
		return 1;
	}
	else if(!_tcscmp(s,_T("mitchell"))) {
		alg->family = IW_RESIZETYPE_CUBIC;
		alg->b = 1.0/3; alg->c = 1.0/3;
		return 1;
	}
	else if(!_tcscmp(s,_T("bspline"))) {
		alg->family = IW_RESIZETYPE_CUBIC;
		alg->b = 1.0; alg->c = 0.0;
		return 1;
	}
	else if(namelen==5 && !_tcsncmp(s,_T("cubic"),namelen)) {
		// Format is "cubic<B>,<C>"
		TCHAR *cpos;
		if(len < namelen+3) goto done; // error
		cpos = _tcschr(s,',');
		if(!cpos) goto done;
		alg->b = _tstof(&s[namelen]);
		alg->c = _tstof(cpos+1);
		alg->family = IW_RESIZETYPE_CUBIC;
		return 1;
	}
	else if(namelen==4 && !_tcsncmp(s,_T("keys"),namelen)) {
		// Format is "keys<alpha>"
		if(len>namelen)
			alg->c = _tstof(&s[namelen]);
		else
			alg->c = 0.5;
		alg->b = 1.0-2.0*alg->c;
		alg->family = IW_RESIZETYPE_CUBIC;
		return 1;
	}

done:
	_tprintf(_T("Unknown resize type %s%s%s\n"),p->symbol_ldquo,s,p->symbol_rdquo);
	return -1;
}

static int iwcmd_string_to_dithertype(struct params_struct *p,const TCHAR *s)
{
	int i;
	struct dithertable_struct {
		const TCHAR *name;
		int dithertype;
	};
	static const struct dithertable_struct dithertable[] = {
		{_T("f"),IW_DITHERTYPE_FS},
		{_T("fs"),IW_DITHERTYPE_FS},
		{_T("o"),IW_DITHERTYPE_ORDERED},
		{_T("r"),IW_DITHERTYPE_RANDOM},
		{_T("r2"),IW_DITHERTYPE_RANDOM2},
		{_T("jjn"),IW_DITHERTYPE_JJN},
		{_T("stucki"),IW_DITHERTYPE_STUCKI},
		{_T("burkes"),IW_DITHERTYPE_BURKES},
		{_T("sierra"),IW_DITHERTYPE_SIERRA3},
		{_T("sierra3"),IW_DITHERTYPE_SIERRA3},
		{_T("sierra2"),IW_DITHERTYPE_SIERRA2},
		{_T("sierralite"),IW_DITHERTYPE_SIERRA42A},
		{_T("atkinson"),IW_DITHERTYPE_ATKINSON},
		{_T("none"),IW_DITHERTYPE_NONE} // This line must be last.
	};

	for(i=0; dithertable[i].dithertype!=IW_DITHERTYPE_NONE; i++) {
		if(!_tcscmp(s,dithertable[i].name))
			return dithertable[i].dithertype;
	}

	_tprintf(_T("Unknown dither type %s%s%s\n"),p->symbol_ldquo,s,p->symbol_rdquo);
	return -1;
}


static int iwcmd_string_to_colorspace(struct params_struct *p,
  struct iw_csdescr *cs, const TCHAR *s)
{
	int namelen;
	int len;

	len=(int)_tcslen(s);
	namelen = iwcmd_get_name_len(s);

	if(namelen==5 && len>5 && !_tcsncmp(s,_T("gamma"),namelen)) {
		cs->cstype=IW_CSTYPE_GAMMA;
		cs->gamma=_tstof(&s[namelen]);
		if(cs->gamma<0.1) cs->gamma=0.1;
		if(cs->gamma>10.0) cs->gamma=10.0;
	}
	else if(!_tcscmp(s,_T("linear"))) {
		cs->cstype=IW_CSTYPE_LINEAR;
	}
	else if(len>=4 && !_tcsncmp(s,_T("srgb"),4)) {
		cs->cstype=IW_CSTYPE_SRGB;
		switch(s[4]) {
			case 'p': cs->sRGB_intent=IW_sRGB_INTENT_PERCEPTUAL; break;
			case 'r': cs->sRGB_intent=IW_sRGB_INTENT_RELATIVE; break;
			case 's': cs->sRGB_intent=IW_sRGB_INTENT_SATURATION; break;
			case 'a': cs->sRGB_intent=IW_sRGB_INTENT_ABSOLUTE; break;
			default:  cs->sRGB_intent=IW_sRGB_INTENT_PERCEPTUAL; break;
		}
	}
	else {
		_tprintf(_T("Unknown color space %s%s%s\n"),p->symbol_ldquo,s,p->symbol_rdquo);
		return -1;
	}
	return 1;
}

static void usage_message(void)
{
	_tprintf(
		_T("Usage: imagew [-width <n>] [-height <n>] [options] <in-file> <out-file>\n")
		_T("Options include -filter, -grayscale, -depth, -cc, -dither, -bkgd, -cs,\n")
		_T(" -quiet, -version.\n")
		_T("See the readme.txt file for more information.\n")
	);
}

static void do_printversion(struct params_struct *p)
{
	TCHAR buf[200];
	int buflen, u;
	int ver;

	buflen = (int)(sizeof(buf)/sizeof(TCHAR));
	u = p->unicode_output?1:0;

	ver = iw_get_version_int();
	_tprintf(_T("ImageWorsener version %s (%d-bit)\n"),
		iw_get_version_string(buf,buflen,u),
		(int)(8*sizeof(void*)) );

	_tprintf(_T("%s\n"),iw_get_copyright_string(buf,buflen,u));

	_tprintf(_T("Uses libjpeg version %s\n"),iw_get_libjpeg_version_string(buf,buflen,u));
	_tprintf(_T("Uses libpng version %s\n"),iw_get_libpng_version_string(buf,buflen,u));
	_tprintf(_T("Uses zlib version %s\n"),iw_get_zlib_version_string(buf,buflen,u));
}

static void iwcmd_init_characters(struct params_struct *p)
{
	if(p->unicode_output) {
#ifdef _UNICODE
		// UTF-16
		p->symbol_arrow = _T("\x2192");
		p->symbol_times = _T("\xd7");
		p->symbol_ldquo = _T("\x201c");
		p->symbol_rdquo = _T("\x201d");
#else
		// UTF-8
		p->symbol_arrow = _T("\xe2\x86\x92");
		p->symbol_times = _T("\xc3\x97");
		p->symbol_ldquo = _T("\xe2\x80\x9c");
		p->symbol_rdquo = _T("\xe2\x80\x9d");
#endif
	}
	else {
		p->symbol_arrow = _T("->");
		p->symbol_times = _T("x");
		p->symbol_ldquo = _T("\"");
		p->symbol_rdquo = _T("\"");
	}
}

enum iwcmd_param_types {
 PT_NONE=0, PT_WIDTH, PT_HEIGHT, PT_DEPTH, PT_INPUTCS, PT_CS,
 PT_RESIZETYPE, PT_RESIZETYPE_X, PT_RESIZETYPE_Y, PT_RESIZETYPE_ALPHA,
 PT_BLUR_FACTOR, PT_BLUR_FACTOR_X, PT_BLUR_FACTOR_Y, PT_BLUR_FACTOR_ALPHA,
 PT_DITHER, PT_DITHERCOLOR, PT_DITHERALPHA, PT_DITHERRED, PT_DITHERGREEN, PT_DITHERBLUE, PT_DITHERGRAY,
 PT_CC, PT_CCCOLOR, PT_CCALPHA, PT_CCRED, PT_CCGREEN, PT_CCBLUE, PT_CCGRAY,
 PT_BKGD, PT_BKGD2, PT_CHECKERSIZE, PT_CHECKERORG, PT_CROP,
 PT_OFFSET_R_H, PT_OFFSET_G_H, PT_OFFSET_B_H, PT_OFFSET_R_V, PT_OFFSET_G_V,
 PT_OFFSET_B_V, PT_OFFSET_RB_H, PT_OFFSET_RB_V,
 PT_JPEGQUALITY, PT_JPEGSAMPLING, PT_PNGCMPRLEVEL, PT_INTERLACE,
 PT_RANDSEED, PT_INFMT, PT_OUTFMT, PT_EDGE_POLICY, PT_GRAYSCALEFORMULA,
 PT_BESTFIT, PT_NOBESTFIT, PT_GRAYSCALE, PT_CONDGRAYSCALE, PT_NOGAMMA,
 PT_INTCLAMP, PT_NOCSLABEL, PT_NOBINARYTRNS,
 PT_QUIET, PT_VERSION, PT_HELP
};

struct parsestate_struct {
	enum iwcmd_param_types param_type;
	int untagged_param_count;
	int printversion;
	int showhelp;
};

static int process_option_name(struct params_struct *p, struct parsestate_struct *ps, const TCHAR *n)
{
	struct opt_struct {
		const TCHAR *name;
		enum iwcmd_param_types code;
		int has_param;
	};
	static const struct opt_struct opt_info[] = {
		{_T("width"),PT_WIDTH,1},
		{_T("height"),PT_HEIGHT,1},
		{_T("depth"),PT_DEPTH,1},
		{_T("inputcs"),PT_INPUTCS,1},
		{_T("cs"),PT_CS,1},
		{_T("filter"),PT_RESIZETYPE,1},
		{_T("filterx"),PT_RESIZETYPE_X,1},
		{_T("filtery"),PT_RESIZETYPE_Y,1},
		{_T("filteralpha"),PT_RESIZETYPE_ALPHA,1},
		{_T("blur"),PT_BLUR_FACTOR,1},
		{_T("blurx"),PT_BLUR_FACTOR_X,1},
		{_T("blury"),PT_BLUR_FACTOR_Y,1},
		{_T("bluralpha"),PT_BLUR_FACTOR_ALPHA,1},
		{_T("dither"),PT_DITHER,1},
		{_T("dithercolor"),PT_DITHERCOLOR,1},
		{_T("ditheralpha"),PT_DITHERALPHA,1},
		{_T("ditherred"),PT_DITHERRED,1},
		{_T("dithergreen"),PT_DITHERGREEN,1},
		{_T("ditherblue"),PT_DITHERBLUE,1},
		{_T("dithergray"),PT_DITHERGRAY,1},
		{_T("cc"),PT_CC,1},
		{_T("cccolor"),PT_CCCOLOR,1},
		{_T("ccalpha"),PT_CCALPHA,1},
		{_T("ccred"),PT_CCRED,1},
		{_T("ccgreen"),PT_CCGREEN,1},
		{_T("ccblue"),PT_CCBLUE,1},
		{_T("ccgray"),PT_CCGRAY,1},
		{_T("bkgd"),PT_BKGD,1},
		{_T("checkersize"),PT_CHECKERSIZE,1},
		{_T("checkerorigin"),PT_CHECKERORG,1},
		{_T("crop"),PT_CROP,1},
		{_T("offsetred"),PT_OFFSET_R_H,1},
		{_T("offsetgreen"),PT_OFFSET_G_H,1},
		{_T("offsetblue"),PT_OFFSET_B_H,1},
		{_T("offsetrb"),PT_OFFSET_RB_H,1},
		{_T("offsetvred"),PT_OFFSET_R_V,1},
		{_T("offsetvgreen"),PT_OFFSET_G_V,1},
		{_T("offsetvblue"),PT_OFFSET_B_V,1},
		{_T("offsetvrb"),PT_OFFSET_RB_V,1},
		{_T("jpegquality"),PT_JPEGQUALITY,1},
		{_T("jpegsampling"),PT_JPEGSAMPLING,1},
		{_T("pngcmprlevel"),PT_PNGCMPRLEVEL,1},
		{_T("randseed"),PT_RANDSEED,1},
		{_T("infmt"),PT_INFMT,1},
		{_T("outfmt"),PT_OUTFMT,1},
		{_T("edge"),PT_EDGE_POLICY,1},
		{_T("grayscaleformula"),PT_GRAYSCALEFORMULA,1},
		{_T("interlace"),PT_INTERLACE,0},
		{_T("bestfit"),PT_BESTFIT,0},
		{_T("nobestfit"),PT_NOBESTFIT,0},
		{_T("grayscale"),PT_GRAYSCALE,0},
		{_T("condgrayscale"),PT_CONDGRAYSCALE,0},
		{_T("nogamma"),PT_NOGAMMA,0},
		{_T("intclamp"),PT_INTCLAMP,0},
		{_T("nocslabel"),PT_NOCSLABEL,0},
		{_T("nobinarytrns"),PT_NOBINARYTRNS,0},
		{_T("quiet"),PT_QUIET,0},
		{_T("version"),PT_VERSION,0},
		{_T("help"),PT_HELP,0},
		{NULL,PT_NONE,0}
	};
	enum iwcmd_param_types pt;
	int i;

	pt=PT_NONE;

	// Search for the option name.
	for(i=0;opt_info[i].name;i++) {
		if(!_tcscmp(n,opt_info[i].name)) {
			if(opt_info[i].has_param) {
				// Found option with a parameter. Record it and return.
				ps->param_type=opt_info[i].code;
				return 1;
			}
			// Found parameterless option.
			pt=opt_info[i].code;
			break;
		}
	}

	// Handle parameterless options.
	switch(pt) {
	case PT_BESTFIT:
		p->bestfit=1;
		break;
	case PT_NOBESTFIT:
		p->bestfit=0;
		break;
	case PT_GRAYSCALE:
		p->grayscale=1;
		break;
	case PT_CONDGRAYSCALE:
		p->condgrayscale=1;
		break;
	case PT_NOGAMMA:
		p->no_gamma=1;
		break;
	case PT_INTCLAMP:
		p->intclamp=1;
		break;
	case PT_NOCSLABEL:
		p->no_cslabel=1;
		break;
	case PT_NOBINARYTRNS:
		p->no_binarytrns=1;
		break;
	case PT_INTERLACE:
		p->interlace=1;
		break;
	case PT_QUIET:
		p->quiet=1;
		break;
	case PT_VERSION:
		ps->printversion=1;
		break;
	case PT_HELP:
		ps->showhelp=1;
		break;
	default:
		_tprintf(_T("Unknown option %s%s%s.\n"),p->symbol_ldquo,n,p->symbol_rdquo);
		return 0;
	}

	return 1;
}

static int process_option_arg(struct params_struct *p, struct parsestate_struct *ps, const TCHAR *v)
{
	int ret;

	switch(ps->param_type) {
	case PT_WIDTH:
		p->new_width=_tstoi(v);
		break;
	case PT_HEIGHT:
		p->new_height=_tstoi(v);
		break;
	case PT_DEPTH:
		p->depth=_tstoi(v);
		break;
	case PT_INPUTCS:
		ret=iwcmd_string_to_colorspace(p,&p->cs_in,v);
		if(ret<0) return 0;
		p->cs_in_set=1;
		break;
	case PT_CS:
		ret=iwcmd_string_to_colorspace(p,&p->cs_out,v);
		if(ret<0) return 0;
		p->cs_out_set=1;
		break;
	case PT_RESIZETYPE:
		ret=iwcmd_string_to_resizetype(p,v,&p->resize_alg_x);
		if(ret<0) return 0;
		p->resize_alg_y=p->resize_alg_x;
		break;
	case PT_RESIZETYPE_X:
		ret=iwcmd_string_to_resizetype(p,v,&p->resize_alg_x);
		if(ret<0) return 0;
		break;
	case PT_RESIZETYPE_Y:
		ret=iwcmd_string_to_resizetype(p,v,&p->resize_alg_y);
		if(ret<0) return 0;
		break;
	case PT_RESIZETYPE_ALPHA:
		ret=iwcmd_string_to_resizetype(p,v,&p->resize_alg_alpha);
		if(ret<0) return 0;
		break;
	case PT_BLUR_FACTOR:
		p->resize_alg_x.blur = p->resize_alg_y.blur = _tstof(v);
		break;
	case PT_BLUR_FACTOR_X:
		p->resize_alg_x.blur = _tstof(v);
		break;
	case PT_BLUR_FACTOR_Y:
		p->resize_alg_y.blur = _tstof(v);
		break;
	case PT_BLUR_FACTOR_ALPHA:
		p->resize_alg_alpha.blur = _tstof(v);
		break;
	case PT_DITHER:
		p->dither_type_all=iwcmd_string_to_dithertype(p,v);
		if(p->dither_type_all<0) return 0;
		break;
	case PT_DITHERCOLOR:
		p->dither_type_nonalpha=iwcmd_string_to_dithertype(p,v);
		if(p->dither_type_nonalpha<0) return 0;
		break;
	case PT_DITHERALPHA:
		p->dither_type_alpha=iwcmd_string_to_dithertype(p,v);
		if(p->dither_type_alpha<0) return 0;
		break;
	case PT_DITHERRED:
		p->dither_type_red=iwcmd_string_to_dithertype(p,v);
		if(p->dither_type_red<0) return 0;
		break;
	case PT_DITHERGREEN:
		p->dither_type_green=iwcmd_string_to_dithertype(p,v);
		if(p->dither_type_green<0) return 0;
		break;
	case PT_DITHERBLUE:
		p->dither_type_blue=iwcmd_string_to_dithertype(p,v);
		if(p->dither_type_blue<0) return 0;
		break;
	case PT_DITHERGRAY:
		p->dither_type_gray=iwcmd_string_to_dithertype(p,v);
		if(p->dither_type_gray<0) return 0;
		break;
	case PT_CC:
		p->color_count_all=_tstoi(v);
		break;
	case PT_CCCOLOR:
		p->color_count_nonalpha=_tstoi(v);
		break;
	case PT_CCALPHA:
		p->color_count_alpha=_tstoi(v);
		break;
	case PT_BKGD:
		p->apply_bkgd=1;
		parse_bkgd(p,v);
		break;
	case PT_CHECKERSIZE:
		p->bkgd_check_size=_tstoi(v);
		break;
	case PT_CHECKERORG:
		iwcmd_parse_int_pair(v,&p->bkgd_check_origin_x,&p->bkgd_check_origin_y);
		break;
	case PT_CROP:
		iwcmd_parse_int_4(v,&p->crop_x,&p->crop_y,&p->crop_w,&p->crop_h);
		p->use_crop=1;
		break;
	case PT_CCRED:
		p->color_count_red=_tstoi(v);
		break;
	case PT_CCGREEN:
		p->color_count_green=_tstoi(v);
		break;
	case PT_CCBLUE:
		p->color_count_blue=_tstoi(v);
		break;
	case PT_CCGRAY:
		p->color_count_gray=_tstoi(v);
		break;
	case PT_OFFSET_R_H:
		p->offset_r_h=_tstof(v);
		break;
	case PT_OFFSET_G_H:
		p->offset_g_h=_tstof(v);
		break;
	case PT_OFFSET_B_H:
		p->offset_b_h=_tstof(v);
		break;
	case PT_OFFSET_R_V:
		p->offset_r_v=_tstof(v);
		break;
	case PT_OFFSET_G_V:
		p->offset_g_v=_tstof(v);
		break;
	case PT_OFFSET_B_V:
		p->offset_b_v=_tstof(v);
		break;
	case PT_OFFSET_RB_H:
		// Shortcut for shifting red and blue in opposite directions.
		p->offset_r_h=_tstof(v);
		p->offset_b_h= -p->offset_r_h;
		break;
	case PT_OFFSET_RB_V:
		// Shortcut for shifting red and blue vertically in opposite directions.
		p->offset_r_v=_tstof(v);
		p->offset_b_v= -p->offset_r_v;
		break;
	case PT_JPEGQUALITY:
		p->jpeg_quality=_tstoi(v);
		break;
	case PT_JPEGSAMPLING:
		iwcmd_parse_int_pair(v,&p->jpeg_samp_factor_h,&p->jpeg_samp_factor_v);
		break;
	case PT_PNGCMPRLEVEL:
		p->pngcmprlevel=_tstoi(v);
		break;
	case PT_RANDSEED:
		if(v[0]=='r') {
			p->randomize = 1;
		}
		else {
			p->random_seed=_tstoi(v);
		}
		break;
	case PT_INFMT:
		p->infmt=get_fmt_from_name(v);
		break;
	case PT_OUTFMT:
		p->outfmt=get_fmt_from_name(v);
		break;
	case PT_EDGE_POLICY:
		if(v[0]=='s') p->edge_policy=IW_EDGE_POLICY_STANDARD;
		else if(v[0]=='r') p->edge_policy=IW_EDGE_POLICY_REPLICATE;
		else {
			_tprintf(_T("Unknown edge policy\n"));
			return 0;
		}
		break;
	case PT_GRAYSCALEFORMULA:
		if(v[0]=='s') p->grayscale_formula=0;
		else if(v[0]=='c') p->grayscale_formula=1;
		else {
			_tprintf(_T("Unknown grayscale formula\n"));
			return 0;
		}
		break;

	case PT_NONE:
		// This is presumably the input or output filename.

		if(ps->untagged_param_count==0) {
			p->infn = v;
		}
		else if(ps->untagged_param_count==1) {
			p->outfn = v;
		}
		ps->untagged_param_count++;
		break;

	default:
		_tprintf(_T("Internal error: unhandled param\n"));
		return 0;
	}

	return 1;
}

int _tmain(int argc, TCHAR* argv[])
{
	struct params_struct p;
	struct parsestate_struct ps;
	int ret;
	int i;
	int unicode_output=0;
	const TCHAR *optname;

	memset(&ps,0,sizeof(struct parsestate_struct));
	ps.param_type=PT_NONE;
	ps.untagged_param_count=0;
	ps.printversion=0;
	ps.showhelp=0;

#ifdef _UNICODE
	unicode_output=1;
	_setmode(_fileno(stdout),_O_U16TEXT);
#endif
#ifndef IW_NO_LOCALE
	setlocale(LC_CTYPE,"");
	unicode_output = (strcmp(nl_langinfo(CODESET), "UTF-8") == 0);
#endif

	memset(&p,0,sizeof(struct params_struct));
	p.new_width = -1;
	p.new_height = -1;
	p.depth = -1;
	p.edge_policy = -1;
	p.bkgd_check_size = 16;
	p.bestfit = 0;
	p.offset_r_h=0.0; p.offset_g_h=0.0; p.offset_b_h=0.0;
	p.offset_r_v=0.0; p.offset_g_v=0.0; p.offset_b_v=0.0;
	p.infmt=IWCMD_FMT_UNKNOWN;
	p.outfmt=IWCMD_FMT_UNKNOWN;
	p.unicode_output=unicode_output;
	p.resize_alg_x.blur = 1.0;
	p.resize_alg_y.blur = 1.0;
	p.resize_alg_alpha.blur = 1.0;
	p.pngcmprlevel = -1;

	iwcmd_init_characters(&p);

	for(i=1;i<argc;i++) {
		if(argv[i][0]=='-' && ps.param_type==PT_NONE) {
			optname = &argv[i][1];
			// If the second char is also a '-', ignore it.
			if(argv[i][1]=='-')
				optname = &argv[i][2];
			if(!process_option_name(&p, &ps, optname)) {
				return 1;
			}
		}
		else {
			// Process a parameter of the previous option.

			if(!process_option_arg(&p, &ps, argv[i])) {
				return 1;
			}

			ps.param_type = PT_NONE;
		}
	}

	if(ps.showhelp) {
		usage_message();
		return 0;
	}

	if(ps.printversion) {
		do_printversion(&p);
		return 0;
	}

	if(ps.untagged_param_count!=2 || ps.param_type!=PT_NONE) {
		usage_message();
		return 1;
	}

	ret=run(&p);
	return ret?0:1;
}
