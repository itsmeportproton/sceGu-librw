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
#include <pspgu.h>

// All immediate mode data is copied into the display list (sceGuGetMemory)
// in the GE's vertex layout, so nothing has to be kept around.

namespace rw {
namespace psp {

static const int primTypeMap[] = {
	GU_POINTS,	// invalid
	GU_LINES,
	GU_LINE_STRIP,
	GU_TRIANGLES,
	GU_TRIANGLE_STRIP,
	GU_TRIANGLE_FAN,
	GU_POINTS
};

void openIm2D(void) {}
void closeIm2D(void) {}
void openIm3D(void) {}
void closeIm3D(void) {}

// GE 2D vertex: texcoord, color, position
struct GeVertex2D
{
	float u, v;
	uint32 color;
	float x, y, z;
};

// GE 3D vertex with normals: texcoord, color, normal, position
struct GeVertex3D
{
	float u, v;
	uint32 color;
	float nx, ny, nz;
	float x, y, z;
};

static const uint32 vtype2D = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D;
static const uint32 vtype3D = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D;

static inline uint32
packColor(uint8 r, uint8 g, uint8 b, uint8 a)
{
	return r | g<<8 | b<<16 | a<<24;
}

static uint16*
copyIndices(void *indices, int32 numIndices)
{
	uint16 *dst = (uint16*)getListMemory((numIndices*2 + 3) & ~3);
	memcpy(dst, indices, numIndices*2);
	return dst;
}

static void
setup2D(void)
{
	setLighting(0, 0);
	applyTexture();
}

static GeVertex2D*
convert2D(void *vertices, int32 numVertices)
{
	Im2DVertex *src = (Im2DVertex*)vertices;
	GeVertex2D *dst = (GeVertex2D*)getListMemory(numVertices*sizeof(GeVertex2D));
	float zn = renderdevice.zNear, zf = renderdevice.zFar;
	for(int32 i = 0; i < numVertices; i++){
		// depth is reversed on the GE: near = 65535
		float z = 65535.0f*(1.0f - (src[i].z - zn)/(zf - zn));
		dst[i].u = src[i].u;
		dst[i].v = src[i].v;
		dst[i].color = packColor(src[i].r, src[i].g, src[i].b, src[i].a);
		dst[i].x = src[i].x;
		dst[i].y = src[i].y;
		dst[i].z = z < 0.0f ? 0.0f : z > 65535.0f ? 65535.0f : z;
	}
	return dst;
}

void
im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
	Im2DVertex *verts = (Im2DVertex*)vertices;
	Im2DVertex tmp[2] = { verts[vert1], verts[vert2] };
	im2DRenderPrimitive(PRIMTYPELINELIST, tmp, 2);
}

void
im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2, int32 vert3)
{
	Im2DVertex *verts = (Im2DVertex*)vertices;
	Im2DVertex tmp[3] = { verts[vert1], verts[vert2], verts[vert3] };
	im2DRenderPrimitive(PRIMTYPETRILIST, tmp, 3);
}

void
im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices)
{
	setup2D();
	GeVertex2D *v = convert2D(vertices, numVertices);
	sceGuDrawArray(primTypeMap[primType], vtype2D, numVertices, nil, v);
}

void
im2DRenderIndexedPrimitive(PrimitiveType primType,
	void *vertices, int32 numVertices,
	void *indices, int32 numIndices)
{
	setup2D();
	GeVertex2D *v = convert2D(vertices, numVertices);
	uint16 *idx = copyIndices(indices, numIndices);
	sceGuDrawArray(primTypeMap[primType], vtype2D | GU_INDEX_16BIT, numIndices, idx, v);
}

// Im3D

RGBA im3dMaterialColor = { 255, 255, 255, 255 };
SurfaceProperties im3dSurfaceProps = { 1.0f, 1.0f, 1.0f };

static GeVertex3D *im3dVertices;
static int32 num3DVertices;
static bool im3dLit;

void
im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	if(world == nil){
		static Matrix ident;
		ident.setIdentity();
		world = &ident;
	}
	setWorldMatrix(world);

	im3dLit = !!(flags & im3d::LIGHTING);
	if(im3dLit){
		setMaterial(Geometry::MODULATE, im3dMaterialColor);
		lightingCB();
	}
	// vertex colors modulate the lit color instead of replacing it
	setLighting(im3dLit, im3dLit);

	if((flags & im3d::VERTEXUV) == 0)
		SetRenderStatePtr(TEXTURERASTER, nil);

	Im3DVertex *src = (Im3DVertex*)vertices;
	im3dVertices = (GeVertex3D*)getListMemory(numVertices*sizeof(GeVertex3D));
	for(int32 i = 0; i < numVertices; i++){
		GeVertex3D *d = &im3dVertices[i];
		d->u = src[i].u;
		d->v = src[i].v;
		d->color = packColor(src[i].r, src[i].g, src[i].b, src[i].a);
		d->nx = src[i].normal.x;
		d->ny = src[i].normal.y;
		d->nz = src[i].normal.z;
		d->x = src[i].position.x;
		d->y = src[i].position.y;
		d->z = src[i].position.z;
	}
	num3DVertices = numVertices;
}

void
im3DRenderPrimitive(PrimitiveType primType)
{
	applyTexture();
	sceGuDrawArray(primTypeMap[primType], vtype3D, num3DVertices, nil, im3dVertices);
}

void
im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices)
{
	applyTexture();
	uint16 *idx = copyIndices(indices, numIndices);
	sceGuDrawArray(primTypeMap[primType], vtype3D | GU_INDEX_16BIT, numIndices, idx, im3dVertices);
}

void
im3DEnd(void)
{
	im3dVertices = nil;
}

}
}

#endif
