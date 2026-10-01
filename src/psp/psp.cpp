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

namespace rw {
namespace psp {

#ifdef RW_PSP

static void*
driverOpen(void *o, int32, int32)
{
	engine->driver[PLATFORM_PSP]->defaultPipeline = makeDefaultPipeline();
	engine->driver[PLATFORM_PSP]->rasterNativeOffset = nativeRasterOffset;
	engine->driver[PLATFORM_PSP]->rasterCreate       = rasterCreate;
	engine->driver[PLATFORM_PSP]->rasterLock         = rasterLock;
	engine->driver[PLATFORM_PSP]->rasterUnlock       = rasterUnlock;
	engine->driver[PLATFORM_PSP]->rasterNumLevels    = rasterNumLevels;
	engine->driver[PLATFORM_PSP]->imageFindRasterFormat = imageFindRasterFormat;
	engine->driver[PLATFORM_PSP]->rasterFromImage    = rasterFromImage;
	engine->driver[PLATFORM_PSP]->rasterToImage      = rasterToImage;
	return o;
}

static void*
driverClose(void *o, int32, int32)
{
	return o;
}

void
registerPlatformPlugins(void)
{
	Driver::registerPlugin(PLATFORM_PSP, 0, PLATFORM_PSP,
	                       driverOpen, driverClose);
	registerNativeRaster();
}

#else

// Stubs so that the rest of librw links on other platforms.

Device renderdevice;

void registerPlatformPlugins(void) {}
void initSkin(void) {}
void initMatFX(void) {}
void *destroyNativeData(void *object, int32, int32) { return object; }
Texture *readNativeTexture(Stream*) { return nil; }
void writeNativeTexture(Texture*, Stream*) {}
uint32 getSizeNativeTexture(Texture*) { return 0; }

#endif

}
}
