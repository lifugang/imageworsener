// imagew-gif.c
// Part of ImageWorsener, Copyright (c) 2011 by Jason Summers.
// For more information, see the readme.txt file.

// This is a self-contained GIF image decoder.
// It supports a single image only, so it does not support animated GIFs,
// or any GIF where the main image is constructed from multiple sub-images.

#include "imagew-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IW_INCLUDE_UTIL_FUNCTIONS
#include "imagew.h"

enum iwgif_string {
	iws_gif_read_error=1,
	iws_gif_unsupported,
	iws_gif_no_image,
	iws_gif_decode_error,
	iws_gif_inval_lzw_min,
	iws_gif_not_a_gif
};

struct iw_stringtableentry iwgif_stringtable[] = {
	{ iws_gif_read_error, "Failed to read GIF file" },
	{ iws_gif_unsupported, "Invalid or unsupported GIF file" },
	{ iws_gif_no_image, "No image in file" },
	{ iws_gif_decode_error, "GIF decoding error" },
	{ iws_gif_inval_lzw_min, "Invalid LZW minimum code size" },
	{ iws_gif_not_a_gif, "Not a GIF file" },
	{ 0, NULL }
};

static const char *iwgif_get_string(struct iw_context *ctx, int n)
{
	return iw_get_string(ctx,IW_STRINGTABLENUM_GIF,n);
}

struct iwgifreadcontext {
	struct iw_iodescr *iodescr;
	struct iw_context *ctx;
	struct iw_image *img;

	struct iw_csdescr csdescr;

	int screen_width, screen_height; // Same as .img->width, .img->height
	int image_width, image_height;
	int image_left, image_top;

	int screen_initialized;
	int interlaced;
	int bytes_per_pixel;
	int has_transparency;
	int has_bg_color;
	int bg_color_index;
	int trans_color_index;

	size_t pixels_set; // Number of pixels decoded so far
	size_t total_npixels; // Total number of pixels in the "image" (not the "screen")

	unsigned char **row_pointers;

	struct iw_palette colortable;

	// A buffer used when reading the GIF file.
	// The largest block we need to read is a 256-color palette.
	unsigned char rbuf[768];
};

static int iwgif_read(struct iwgifreadcontext *rctx,
		unsigned char *buf, size_t buflen)
{
	int ret;
	size_t bytesread = 0;

	ret = (*rctx->iodescr->read_fn)(rctx->ctx,rctx->iodescr,
		buf,buflen,&bytesread);
	if(!ret || bytesread!=buflen) {
		return 0;
	}
	return 1;
}

static int iw_read_uint16le(unsigned char *buf)
{
	return (int)buf[0] | (((int)buf[1])<<8);
}

static int iwgif_read_file_header(struct iwgifreadcontext *rctx)
{
	if(!iwgif_read(rctx,rctx->rbuf,6)) return 0;
	if(rctx->rbuf[0]!='G' || rctx->rbuf[1]!='I' || rctx->rbuf[2]!='F') {
		iw_seterror(rctx->ctx,iwgif_get_string(rctx->ctx,iws_gif_not_a_gif));
		return 0;
	}
	return 1;
}

static int iwgif_read_screen_descriptor(struct iwgifreadcontext *rctx)
{
	int has_global_ct;
	int global_ct_size;
	int aspect_ratio_code;

	// The screen descriptor is always 7 bytes in size.
	if(!iwgif_read(rctx,rctx->rbuf,7)) return 0;
	rctx->screen_width = iw_read_uint16le(&rctx->rbuf[0]);
	rctx->screen_height = iw_read_uint16le(&rctx->rbuf[2]);
	if(!iw_check_image_dimensons(rctx->ctx,rctx->screen_width,rctx->screen_height)) {
		return 0;
	}
	rctx->img->width = rctx->screen_width;
	rctx->img->height = rctx->screen_height;

	has_global_ct = (int)((rctx->rbuf[4]>>7)&0x01);

	if(has_global_ct) {
		global_ct_size = (int)(rctx->rbuf[4]&0x07);
		rctx->colortable.num_entries = 1<<(1+global_ct_size);
	}

	if(has_global_ct) {
		rctx->bg_color_index = (int)rctx->rbuf[5];
		if(rctx->bg_color_index < rctx->colortable.num_entries)
			rctx->has_bg_color = 1;
	}

	aspect_ratio_code = (int)rctx->rbuf[6];
	if(aspect_ratio_code!=0) {
		// [aspect ratio = (pixel width)/(pixel height)]
		rctx->img->density_code = IW_DENSITY_UNITS_UNKNOWN;
		rctx->img->density_x = 64000.0/(double)(aspect_ratio_code + 15);
		rctx->img->density_y = 1000.0;
	}

	return 1;
}

// ct->num_entries must be set by caller
// Reads the palette entries.
static int iwgif_read_color_table(struct iwgifreadcontext *rctx, struct iw_palette *ct)
{
	int i;
	if(ct->num_entries<1) return 1;

	if(!iwgif_read(rctx,rctx->rbuf,3*ct->num_entries)) return 0;
	for(i=0;i<ct->num_entries;i++) {
		ct->entry[i].r = rctx->rbuf[3*i+0];
		ct->entry[i].g = rctx->rbuf[3*i+1];
		ct->entry[i].b = rctx->rbuf[3*i+2];
	}
	return 1;
}

static int iwgif_skip_subblocks(struct iwgifreadcontext *rctx)
{
	unsigned char subblock_size;

	while(1) {
		// Read the subblock size
		if(!iwgif_read(rctx,rctx->rbuf,1)) return 0;

		subblock_size = rctx->rbuf[0];
		// A size of 0 marks the end of the subblocks.
		if(subblock_size==0) return 1;

		// Read the subblock's data
		if(!iwgif_read(rctx,rctx->rbuf,(size_t)subblock_size)) return 0;
	}
}

// We need transparency information, so we have to process graphics control
// extensions.
static int iwgif_read_graphics_control_ext(struct iwgifreadcontext *rctx)
{
	int retval;

	// Read 6 bytes:
	//  The first is the subblock size, which must be 4.
	//  The last is the block terminator.
	//  The middle 4 contain the actual data.
	if(!iwgif_read(rctx,rctx->rbuf,6)) goto done;

	if(rctx->rbuf[0]!=4) goto done;
	if(rctx->rbuf[5]!=0) goto done;
	rctx->has_transparency = (int)((rctx->rbuf[1])&0x01);
	if(rctx->has_transparency) {
		rctx->trans_color_index = (int)rctx->rbuf[4];
		rctx->colortable.entry[rctx->trans_color_index].a = 0;
	}

	retval=1;
done:
	return retval;
}


static int iwgif_read_extension(struct iwgifreadcontext *rctx)
{
	int retval=0;
	unsigned char ext_type;

	if(!iwgif_read(rctx,rctx->rbuf,1)) goto done;
	ext_type=rctx->rbuf[0];

	switch(ext_type) {
	case 0xf9:
		if(!iwgif_read_graphics_control_ext(rctx)) goto done;
		break;
	default:
		if(!iwgif_skip_subblocks(rctx)) goto done;
	}
	
	retval=1;
done:
	return retval;
}

// Sets the (rctx->pixels_set + offset)th pixel in the logical image to the
// color represented by palette entry #coloridx.
static void iwgif_record_pixel(struct iwgifreadcontext *rctx, unsigned int coloridx,
		int offset)
{
	struct iw_image *img;
	unsigned int r,g,b,a;
	size_t pixnum;
	size_t xi,yi; // position in image coordinates
	size_t xs,ys; // position in screen coordinates
	unsigned char *ptr;

	img = rctx->img;

	// Figure out which pixel to set.

	pixnum = rctx->pixels_set + offset;
	xi = pixnum%rctx->image_width;
	yi = pixnum/rctx->image_width;
	xs = rctx->image_left + xi;
	ys = rctx->image_top + yi;
	if(xs>=rctx->screen_width || ys>=rctx->screen_height) return;
	
	// Figure out what color to set it to.

	if(coloridx<(unsigned int)rctx->colortable.num_entries) {
		r=rctx->colortable.entry[coloridx].r;
		g=rctx->colortable.entry[coloridx].g;
		b=rctx->colortable.entry[coloridx].b;
		a=rctx->colortable.entry[coloridx].a;
	}
	else {
		return; // Illegal palette index
	}

	// Set the pixel.

	ptr = &rctx->row_pointers[yi][rctx->bytes_per_pixel*xi];
	ptr[0]=r; ptr[1]=g; ptr[2]=b;
	if(img->imgtype==IW_IMGTYPE_RGBA) {
		ptr[3]=a;
	}
}

////////////////////////////////////////////////////////
//                    LZW decoder
////////////////////////////////////////////////////////

struct lzw_tableentry {
	unsigned int reference; // pointer to previous table entry (if not a root code)
	unsigned int length;
	unsigned char value;
	unsigned char firstchar;
};

struct lzwdeccontext {
	unsigned int root_codesize;
	unsigned int current_codesize;
	int eoi_flag;
	unsigned int oldcode;
	unsigned int pending_code;
	unsigned int bits_in_pending_code;
	unsigned int num_root_codes;
	int ncodes_since_clear;

	unsigned int clear_code;
	unsigned int eoi_code;

	unsigned int ct_used; // Number of items used in the code table
	struct lzw_tableentry ct[4096]; // Code table
};

static void lzw_init(struct lzwdeccontext *d, unsigned int root_codesize)
{
	unsigned int i;

	memset(d,0,sizeof(struct lzwdeccontext));

	d->root_codesize = root_codesize;
	d->num_root_codes = 1<<d->root_codesize;
	d->clear_code = d->num_root_codes;
	d->eoi_code = d->num_root_codes+1;
	for(i=0;i<d->num_root_codes;i++) {
		d->ct[i].reference = 0;
		d->ct[i].value = (unsigned char)i;
		d->ct[i].firstchar = d->ct[i].value;
		d->ct[i].length = 0;
	}
}

static void lzw_clear(struct lzwdeccontext *d)
{
	d->ct_used = d->num_root_codes+2;
	d->current_codesize = d->root_codesize+1;
	d->ncodes_since_clear=0;
	d->oldcode=0;
}

// Decode an LZW code to one or more pixels, and record it in the image.
static void lzw_emit_code(struct iwgifreadcontext *rctx, struct lzwdeccontext *d,
		unsigned int first_code)
{
	unsigned int code;
	code = first_code;

	// An LZW code may decode to more than one pixel. Note that the pixels for
	// an LZW code are decoded in reverse order (right to left).

	while(1) {
		iwgif_record_pixel(rctx, d->ct[code].value, d->ct[code].length);
		if(d->ct[code].length<1) break;
		// The codes are structured as a "forest" (multiple trees).
		// Go to the parent code, which should have a length 1 less than this one.
		code = d->ct[code].reference;
	}

	// Track the total number of pixels decoded in this image.
	rctx->pixels_set += d->ct[first_code].length +1;
}

// Add a code to the dictionary.
// Returns the position where it was added.
// If table is full, returns -1.
static int lzw_add_to_dict(struct lzwdeccontext *d, int oldcode, unsigned char val)
{
	int newpos;

	if(d->ct_used>=4096) {
		return -1;
	}

	newpos = d->ct_used;
	d->ct_used++;

	d->ct[newpos].reference = oldcode;
	d->ct[newpos].value = val;
	d->ct[newpos].firstchar = d->ct[oldcode].firstchar;
	d->ct[newpos].length = d->ct[oldcode].length + 1;

	// If we've used the last code of this size, we need to increase the codesize.
	switch(newpos) {
	case 7: case 15: case 31: case 63: case 127: case 255:
	case 511: case 1023: case 2047:
		d->current_codesize++;
	}

	return newpos;
}

// Process a single LZW code that was read from the input stream.
static int lzw_process_code(struct iwgifreadcontext *rctx, struct lzwdeccontext *d,
		unsigned int code)
{
	int newpos;

	if(code==d->eoi_code) {
		d->eoi_flag=1;
		return 1;
	}

	if(code==d->clear_code) {
		lzw_clear(d);
		return 1;
	}

	d->ncodes_since_clear++;

	if(d->ncodes_since_clear==1) {
		// Special case for the first code.
		lzw_emit_code(rctx,d,code);
		d->oldcode = code;
		return 1;
	}

	// Is code in code table?
	if(code < d->ct_used) {
		// Yes, code is in table.
		lzw_emit_code(rctx,d,code);

		// Let k = the first character of the translation of the code.
		// Add <old>k to the dictionary.
		lzw_add_to_dict(d,d->oldcode,d->ct[code].firstchar);
	}
	else {
		// No, code is not in table.
		if(d->oldcode>=d->ct_used) {
			iw_seterror(rctx->ctx,iwgif_get_string(rctx->ctx,iws_gif_decode_error));
			return 0;
		}

		// Let k = the first char of the translation of oldcode.
		// Add <oldcode>k to the dictionary.
		newpos = lzw_add_to_dict(d,d->oldcode,d->ct[d->oldcode].firstchar);

		// Write <oldcode>k to the output stream.
		if(newpos>=0)
			lzw_emit_code(rctx,d,newpos);
	}
	d->oldcode = code;

	return 1;
}

// Decode as much as possible of the provided LZW-encoded data.
// Any unfinished business is recorded, to be continued the next time
// this function is called.
static int lzw_process_bytes(struct iwgifreadcontext *rctx, struct lzwdeccontext *d,
	unsigned char *data, size_t data_size)
{
	static const unsigned char bitmasks[8] = {1,2,4,8,16,32,64,128};
	size_t i;
	int b;
	int retval=0;

	for(i=0;i<data_size;i++) {
		// Look at the bits one at a time.
		for(b=0;b<8;b++) {
			if(d->eoi_flag) { // Stop if we've seen an EOI (end of image) code.
				retval=1;
				goto done;
			}

			if(data[i]&bitmasks[b])
				d->pending_code |= 1<<d->bits_in_pending_code;
			d->bits_in_pending_code++;

			// When we get enough bits to form a complete LZW code, process it.
			if(d->bits_in_pending_code >= d->current_codesize) {
				if(!lzw_process_code(rctx,d,d->pending_code)) goto done;
				d->pending_code=0;
				d->bits_in_pending_code=0;
			}
		}
	}
	retval=1;

done:
	return retval;
}

////////////////////////////////////////////////////////

// Allocate and set up the global "screen".
static int iwgif_init_screen(struct iwgifreadcontext *rctx)
{
	struct iw_image *img;
	int retval=0;

	if(rctx->screen_initialized) return 1;
	rctx->screen_initialized = 1;

	img = rctx->img;

	// Allocate IW image
	if(rctx->has_transparency) {
		rctx->bytes_per_pixel=4;
		img->imgtype = IW_IMGTYPE_RGBA;
	}
	else {
		rctx->bytes_per_pixel=3;
		img->imgtype = IW_IMGTYPE_RGB;
	}
	img->bit_depth = 8;
	img->bpr = rctx->bytes_per_pixel * img->width;

	img->pixels = (unsigned char*)iw_malloc_large(rctx->ctx, img->bpr, img->height);
	if(!img->pixels) goto done;

	// TODO: Maybe it would be better to clear the screen to the background
	// color, if available.
	memset(img->pixels,0,img->bpr*img->height);

	retval=1;
done:
	return retval;
}

// Make an array of pointers into the global screen, which point to the
// start of each row in the local image. This will be useful for
// de-interlacing.
static int iwgif_make_row_pointers(struct iwgifreadcontext *rctx)
{
	struct iw_image *img;
	int pass;
	int startrow, rowskip;
	int rowcount;
	int row;

	if(rctx->row_pointers) iw_free(rctx->row_pointers);
	rctx->row_pointers = (unsigned char**)iw_malloc(rctx->ctx, sizeof(unsigned char*)*rctx->image_height);
	if(!rctx->row_pointers) return 0;

	img = rctx->img;

	if(rctx->interlaced) {
		rowcount=0;
		for(pass=1;pass<=4;pass++) {
			if(pass==1) { startrow=0; rowskip=8; }
			else if(pass==2) { startrow=4; rowskip=8; }
			else if(pass==3) { startrow=2; rowskip=4; }
			else { startrow=1; rowskip=2; }

			for(row=startrow;row<rctx->image_height;row+=rowskip) {
				rctx->row_pointers[rowcount++] = &img->pixels[(rctx->image_top+row)*img->bpr + (rctx->image_left)*rctx->bytes_per_pixel];
			}
		}
	}
	else {
		for(row=0;row<rctx->image_height;row++) {
			rctx->row_pointers[row] = &img->pixels[(rctx->image_top+row)*img->bpr + (rctx->image_left)*rctx->bytes_per_pixel];
		}
	}
	return 1;
}

static int iwgif_read_image(struct iwgifreadcontext *rctx)
{
	int retval=0;
	struct lzwdeccontext d;
	size_t subblocksize;
	struct iw_image *img;
	int has_local_ct;
	int local_ct_size;

	unsigned int root_codesize;

	img = rctx->img;

	// Read image header information
	if(!iwgif_read(rctx,rctx->rbuf,10)) goto done;

	rctx->image_left = iw_read_uint16le(&rctx->rbuf[0]);
	rctx->image_height = iw_read_uint16le(&rctx->rbuf[2]);
	rctx->image_width = iw_read_uint16le(&rctx->rbuf[4]);
	rctx->image_height = iw_read_uint16le(&rctx->rbuf[6]);

	rctx->interlaced = (int)((rctx->rbuf[8]>>6)&0x01);

	has_local_ct = (int)((rctx->rbuf[8]>>7)&0x01);
	if(has_local_ct) {
		local_ct_size = (int)(rctx->rbuf[8]&0x07);
		rctx->colortable.num_entries = 1<<(1+local_ct_size);
	}

	if(has_local_ct) {
		// We only support one image, so we don't need to keep both a global and a
		// local color table. If an image has both, the local table will overwrite
		// the global one.
		if(!iwgif_read_color_table(rctx,&rctx->colortable)) goto done;
	}

	root_codesize = (unsigned int)rctx->rbuf[9];

	// The spec does not allow the "minimum code size" to be less than 2.
	// Sizes >=12 are impossible to support.
	// There's no reason for the size to be larger than 8, but the spec
	// does not seem to forbid it.
	if(root_codesize<2 || root_codesize>11) {
		iw_seterror(rctx->ctx,iwgif_get_string(rctx->ctx,iws_gif_inval_lzw_min));
		goto done;
	}

	// The creation fo the global "screen" was deferred until now, to wait until
	// we know whether the first image has transparency.
	if(!iwgif_init_screen(rctx)) goto done;

	rctx->total_npixels = rctx->image_width * rctx->image_height;

	if(!iwgif_make_row_pointers(rctx)) goto done;

	lzw_init(&d,root_codesize);
	lzw_clear(&d);

	while(1) {
		// Read size of next subblock
		if(!iwgif_read(rctx,rctx->rbuf,1)) goto done;
		subblocksize = (size_t)rctx->rbuf[0];
		if(subblocksize==0) break;

		// Read next subblock
		if(!iwgif_read(rctx,rctx->rbuf,subblocksize)) goto done;
		if(!lzw_process_bytes(rctx,&d,rctx->rbuf,subblocksize)) goto done;

		if(d.eoi_flag) break;

		// Stop if we reached the end of the image. We don't care if we've read an
		// EOI code or not.
		if(rctx->pixels_set >= rctx->total_npixels) break;
	}

	retval=1;

done:
	return retval;
}

static int iwgif_read_main(struct iwgifreadcontext *rctx)
{
	int retval=0;
	int i;

	// Make all colors opaque by default.
	for(i=0;i<256;i++) {
		rctx->colortable.entry[i].a=255;
	}

	if(!iwgif_read_file_header(rctx)) goto done;

	if(!iwgif_read_screen_descriptor(rctx)) goto done;

	// Read global color table
	if(!iwgif_read_color_table(rctx,&rctx->colortable)) goto done;

	// Tell IW the background color.
	if(rctx->has_bg_color) {
		iw_set_input_bkgd_label(rctx->ctx,
			((double)rctx->colortable.entry[rctx->bg_color_index].r)/255.0,
			((double)rctx->colortable.entry[rctx->bg_color_index].g)/255.0,
			((double)rctx->colortable.entry[rctx->bg_color_index].b)/255.0);
	}

	// The remainder of the file consists of blocks whose type is indicated by
	// their initial byte.

	while(1) {
		// Read block type
		if(!iwgif_read(rctx,rctx->rbuf,1)) goto done;

		switch(rctx->rbuf[0]) {
		case 0x21: // extension
			if(!iwgif_read_extension(rctx)) goto done;
			break;
		case 0x2c: // image
			if(!iwgif_read_image(rctx)) goto done;
			goto ok;
		case 0x3b: // file trailer
			// We stop after reading the first image, so if we ever see a file
			// trailer, something's wrong.
			iw_seterror(rctx->ctx,iwgif_get_string(rctx->ctx,iws_gif_no_image));
			goto done;
		default:
			iw_seterror(rctx->ctx,iwgif_get_string(rctx->ctx,iws_gif_unsupported));
			goto done;
		}
	}

ok:
	retval=1;

done:
	return retval;
}

int iw_read_gif_file(struct iw_context *ctx, struct iw_iodescr *iodescr)
{
	struct iw_image img;
	struct iwgifreadcontext *rctx = NULL;
	int retval=0;

	memset(&img,0,sizeof(struct iw_image));
	rctx = iw_malloc(ctx,sizeof(struct iwgifreadcontext));
	if(!rctx) goto done;
	memset(rctx,0,sizeof(struct iwgifreadcontext));

	iw_set_string_table(ctx,IW_STRINGTABLENUM_GIF,iwgif_stringtable);

	rctx->ctx = ctx;
	rctx->iodescr = iodescr;
	rctx->img = &img;

	// Assume GIF images are sRGB.
	rctx->csdescr.cstype = IW_CSTYPE_SRGB;
	rctx->csdescr.sRGB_intent = IW_sRGB_INTENT_PERCEPTUAL;

	if(!iwgif_read_main(rctx))
		goto done;

	iw_set_input_image(ctx, &img);

	iw_set_input_colorspace(ctx,&rctx->csdescr);

	retval = 1;

done:
	if(!retval) {
		iw_seterror(ctx,iwgif_get_string(ctx,iws_gif_read_error));
	}

	if(iodescr->close_fn)
		(*iodescr->close_fn)(ctx,iodescr);

	if(rctx) {
		if(rctx->row_pointers) iw_free(rctx->row_pointers);
		iw_free(rctx);
	}

	return retval;
}
