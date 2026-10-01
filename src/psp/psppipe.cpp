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
#include "../rwplugins.h"

#include "rwpsp.h"

#ifdef RW_PSP

#include <pspkernel.h>
#include <pspgu.h>

namespace rw {
namespace psp {

static void
freeInstanceData(Geometry *geometry)
{
	if(geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_PSP)
		return;
	InstanceDataHeader *header = (InstanceDataHeader*)geometry->instData;
	geometry->instData = nil;
	if(header->ownsVertices)
		for(uint32 i = 0; i < header->numInst; i++){
			rwFree(header->inst[i].vertices);
			rwFree(header->inst[i].indices);
		}
	rwFree(header->indexBuffer);
	rwFree(header->vertexBuffer);
	rwFree(header->inst);
	rwFree(header);
}

void*
destroyNativeData(void *object, int32, int32)
{
	freeInstanceData((Geometry*)object);
	return object;
}

static InstanceDataHeader*
instanceMesh(rw::ObjPipeline *rwpipe, Geometry *geo)
{
	InstanceDataHeader *header = rwNewT(InstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
	MeshHeader *meshh = geo->meshHeader;
	geo->instData = header;
	header->platform = PLATFORM_PSP;

	header->serialNumber = meshh->serialNum;
	header->numMeshes = meshh->numMeshes;
	header->numInst = meshh->numMeshes;
	header->primType = meshh->flags == MeshHeader::TRISTRIP ? GU_TRIANGLE_STRIP : GU_TRIANGLES;
	header->totalNumVertex = geo->numVertices;
	header->totalNumIndex = meshh->totalIndices;
	header->ownsVertices = 0;
	header->vertexBuffer = nil;
	header->stride = 0;
	header->vtype = 0;
	header->inst = rwNewT(InstanceData, header->numMeshes, MEMDUR_EVENT | ID_GEOMETRY);

	header->indexBuffer = rwNewT(uint16, header->totalNumIndex, MEMDUR_EVENT | ID_GEOMETRY);
	InstanceData *inst = header->inst;
	Mesh *mesh = meshh->getMeshes();
	uint32 offset = 0;
	for(uint32 i = 0; i < header->numMeshes; i++){
		findMinVertAndNumVertices(mesh->indices, mesh->numIndices,
		                          &inst->minVert, &inst->numVertices);
		assert(inst->minVert != 0xFFFFFFFF);
		inst->numIndex = mesh->numIndices;
		inst->material = mesh->material;
		inst->vertexAlpha = 0;
		inst->numBones = 0;
		inst->indices = header->indexBuffer + offset;
		inst->vertices = nil;
		memcpy(inst->indices, mesh->indices, inst->numIndex*2);
		offset += inst->numIndex;
		mesh++;
		inst++;
	}
	sceKernelDcacheWritebackRange(header->indexBuffer, header->totalNumIndex*2);
	return header;
}

static void
instance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	// don't try to (re)instance native data
	if(geo->flags & Geometry::NATIVE)
		return;

	InstanceDataHeader *header = (InstanceDataHeader*)geo->instData;
	if(geo->instData){
		// Already have instanced data, so check if we have to reinstance
		assert(header->platform == PLATFORM_PSP);
		if(header->serialNumber != geo->meshHeader->serialNum){
			// Mesh changed, so reinstance everything
			freeInstanceData(geo);
		}
	}

	// no instance or complete reinstance
	if(geo->instData == nil){
		geo->instData = instanceMesh(rwpipe, geo);
		pipe->instanceCB(geo, (InstanceDataHeader*)geo->instData, 0);
	}else if(geo->lockedSinceInst)
		pipe->instanceCB(geo, (InstanceDataHeader*)geo->instData, 1);

	geo->lockedSinceInst = 0;
}

static void
uninstance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	assert(0 && "can't uninstance");
}

static void
render(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	pipe->instance(atomic);
	assert(geo->instData != nil);
	assert(geo->instData->platform == PLATFORM_PSP);
	if(pipe->renderCB)
		pipe->renderCB(atomic, (InstanceDataHeader*)geo->instData);
}

void
ObjPipeline::init(void)
{
	this->rw::ObjPipeline::init(PLATFORM_PSP);
	this->impl.instance = psp::instance;
	this->impl.uninstance = psp::uninstance;
	this->impl.render = psp::render;
	this->instanceCB = nil;
	this->uninstanceCB = nil;
	this->renderCB = nil;
}

ObjPipeline*
ObjPipeline::create(void)
{
	ObjPipeline *pipe = rwNewT(ObjPipeline, 1, MEMDUR_GLOBAL);
	pipe->init();
	return pipe;
}

// Default pipeline

// Shared between default and skin: vertex layout for the GE
VertexLayout
makeLayout(Geometry *geo, uint32 weightBytes)
{
	VertexLayout l;
	uint32 off = weightBytes;
	l.vtype = GU_VERTEX_32BITF;
	l.uv = l.color = l.normal = 0;
	if(geo->numTexCoordSets > 0){
		l.vtype |= GU_TEXTURE_32BITF;
		l.uv = off;
		off += 8;
	}
	if(geo->flags & Geometry::PRELIT){
		l.vtype |= GU_COLOR_8888;
		l.color = off;
		off += 4;
	}
	if(geo->flags & Geometry::NORMALS){
		l.vtype |= GU_NORMAL_32BITF;
		l.normal = off;
		off += 12;
	}
	l.pos = off;
	l.size = off + 12;
	return l;
}

void
defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	bool isPrelit = !!(geo->flags & Geometry::PRELIT);
	bool hasNormals = !!(geo->flags & Geometry::NORMALS);
	VertexLayout l = makeLayout(geo, 0);

	if(!reinstance){
		header->vtype = l.vtype;
		header->stride = l.size;
		header->vertexBuffer = rwNewT(uint8, header->totalNumVertex*l.size, MEMDUR_EVENT | ID_GEOMETRY);
		for(uint32 i = 0; i < header->numInst; i++)
			header->inst[i].vertices = header->vertexBuffer;
	}
	uint8 *verts = header->vertexBuffer;

	// Positions
	if(!reinstance || geo->lockedSinceInst&Geometry::LOCKVERTICES)
		instV3d(VERT_FLOAT3, verts + l.pos, geo->morphTargets[0].vertices,
			header->totalNumVertex, l.size);

	// Normals
	if(hasNormals && (!reinstance || geo->lockedSinceInst&Geometry::LOCKNORMALS))
		instV3d(VERT_FLOAT3, verts + l.normal, geo->morphTargets[0].normals,
			header->totalNumVertex, l.size);

	// Prelighting
	if(isPrelit && (!reinstance || geo->lockedSinceInst&Geometry::LOCKPRELIGHT)){
		InstanceData *inst = header->inst;
		int n = header->numMeshes;
		while(n--){
			assert(inst->minVert != 0xFFFFFFFF);
			inst->vertexAlpha = instColor(VERT_RGBA,
				verts + l.color + l.size*inst->minVert,
				geo->colors + inst->minVert,
				inst->numVertices, l.size);
			inst++;
		}
	}

	// Texture coordinates (first set only, the GE has one)
	if(geo->numTexCoordSets > 0 &&
	   (!reinstance || geo->lockedSinceInst&Geometry::LOCKTEXCOORDS))
		instTexCoords(VERT_FLOAT2, verts + l.uv, geo->texCoords[0],
			header->totalNumVertex, l.size);

	sceKernelDcacheWritebackRange(verts, header->totalNumVertex*l.size);
}

void
defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header)
{
	assert(0 && "can't uninstance");
}

// Emulate PS2 GS alpha test FB_ONLY case: failed alpha writes to frame- but not to depth buffer
static void
drawInst_GSemu(InstanceDataHeader *header, InstanceData *inst)
{
	if(getAlphaBlend()){
		int alphafunc = rw::GetRenderState(rw::ALPHATESTFUNC);
		if(rw::GetRenderState(rw::ZWRITEENABLE)){
			int alpharef = rw::GetRenderState(rw::ALPHATESTREF);
			int gsalpharef = rw::GetRenderState(rw::GSALPHATESTREF);

			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAGREATEREQUAL);
			SetRenderState(rw::ALPHATESTREF, gsalpharef);
			drawInst_simple(header, inst);
			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHALESS);
			SetRenderState(rw::ZWRITEENABLE, 0);
			drawInst_simple(header, inst);
			SetRenderState(rw::ZWRITEENABLE, 1);
			SetRenderState(rw::ALPHATESTFUNC, alphafunc);
			SetRenderState(rw::ALPHATESTREF, alpharef);
		}else{
			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAALWAYS);
			drawInst_simple(header, inst);
			SetRenderState(rw::ALPHATESTFUNC, alphafunc);
		}
	}else
		drawInst_simple(header, inst);
}

void
drawInst_simple(InstanceDataHeader *header, InstanceData *inst)
{
	uint32 vtype = header->vtype | GU_INDEX_16BIT | GU_TRANSFORM_3D;
	if(inst->numBones)
		vtype |= GU_WEIGHTS(inst->numBones) | GU_WEIGHT_32BITF;
	sceGuDrawArray(header->primType, vtype, inst->numIndex, inst->indices, inst->vertices);
}

void
drawInst(InstanceDataHeader *header, InstanceData *inst)
{
	if(rw::GetRenderState(rw::GSALPHATEST))
		drawInst_GSemu(header, inst);
	else
		drawInst_simple(header, inst);
}

// Everything before the draw that default and skin have in common.
void
setupMeshState(uint32 geoflags, InstanceData *inst)
{
	Material *m = inst->material;
	bool lit = (geoflags & Geometry::LIGHT) && (geoflags & Geometry::NORMALS);
	setMaterial(geoflags, m->color);
	setLighting(lit, geoflags & Geometry::PRELIT);
	setTexture(m->texture);
	rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);
	applyTexture();
}

void
defaultRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	lightingCB(atomic);

	InstanceData *inst = header->inst;
	for(uint32 n = header->numInst; n--; inst++){
		setupMeshState(flags, inst);
		drawInst(header, inst);
	}
}

ObjPipeline*
makeDefaultPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = defaultRenderCB;
	return pipe;
}

// MatFX: not implemented yet. Atomics with MatFX are drawn like normal
// ones, so env maps, bump maps and dual textures are missing.

static ObjPipeline*
makeMatFXPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = defaultRenderCB;
	pipe->pluginID = ID_MATFX;
	pipe->pluginData = 0;
	return pipe;
}

static void*
matfxOpen(void *o, int32, int32)
{
	matFXGlobals.pipelines[PLATFORM_PSP] = makeMatFXPipeline();
	return o;
}

static void*
matfxClose(void *o, int32, int32)
{
	((ObjPipeline*)matFXGlobals.pipelines[PLATFORM_PSP])->destroy();
	matFXGlobals.pipelines[PLATFORM_PSP] = nil;
	return o;
}

void
initMatFX(void)
{
	Driver::registerPlugin(PLATFORM_PSP, 0, ID_MATFX, matfxOpen, matfxClose);
}

}
}

#endif
