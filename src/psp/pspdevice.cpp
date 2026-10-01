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

#ifdef RW_PSP

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>

namespace rw {
namespace psp {

#define SCR_W 480
#define SCR_H 272
#define BUF_W 512

// VRAM layout: two 8888 color buffers, then a 16 bit depth buffer
#define FB_DRAW_OFFS 0
#define FB_DISP_OFFS (BUF_W*SCR_H*4)
#define FB_DEPTH_OFFS (BUF_W*SCR_H*4*2)

static unsigned int __attribute__((aligned(16))) displayList[0x40000];

static bool listOpen;

// Everything the RW state machine can set. The GU calls are made
// right away if we are inside a display list, otherwise the cache is
// replayed at the start of the next one.
struct RwStateCache
{
	bool32 vertexAlpha, textureAlpha;
	bool32 alphaTestEnable, blendEnable;
	uint32 srcblend, destblend;
	uint32 ztest, zwrite;
	uint32 fogEnable, fogColor;
	float32 fogStart, fogEnd;
	uint32 cullmode;
	uint32 alphaFunc, alphaRef;
	uint32 gsalpha, gsalpharef;
	// stencil is not implemented on this backend, state is only stored
	uint32 stencil[8];

	Raster *texRaster;
	uint32 filter, addressU, addressV;
};
static RwStateCache rwStateCache;

// Lighting
static uint32 ambientColor;
static int32 numGuLights;

static inline uint32
packABGR(const RGBAf &c)
{
	auto cl = [](float f) -> uint32 {
		int32 i = (int32)(f*255.0f + 0.5f);
		return i < 0 ? 0 : i > 255 ? 255 : i;
	};
	return cl(c.red) | cl(c.green)<<8 | cl(c.blue)<<16 | cl(c.alpha)<<24;
}

static inline uint32
packABGR(const RGBA &c)
{
	return c.red | c.green<<8 | c.blue<<16 | c.alpha<<24;
}

static void
guEnable(int cap, bool on)
{
	if(on)
		sceGuEnable(cap);
	else
		sceGuDisable(cap);
}

// Blend factors. The GE has separate sets for source and destination.

static void
blendFactor(uint32 rwfunc, bool isSrc, int *mode, uint32 *fix)
{
	*fix = 0;
	switch(rwfunc){
	case BLENDZERO:		*mode = GU_FIX; *fix = 0x000000; break;
	case BLENDONE:		*mode = GU_FIX; *fix = 0xFFFFFF; break;
	case BLENDSRCCOLOR:	*mode = isSrc ? GU_SRC_ALPHA : GU_SRC_COLOR; break;
	case BLENDINVSRCCOLOR:	*mode = isSrc ? GU_ONE_MINUS_SRC_ALPHA : GU_ONE_MINUS_SRC_COLOR; break;
	case BLENDSRCALPHA:	*mode = GU_SRC_ALPHA; break;
	case BLENDINVSRCALPHA:	*mode = GU_ONE_MINUS_SRC_ALPHA; break;
	case BLENDDESTALPHA:	*mode = GU_DST_ALPHA; break;
	case BLENDINVDESTALPHA:	*mode = GU_ONE_MINUS_DST_ALPHA; break;
	case BLENDDESTCOLOR:	*mode = isSrc ? GU_DST_COLOR : GU_DST_ALPHA; break;
	case BLENDINVDESTCOLOR:	*mode = isSrc ? GU_ONE_MINUS_DST_COLOR : GU_ONE_MINUS_DST_ALPHA; break;
	default:		*mode = GU_SRC_ALPHA; break;
	}
}

static void
applyBlend(void)
{
	int sm, dm;
	uint32 sf, df;
	blendFactor(rwStateCache.srcblend, true, &sm, &sf);
	blendFactor(rwStateCache.destblend, false, &dm, &df);
	sceGuBlendFunc(GU_ADD, sm, dm, sf, df);
	guEnable(GU_BLEND, rwStateCache.blendEnable);
}

static void
applyAlphaTest(void)
{
	static const int funcs[] = { GU_ALWAYS, GU_GEQUAL, GU_LESS };
	sceGuAlphaFunc(funcs[rwStateCache.alphaFunc], rwStateCache.alphaRef, 0xFF);
	guEnable(GU_ALPHA_TEST, rwStateCache.alphaTestEnable &&
	                        rwStateCache.alphaFunc != ALPHAALWAYS);
}

static void
applyDepth(void)
{
	// GE can't write depth with the test disabled, so emulate that with ALWAYS
	sceGuEnable(GU_DEPTH_TEST);
	sceGuDepthFunc(rwStateCache.ztest ? GU_GEQUAL : GU_ALWAYS);
	sceGuDepthMask(rwStateCache.zwrite ? GU_FALSE : GU_TRUE);
}

static void
applyCull(void)
{
	guEnable(GU_CULL_FACE, rwStateCache.cullmode != CULLNONE);
	// TODO: verify winding against the view matrix X flip on device
	sceGuFrontFace(rwStateCache.cullmode == CULLBACK ? GU_CW : GU_CCW);
}

static void
applyFog(void)
{
	guEnable(GU_FOG, rwStateCache.fogEnable);
	sceGuFog(rwStateCache.fogStart, rwStateCache.fogEnd, rwStateCache.fogColor);
}

static void
applyAllState(void)
{
	applyBlend();
	applyAlphaTest();
	applyDepth();
	applyCull();
	applyFog();
}

void
setAlphaBlend(bool32 enable)
{
	if(rwStateCache.blendEnable != enable){
		rwStateCache.blendEnable = enable;
		if(listOpen)
			guEnable(GU_BLEND, enable);
	}
}

bool32
getAlphaBlend(void)
{
	return rwStateCache.blendEnable;
}

static void
setAlphaTest(bool32 enable)
{
	if(rwStateCache.alphaTestEnable != enable){
		rwStateCache.alphaTestEnable = enable;
		if(listOpen)
			applyAlphaTest();
	}
}

static void
setAlphaTestFunction(uint32 func)
{
	if(rwStateCache.alphaFunc != func){
		rwStateCache.alphaFunc = func;
		if(listOpen)
			applyAlphaTest();
	}
}

static void
setVertexAlpha(bool32 enable)
{
	if(rwStateCache.vertexAlpha != enable){
		if(!rwStateCache.textureAlpha){
			setAlphaBlend(enable);
			setAlphaTest(enable);
		}
		rwStateCache.vertexAlpha = enable;
	}
}

// Textures

void
evictRaster(Raster *raster)
{
	if(rwStateCache.texRaster == raster)
		rwStateCache.texRaster = nil;
}

static void
setRasterStage(Raster *raster)
{
	if(raster == rwStateCache.texRaster)
		return;
	rwStateCache.texRaster = raster;
	bool32 alpha = 0;
	if(raster){
		assert(raster->platform == PLATFORM_PSP);
		alpha = GETPSPRASTEREXT(raster)->hasAlpha;
	}
	if(alpha != rwStateCache.textureAlpha){
		rwStateCache.textureAlpha = alpha;
		if(!rwStateCache.vertexAlpha){
			setAlphaBlend(alpha);
			setAlphaTest(alpha);
		}
	}
}

void
setTexture(Texture *tex)
{
	if(tex == nil || tex->raster == nil){
		setRasterStage(nil);
		return;
	}
	setRasterStage(tex->raster);
	rwStateCache.filter = tex->getFilter();
	rwStateCache.addressU = tex->getAddressU();
	rwStateCache.addressV = tex->getAddressV();
}

// Flush the texture state to the GE. Called right before something is drawn.
void
applyTexture(void)
{
	Raster *raster = rwStateCache.texRaster;
	if(raster == nil){
		sceGuDisable(GU_TEXTURE_2D);
		return;
	}
	PspRaster *natras = GETPSPRASTEREXT(raster);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexMode(natras->psm, natras->numLevels-1, 0, 0);

	int32 w = raster->originalWidth, h = raster->originalHeight;
	uint8 *p = natras->base;
	for(int32 i = 0; i < natras->numLevels; i++){
		int32 bufw = w*natras->bpp < 16 ? 16/natras->bpp : w;
		sceGuTexImage(i, w, h, bufw, p);
		p += ((bufw*natras->bpp*h) + 15) & ~15;
		if(w > 1) w /= 2;
		if(h > 1) h /= 2;
	}

	bool mip = natras->numLevels > 1;
	int min, mag;
	switch(rwStateCache.filter){
	case Texture::NEAREST:
		min = GU_NEAREST; mag = GU_NEAREST; break;
	case Texture::MIPNEAREST:
		min = mip ? GU_NEAREST_MIPMAP_NEAREST : GU_NEAREST; mag = GU_NEAREST; break;
	case Texture::MIPLINEAR:
		min = mip ? GU_LINEAR_MIPMAP_NEAREST : GU_LINEAR; mag = GU_LINEAR; break;
	case Texture::LINEARMIPNEAREST:
		min = mip ? GU_NEAREST_MIPMAP_LINEAR : GU_NEAREST; mag = GU_NEAREST; break;
	case Texture::LINEARMIPLINEAR:
		min = mip ? GU_LINEAR_MIPMAP_LINEAR : GU_LINEAR; mag = GU_LINEAR; break;
	case Texture::LINEAR:
	default:
		min = GU_LINEAR; mag = GU_LINEAR; break;
	}
	sceGuTexFilter(min, mag);
	if(mip)
		sceGuTexLevelMode(GU_TEXTURE_AUTO, 0.0f);
	// GE only knows repeat and clamp
	sceGuTexWrap(rwStateCache.addressU == Texture::CLAMP || rwStateCache.addressU == Texture::BORDER ? GU_CLAMP : GU_REPEAT,
	             rwStateCache.addressV == Texture::CLAMP || rwStateCache.addressV == Texture::BORDER ? GU_CLAMP : GU_REPEAT);
	sceGuTexFunc(GU_TFX_MODULATE, natras->hasAlpha ? GU_TCC_RGBA : GU_TCC_RGB);
	sceGuTexScale(1.0f, 1.0f);
	sceGuTexOffset(0.0f, 0.0f);
}

// Matrices

void
setWorldMatrix(Matrix *mat)
{
	ScePspFMatrix4 m;
	memcpy(&m, mat, sizeof(float)*16);
	// Matrix has flags words in the padding slots
	float *f = (float*)&m;
	f[3] = f[7] = f[11] = 0.0f;
	f[15] = 1.0f;
	sceGuSetMatrix(GU_MODEL, &m);
}

// Lighting

int32
setLights(WorldLights *lightData)
{
	int32 n = 0;
	int32 i;

	for(i = 0; i < 4; i++)
		sceGuDisable(GU_LIGHT0 + i);

	ambientColor = packABGR(lightData->ambient) | 0xFF000000;
	sceGuAmbient(ambientColor);

	// Light vectors are in world space, which is also where RW lights live.
	for(i = 0; i < lightData->numDirectionals && n < 4; i++, n++){
		Light *l = lightData->directionals[i];
		V3d d = neg(l->getFrame()->getLTM()->at);	// towards the light
		ScePspFVector3 v = { d.x, d.y, d.z };
		sceGuLight(n, GU_DIRECTIONAL, GU_DIFFUSE, &v);
		sceGuLightColor(n, GU_DIFFUSE, packABGR(l->color));
		sceGuEnable(GU_LIGHT0 + n);
	}
	for(i = 0; i < lightData->numLocals && n < 4; i++){
		Light *l = lightData->locals[i];
		V3d p = l->getFrame()->getLTM()->pos;
		ScePspFVector3 pos = { p.x, p.y, p.z };
		switch(l->getType()){
		case Light::POINT:
			sceGuLight(n, GU_POINTLIGHT, GU_DIFFUSE, &pos);
			break;
		case Light::SPOT:
		case Light::SOFTSPOT: {
			V3d a = l->getFrame()->getLTM()->at;
			ScePspFVector3 dir = { a.x, a.y, a.z };
			sceGuLight(n, GU_SPOTLIGHT, GU_DIFFUSE, &pos);
			sceGuLightSpot(n, &dir, 1.0f, -l->minusCosAngle);
			break;
		}
		default:
			continue;
		}
		// RW falls off linearly to 0 at radius. Approximation: 1/(1 + k*d^2)
		float r = l->radius > 0.0f ? l->radius : 1.0f;
		sceGuLightAtt(n, 1.0f, 0.0f, 4.0f/(r*r));
		sceGuLightColor(n, GU_DIFFUSE, packABGR(l->color));
		sceGuEnable(GU_LIGHT0 + n);
		n++;
	}
	numGuLights = n;
	return n;
}

int32
lightingCB(Atomic *atomic)
{
	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;

	if(atomic->geometry->flags & rw::Geometry::LIGHT)
		((World*)engine->currentWorld)->enumerateLights(atomic, &lightData);
	else
		memset(&lightData, 0, sizeof(lightData));
	return setLights(&lightData);
}

int32
lightingCB(void)
{
	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;

	((World*)engine->currentWorld)->enumerateLights(&lightData);
	return setLights(&lightData);
}

// enable: run the vertex normals through the GE lighting.
// vertexColors: vertex colors replace the material color (prelit)
void
setLighting(bool32 enable, bool32 vertexColors)
{
	guEnable(GU_LIGHTING, enable);
	if(enable)
		sceGuColorMaterial(vertexColors ? GU_AMBIENT|GU_DIFFUSE : 0);
}

void
setMaterial(uint32 flags, const RGBA &color)
{
	static RGBA white = { 255, 255, 255, 255 };
	uint32 c = packABGR((flags & Geometry::MODULATE) ? color : white);
	// sceGuColor sets ambient/diffuse/specular; alpha is carried by ambient
	sceGuColor(c);
	sceGuMaterial(GU_AMBIENT|GU_DIFFUSE, c);
}

void
flushCache(void)
{
	sceKernelDcacheWritebackAll();
}

void*
getListMemory(uint32 size)
{
	return sceGuGetMemory(size);
}

// Render states

static void
setRenderState(int32 state, void *pvalue)
{
	uint32 value = (uint32)(uintptr)pvalue;
	switch(state){
	case TEXTURERASTER:
		setRasterStage((Raster*)pvalue);
		break;
	case TEXTUREADDRESS:
		rwStateCache.addressU = rwStateCache.addressV = value;
		break;
	case TEXTUREADDRESSU:
		rwStateCache.addressU = value;
		break;
	case TEXTUREADDRESSV:
		rwStateCache.addressV = value;
		break;
	case TEXTUREFILTER:
		rwStateCache.filter = value;
		break;
	case VERTEXALPHA:
		setVertexAlpha(value);
		break;
	case SRCBLEND:
		rwStateCache.srcblend = value;
		if(listOpen) applyBlend();
		break;
	case DESTBLEND:
		rwStateCache.destblend = value;
		if(listOpen) applyBlend();
		break;
	case ZTESTENABLE:
		rwStateCache.ztest = value;
		if(listOpen) applyDepth();
		break;
	case ZWRITEENABLE:
		rwStateCache.zwrite = value;
		if(listOpen) applyDepth();
		break;
	case FOGENABLE:
		rwStateCache.fogEnable = value;
		if(listOpen) applyFog();
		break;
	case FOGCOLOR:
		// RW: 0xAABBGGRR as RWRGBAINT, which is what the GE wants too
		rwStateCache.fogColor = value;
		if(listOpen) applyFog();
		break;
	case CULLMODE:
		rwStateCache.cullmode = value;
		if(listOpen) applyCull();
		break;

	case STENCILENABLE: case STENCILFAIL: case STENCILZFAIL: case STENCILPASS:
	case STENCILFUNCTION: case STENCILFUNCTIONREF: case STENCILFUNCTIONMASK:
	case STENCILFUNCTIONWRITEMASK:
		rwStateCache.stencil[state-STENCILENABLE] = value;
		break;

	case ALPHATESTFUNC:
		setAlphaTestFunction(value);
		break;
	case ALPHATESTREF:
		rwStateCache.alphaRef = value;
		if(listOpen) applyAlphaTest();
		break;
	case GSALPHATEST:
		rwStateCache.gsalpha = value;
		break;
	case GSALPHATESTREF:
		rwStateCache.gsalpharef = value;
		break;
	}
}

static void*
getRenderState(int32 state)
{
	uint32 val;
	switch(state){
	case TEXTURERASTER:
		return rwStateCache.texRaster;
	case TEXTUREADDRESS:
		val = rwStateCache.addressU == rwStateCache.addressV ? rwStateCache.addressU : 0;
		break;
	case TEXTUREADDRESSU: val = rwStateCache.addressU; break;
	case TEXTUREADDRESSV: val = rwStateCache.addressV; break;
	case TEXTUREFILTER: val = rwStateCache.filter; break;
	case VERTEXALPHA: val = rwStateCache.vertexAlpha; break;
	case SRCBLEND: val = rwStateCache.srcblend; break;
	case DESTBLEND: val = rwStateCache.destblend; break;
	case ZTESTENABLE: val = rwStateCache.ztest; break;
	case ZWRITEENABLE: val = rwStateCache.zwrite; break;
	case FOGENABLE: val = rwStateCache.fogEnable; break;
	case FOGCOLOR: val = rwStateCache.fogColor; break;
	case CULLMODE: val = rwStateCache.cullmode; break;

	case STENCILENABLE: case STENCILFAIL: case STENCILZFAIL: case STENCILPASS:
	case STENCILFUNCTION: case STENCILFUNCTIONREF: case STENCILFUNCTIONMASK:
	case STENCILFUNCTIONWRITEMASK:
		val = rwStateCache.stencil[state-STENCILENABLE];
		break;

	case ALPHATESTFUNC: val = rwStateCache.alphaFunc; break;
	case ALPHATESTREF: val = rwStateCache.alphaRef; break;
	case GSALPHATEST: val = rwStateCache.gsalpha; break;
	case GSALPHATESTREF: val = rwStateCache.gsalpharef; break;
	default:
		val = 0;
	}
	return (void*)(uintptr)val;
}

static void
resetRenderState(void)
{
	memset(&rwStateCache, 0, sizeof(rwStateCache));
	rwStateCache.alphaFunc = ALPHAGREATEREQUAL;
	rwStateCache.alphaRef = 10;
	rwStateCache.gsalpharef = 128;
	rwStateCache.srcblend = BLENDSRCALPHA;
	rwStateCache.destblend = BLENDINVSRCALPHA;
	rwStateCache.zwrite = 1;
	rwStateCache.ztest = 1;
	rwStateCache.cullmode = CULLNONE;
	rwStateCache.fogColor = 0xFFFFFFFF;
	rwStateCache.filter = Texture::LINEAR;
	rwStateCache.addressU = rwStateCache.addressV = Texture::WRAP;
	rwStateCache.stencil[STENCILFUNCTION-STENCILENABLE] = STENCILALWAYS;
	rwStateCache.stencil[STENCILFUNCTIONMASK-STENCILENABLE] = 0xFFFFFFFF;
	rwStateCache.stencil[STENCILFUNCTIONWRITEMASK-STENCILENABLE] = 0xFFFFFFFF;
	rwStateCache.stencil[STENCILFAIL-STENCILENABLE] =
	rwStateCache.stencil[STENCILZFAIL-STENCILENABLE] =
	rwStateCache.stencil[STENCILPASS-STENCILENABLE] = STENCILKEEP;
}

// Frame handling

static void
setViewport(Raster *fb)
{
	int32 x = fb->offsetX, y = fb->offsetY;
	int32 w = fb->width, h = fb->height;
	sceGuOffset(2048 - SCR_W/2, 2048 - SCR_H/2);
	sceGuViewport(2048 - SCR_W/2 + x + w/2, 2048 - SCR_H/2 + y + h/2, w, h);
	sceGuScissor(x, y, x+w, y+h);
	sceGuEnable(GU_SCISSOR_TEST);
}

static void
startList(void)
{
	if(listOpen)
		return;
	sceGuStart(GU_DIRECT, displayList);
	listOpen = true;
	sceGuDepthRange(65535, 0);
	sceGuEnable(GU_CLIP_PLANES);
	sceGuShadeModel(GU_SMOOTH);
	applyAllState();
}

static void
beginUpdate(Camera *cam)
{
	startList();

	float view[16], proj[16];
	// View Matrix
	Matrix inv;
	Matrix::invert(&inv, cam->getFrame()->getLTM());
	// Since we're looking into positive Z,
	// flip X to ge a left handed view space.
	view[0]  = -inv.right.x;
	view[1]  =  inv.right.y;
	view[2]  =  inv.right.z;
	view[3]  =  0.0f;
	view[4]  = -inv.up.x;
	view[5]  =  inv.up.y;
	view[6]  =  inv.up.z;
	view[7]  =  0.0f;
	view[8]  =  -inv.at.x;
	view[9]  =   inv.at.y;
	view[10] =  inv.at.z;
	view[11] =  0.0f;
	view[12] = -inv.pos.x;
	view[13] =  inv.pos.y;
	view[14] =  inv.pos.z;
	view[15] =  1.0f;
	memcpy(&cam->devView, &view, sizeof(RawMatrix));
	sceGuSetMatrix(GU_VIEW, (ScePspFMatrix4*)view);

	// Projection Matrix
	float32 invwx = 1.0f/cam->viewWindow.x;
	float32 invwy = 1.0f/cam->viewWindow.y;
	float32 invz = 1.0f/(cam->farPlane-cam->nearPlane);

	proj[0] = invwx;
	proj[1] = 0.0f;
	proj[2] = 0.0f;
	proj[3] = 0.0f;

	proj[4] = 0.0f;
	proj[5] = invwy;
	proj[6] = 0.0f;
	proj[7] = 0.0f;

	proj[8] = cam->viewOffset.x*invwx;
	proj[9] = cam->viewOffset.y*invwy;
	proj[12] = -proj[8];
	proj[13] = -proj[9];
	if(cam->projection == Camera::PERSPECTIVE){
		proj[10] = (cam->farPlane+cam->nearPlane)*invz;
		proj[11] = 1.0f;

		proj[14] = -2.0f*cam->nearPlane*cam->farPlane*invz;
		proj[15] = 0.0f;
	}else{
		proj[10] = 2.0f*invz;
		proj[11] = 0.0f;

		proj[14] = -(cam->farPlane+cam->nearPlane)*invz;
		proj[15] = 1.0f;
	}
	memcpy(&cam->devProj, &proj, sizeof(RawMatrix));
	sceGuSetMatrix(GU_PROJECTION, (ScePspFMatrix4*)proj);

	rwStateCache.fogStart = cam->fogPlane;
	rwStateCache.fogEnd = cam->farPlane;
	applyFog();

	setViewport(cam->frameBuffer);
}

static void
endUpdate(Camera *cam)
{
}

static void
clearCamera(Camera *cam, RGBA *col, uint32 mode)
{
	startList();
	setViewport(cam->frameBuffer);

	int flags = 0;
	if(mode & Camera::CLEARIMAGE)
		flags |= GU_COLOR_BUFFER_BIT;
	if(mode & Camera::CLEARZ)
		flags |= GU_DEPTH_BUFFER_BIT;
	if(mode & Camera::CLEARSTENCIL)
		flags |= GU_STENCIL_BUFFER_BIT;
	sceGuClearColor(packABGR(*col));
	sceGuClearDepth(0);	// depth is reversed, 0 is far
	sceGuClear(flags);
}

static void
showRaster(Raster *raster, uint32 flags)
{
	if(!listOpen)
		return;
	// the GE reads straight from RAM
	flushCache();
	sceGuFinish();
	sceGuSync(0, 0);
	listOpen = false;
	if(flags & Raster::FLIPWAITVSYNCH)
		sceDisplayWaitVblankStart();
	sceGuSwapBuffers();
}

static bool32
rasterRenderFast(Raster *raster, int32 x, int32 y)
{
	return 0;
}

// Device system

static bool
initGU(void)
{
	sceGuInit();
	sceGuStart(GU_DIRECT, displayList);
	sceGuDrawBuffer(GU_PSM_8888, (void*)FB_DRAW_OFFS, BUF_W);
	sceGuDispBuffer(SCR_W, SCR_H, (void*)FB_DISP_OFFS, BUF_W);
	sceGuDepthBuffer((void*)FB_DEPTH_OFFS, BUF_W);
	sceGuOffset(2048 - SCR_W/2, 2048 - SCR_H/2);
	sceGuViewport(2048, 2048, SCR_W, SCR_H);
	sceGuScissor(0, 0, SCR_W, SCR_H);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDepthRange(65535, 0);
	sceGuFinish();
	sceGuSync(0, 0);
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
	listOpen = false;
	return true;
}

static int
deviceSystem(DeviceReq req, void *arg, int32 n)
{
	switch(req){
	case DEVICEOPEN:
	case DEVICECLOSE:
		return 1;

	case DEVICEINIT:
		resetRenderState();
		return initGU();
	case DEVICETERM:
		sceGuTerm();
		return 1;

	case DEVICEFINALIZE:
		openIm2D();
		openIm3D();
		return 1;

	case DEVICEGETNUMSUBSYSTEMS:
		return 1;
	case DEVICEGETCURRENTSUBSYSTEM:
		return 0;
	case DEVICESETSUBSYSTEM:
		return n == 0;
	case DEVICEGETSUBSSYSTEMINFO:
		strncpy(((SubSystemInfo*)arg)->name, "PSP GE", sizeof(SubSystemInfo::name));
		return 1;

	case DEVICEGETNUMVIDEOMODES:
		return 1;
	case DEVICEGETCURRENTVIDEOMODE:
		return 0;
	case DEVICESETVIDEOMODE:
		return n == 0;
	case DEVICEGETVIDEOMODEINFO: {
		VideoMode *m = (VideoMode*)arg;
		m->width = SCR_W;
		m->height = SCR_H;
		m->depth = 32;
		m->flags = VIDEOMODEEXCLUSIVE;
		return 1;
	}

	case DEVICEGETMAXMULTISAMPLINGLEVELS:
	case DEVICEGETMULTISAMPLINGLEVELS:
		return 1;
	case DEVICESETMULTISAMPLINGLEVELS:
		return 1;
	}
	return 0;
}

Device renderdevice = {
	-1.0f, 1.0f,
	psp::beginUpdate,
	psp::endUpdate,
	psp::clearCamera,
	psp::showRaster,
	psp::rasterRenderFast,
	psp::setRenderState,
	psp::getRenderState,
	psp::im2DRenderLine,
	psp::im2DRenderTriangle,
	psp::im2DRenderPrimitive,
	psp::im2DRenderIndexedPrimitive,
	psp::im3DTransform,
	psp::im3DRenderPrimitive,
	psp::im3DRenderIndexedPrimitive,
	psp::im3DEnd,
	psp::deviceSystem
};

}
}

#endif
