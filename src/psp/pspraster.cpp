#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"

#include "rwpsp.h"

#define PLUGIN_ID ID_DRIVER

namespace rw {
namespace psp {

int32 nativeRasterOffset;

#ifdef RW_PSP

#include <malloc.h>
#include <pspgu.h>
#include <pspkernel.h>

// GE texture limits
enum { MAXLEVELS = 8, MAXDIM = 512 };

static bool
isPow2(int32 n)
{
	return n > 0 && (n & (n-1)) == 0;
}

// Geometry of one mip level. Rows are padded to 16 bytes, which is what
// the GE wants for the buffer width of tiny levels.
static void
levelInfo(Raster *raster, int32 level, int32 *pw, int32 *ph, int32 *pstride, uint32 *poffset)
{
	PspRaster *natras = GETPSPRASTEREXT(raster);
	int32 w = raster->originalWidth, h = raster->originalHeight;
	uint32 offset = 0;
	int32 stride = 0;
	for(int32 i = 0;; i++){
		int32 bufw = w*natras->bpp < 16 ? 16/natras->bpp : w;
		stride = bufw*natras->bpp;
		uint32 size = (stride*h + 15) & ~15u;
		if(i == level)
			break;
		offset += size;
		if(w > 1) w /= 2;
		if(h > 1) h /= 2;
	}
	if(pw) *pw = w;
	if(ph) *ph = h;
	if(pstride) *pstride = stride;
	if(poffset) *poffset = offset;
}

static uint32
getLevelSize(Raster *raster, int32 level)
{
	int32 h, stride;
	levelInfo(raster, level, nil, &h, &stride, nil);
	return stride*h;
}

static Raster*
rasterCreateTexture(Raster *raster)
{
	if(raster->format & (Raster::PAL4 | Raster::PAL8)){
		RWERROR((ERR_NOTEXTURE));
		return nil;
	}
	if(!isPow2(raster->width) || !isPow2(raster->height) ||
	   raster->width > MAXDIM || raster->height > MAXDIM){
		RWERROR((ERR_INVRASTER));
		return nil;
	}

	PspRaster *natras = GETPSPRASTEREXT(raster);
	switch(raster->format & 0xF00){
	case Raster::C8888:
		natras->psm = GU_PSM_8888;
		natras->hasAlpha = 1;
		natras->bpp = 4;
		break;
	case Raster::DEFAULT:
	case Raster::C888:
		// stored as 8888, alpha is always 0xFF
		natras->psm = GU_PSM_8888;
		natras->hasAlpha = 0;
		natras->bpp = 4;
		break;
	case Raster::C1555:
		natras->psm = GU_PSM_5551;
		natras->hasAlpha = 1;
		natras->bpp = 2;
		break;
	case Raster::C565:
		natras->psm = GU_PSM_5650;
		natras->hasAlpha = 0;
		natras->bpp = 2;
		break;
	case Raster::C4444:
		natras->psm = GU_PSM_4444;
		natras->hasAlpha = 1;
		natras->bpp = 2;
		break;
	default:
		RWERROR((ERR_INVRASTER));
		return nil;
	}
	raster->depth = natras->bpp*8;

	natras->numLevels = 1;
	if(raster->format & Raster::MIPMAP){
		int32 n = Raster::calculateNumLevels(raster->width, raster->height);
		natras->numLevels = n > MAXLEVELS ? MAXLEVELS : n;
	}

	// levelInfo wants these
	raster->originalWidth = raster->width;
	raster->originalHeight = raster->height;
	int32 stride0;
	levelInfo(raster, 0, nil, nil, &stride0, nil);
	raster->stride = stride0;

	uint32 size = 0;
	for(int32 i = 0; i < natras->numLevels; i++)
		size += (getLevelSize(raster, i) + 15) & ~15u;
	natras->size = size;
	natras->base = (uint8*)memalign(16, size);
	if(natras->base == nil){
		RWERROR((ERR_ALLOC, size));
		return nil;
	}
	memset(natras->base, 0, size);

	raster->flags &= ~Raster::DONTALLOCATE;
	return raster;
}

Raster*
rasterCreate(Raster *raster)
{
	PspRaster *natras = GETPSPRASTEREXT(raster);

	natras->hasAlpha = 0;
	natras->numLevels = 1;
	natras->base = nil;
	natras->size = 0;

	Raster *ret = raster;

	if(raster->width == 0 || raster->height == 0){
		raster->flags |= Raster::DONTALLOCATE;
		raster->stride = 0;
		goto ret;
	}
	if(raster->flags & Raster::DONTALLOCATE)
		goto ret;

	switch(raster->type){
	case Raster::NORMAL:
	case Raster::TEXTURE:
		ret = rasterCreateTexture(raster);
		break;
	case Raster::ZBUFFER:
	case Raster::CAMERA:
		// lives in VRAM and is managed by the device, nothing to allocate
		break;
	default:
		// CAMERATEXTURE: no render to texture yet
		RWERROR((ERR_INVRASTER));
		return nil;
	}

ret:
	raster->originalWidth = raster->width;
	raster->originalHeight = raster->height;
	raster->originalStride = raster->stride;
	raster->originalPixels = raster->pixels;
	return ret;
}

uint8*
rasterLock(Raster *raster, int32 level, int32 lockMode)
{
	PspRaster *natras = GETPSPRASTEREXT(raster);

	assert(raster->privateFlags == 0);

	switch(raster->type){
	case Raster::NORMAL:
	case Raster::TEXTURE: {
		if(natras->base == nil || level >= natras->numLevels)
			return nil;
		int32 w, h, stride;
		uint32 offset;
		levelInfo(raster, level, &w, &h, &stride, &offset);
		raster->width = w;
		raster->height = h;
		raster->stride = stride;
		raster->pixels = natras->base + offset;
		raster->privateFlags = lockMode;
		return raster->pixels;
	}
	default:
		assert(0 && "cannot lock this type of raster");
		return nil;
	}
}

void
rasterUnlock(Raster *raster, int32 level)
{
	PspRaster *natras = GETPSPRASTEREXT(raster);

	assert(raster->pixels);

	if(raster->privateFlags & Raster::LOCKWRITE)
		sceKernelDcacheWritebackRange(natras->base, natras->size);

	raster->width = raster->originalWidth;
	raster->height = raster->originalHeight;
	raster->stride = raster->originalStride;
	raster->pixels = raster->originalPixels;
	raster->privateFlags = 0;
}

int32
rasterNumLevels(Raster *raster)
{
	return GETPSPRASTEREXT(raster)->numLevels;
}

bool32
imageFindRasterFormat(Image *img, int32 type,
	int32 *pWidth, int32 *pHeight, int32 *pDepth, int32 *pFormat)
{
	int32 width, height, depth, format;

	assert((type&0xF) == Raster::TEXTURE);

	// The GE needs power of two textures
	for(width = 1; width < img->width; width <<= 1);
	for(height = 1; height < img->height; height <<= 1);
	if(width != img->width || height != img->height || width > MAXDIM || height > MAXDIM){
		RWERROR((ERR_INVRASTER));
		return 0;
	}

	depth = img->depth;
	if(depth <= 8)
		depth = 32;

	switch(depth){
	case 32:
		if(img->hasAlpha())
			format = Raster::C8888;
		else{
			format = Raster::C888;
			depth = 24;
		}
		break;
	case 24:
		format = Raster::C888;
		break;
	case 16:
		format = Raster::C1555;
		break;
	default:
		RWERROR((ERR_INVRASTER));
		return 0;
	}

	format |= type;

	*pWidth = width;
	*pHeight = height;
	*pDepth = depth;
	*pFormat = format;
	return 1;
}

bool32
rasterFromImage(Raster *raster, Image *image)
{
	if((raster->type&0xF) != Raster::TEXTURE)
		return 0;

	// Unpalettize image if necessary but don't change original
	Image *truecolimg = nil;
	if(image->depth <= 8){
		truecolimg = Image::create(image->width, image->height, image->depth);
		truecolimg->pixels = image->pixels;
		truecolimg->stride = image->stride;
		truecolimg->palette = image->palette;
		truecolimg->unpalettize();
		image = truecolimg;
	}

	PspRaster *natras = GETPSPRASTEREXT(raster);
	void (*conv)(uint8 *out, uint8 *in) = nil;
	switch(image->depth){
	case 32:
		if(natras->psm == GU_PSM_8888)
			conv = conv_RGBA8888_from_RGBA8888;
		break;
	case 24:
		if(natras->psm == GU_PSM_8888)
			conv = conv_RGBA8888_from_RGB888;
		break;
	case 16:
		if(natras->psm == GU_PSM_5551)
			conv = conv_ABGR1555_from_ARGB1555;
		break;
	}
	if(conv == nil){
		if(truecolimg)
			truecolimg->destroy();
		RWERROR((ERR_INVRASTER));
		return 0;
	}

	bool unlock = false;
	if(raster->pixels == nil){
		raster->lock(0, Raster::LOCKWRITE|Raster::LOCKNOFETCH);
		unlock = true;
	}
	uint8 *pixels = raster->pixels;
	assert(pixels);
	assert(image->width == raster->width);
	assert(image->height == raster->height);

	// GE texture space has v = 0 at the top, same as Image: no flipping
	uint8 *imgpixels = image->pixels;
	for(int32 y = 0; y < image->height; y++){
		uint8 *imgrow = imgpixels;
		uint8 *rasrow = pixels;
		for(int32 x = 0; x < image->width; x++){
			conv(rasrow, imgrow);
			imgrow += image->bpp;
			rasrow += natras->bpp;
		}
		imgpixels += image->stride;
		pixels += raster->stride;
	}
	if(unlock)
		raster->unlock(0);
	if(truecolimg)
		truecolimg->destroy();
	return 1;
}

Image*
rasterToImage(Raster *raster)
{
	int32 depth;
	PspRaster *natras = GETPSPRASTEREXT(raster);

	switch(natras->psm){
	case GU_PSM_8888:
		depth = (raster->format & 0xF00) == Raster::C8888 ? 32 : 24;
		break;
	case GU_PSM_5551:
		depth = 16;
		break;
	default:
		RWERROR((ERR_INVRASTER));
		return nil;
	}

	bool unlock = false;
	if(raster->pixels == nil){
		raster->lock(0, Raster::LOCKREAD);
		unlock = true;
	}

	Image *image = Image::create(raster->width, raster->height, depth);
	image->allocate();

	uint8 *imgpixels = image->pixels;
	uint8 *pixels = raster->pixels;
	for(int32 y = 0; y < image->height; y++){
		uint8 *imgrow = imgpixels;
		uint8 *rasrow = pixels;
		for(int32 x = 0; x < image->width; x++){
			if(depth == 16)
				conv_ARGB1555_from_ABGR1555(imgrow, rasrow);
			else if(depth == 32)
				conv_RGBA8888_from_RGBA8888(imgrow, rasrow);
			else
				conv_RGB888_from_RGB888(imgrow, rasrow);
			imgrow += image->bpp;
			rasrow += natras->bpp;
		}
		imgpixels += image->stride;
		pixels += raster->stride;
	}

	if(unlock)
		raster->unlock(0);
	return image;
}

static void*
createNativeRaster(void *object, int32 offset, int32)
{
	PspRaster *ras = PLUGINOFFSET(PspRaster, object, offset);
	ras->base = nil;
	ras->size = 0;
	return object;
}

void evictRaster(Raster *raster);

static void*
destroyNativeRaster(void *object, int32 offset, int32)
{
	Raster *raster = (Raster*)object;
	PspRaster *natras = PLUGINOFFSET(PspRaster, object, offset);
	evictRaster(raster);
	free(natras->base);
	natras->base = nil;
	return object;
}

static void*
copyNativeRaster(void *dst, void *, int32 offset, int32)
{
	PspRaster *d = PLUGINOFFSET(PspRaster, dst, offset);
	d->base = nil;
	d->size = 0;
	return dst;
}

// Native texture streaming (our own format, little endian)

Texture*
readNativeTexture(Stream *stream)
{
	uint32 platform;
	if(!findChunk(stream, ID_STRUCT, nil, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	platform = stream->readU32();
	if(platform != PLATFORM_PSP){
		RWERROR((ERR_PLATFORM, platform));
		return nil;
	}
	Texture *tex = Texture::create(nil);
	if(tex == nil)
		return nil;

	// Texture
	tex->filterAddressing = stream->readU32();
	stream->read8(tex->name, 32);
	stream->read8(tex->mask, 32);

	// Raster
	uint32 format = stream->readU32();
	int32 width = stream->readI32();
	int32 height = stream->readI32();
	int32 depth = stream->readI32();
	int32 numLevels = stream->readI32();

	Raster *raster = Raster::create(width, height, depth, format | Raster::TEXTURE, PLATFORM_PSP);
	if(raster == nil){
		tex->destroy();
		return nil;
	}
	tex->raster = raster;
	PspRaster *natras = GETPSPRASTEREXT(raster);
	natras->hasAlpha = stream->readI32() & 1;
	if(numLevels > natras->numLevels){
		tex->destroy();
		RWERROR((ERR_FORMAT_UNSUPPORTED));
		return nil;
	}

	for(int32 i = 0; i < numLevels; i++){
		uint32 size = stream->readU32();
		uint8 *data = raster->lock(i, Raster::LOCKWRITE|Raster::LOCKNOFETCH);
		stream->read8(data, size);
		raster->unlock(i);
	}
	return tex;
}

void
writeNativeTexture(Texture *tex, Stream *stream)
{
	Raster *raster = tex->raster;
	PspRaster *natras = GETPSPRASTEREXT(raster);

	int32 chunksize = getSizeNativeTexture(tex);
	writeChunkHeader(stream, ID_STRUCT, chunksize-12);
	stream->writeU32(PLATFORM_PSP);

	// Texture
	stream->writeU32(tex->filterAddressing);
	stream->write8(tex->name, 32);
	stream->write8(tex->mask, 32);

	// Raster
	int32 numLevels = natras->numLevels;
	stream->writeI32(raster->format);
	stream->writeI32(raster->width);
	stream->writeI32(raster->height);
	stream->writeI32(raster->depth);
	stream->writeI32(numLevels);
	stream->writeI32(natras->hasAlpha ? 1 : 0);

	for(int32 i = 0; i < numLevels; i++){
		uint32 size = getLevelSize(raster, i);
		stream->writeU32(size);
		uint8 *data = raster->lock(i, Raster::LOCKREAD);
		stream->write8(data, size);
		raster->unlock(i);
	}
}

uint32
getSizeNativeTexture(Texture *tex)
{
	uint32 size = 12 + 4 + 4 + 64 + 24;
	int32 levels = tex->raster->getNumLevels();
	for(int32 i = 0; i < levels; i++)
		size += 4 + getLevelSize(tex->raster, i);
	return size;
}

void
registerNativeRaster(void)
{
	nativeRasterOffset = Raster::registerPlugin(sizeof(PspRaster),
	                                            ID_RASTERPSP,
	                                            createNativeRaster,
	                                            destroyNativeRaster,
	                                            copyNativeRaster);
}

#endif

}
}
