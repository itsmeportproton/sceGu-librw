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
#include "../rwanim.h"
#include "../rwplugins.h"

#include "rwpsp.h"

#ifdef RW_PSP

#include <pspkernel.h>
#include <pspgu.h>

// Hardware skinning on the GE:
// There are 8 bone matrices and a vertex has n weights; weight k always
// blends matrix k. RW skins have up to 4 (bone index, weight) pairs per
// vertex and an arbitrary number of bones, so at instance time every mesh
// is cut into batches of triangles that use at most 8 different bones.
// Each batch gets its own vertex buffer with the weights sorted into the
// slots of the batch's bone map. Vertices shared between batches are
// duplicated.

namespace rw {
namespace psp {

struct Batch
{
	int32 numBones;
	uint8 bones[MAXBONES];
	uint32 firstTri;	// into the triangle list of the mesh
	uint32 numTris;
};

static int32
findBone(Batch *b, uint8 bone)
{
	for(int32 i = 0; i < b->numBones; i++)
		if(b->bones[i] == bone)
			return i;
	return -1;
}

// Bones used by a vertex, strongest first is not needed: RW stores 4 slots,
// unused ones have weight 0.
static int32
vertexBones(Skin *skin, uint32 v, uint8 *bones)
{
	int32 n = 0;
	for(int32 k = 0; k < 4; k++)
		if(skin->weights[v*4+k] > 0.0f)
			bones[n++] = skin->indices[v*4+k];
	return n;
}

// Append the bones of triangle (a,b,c) to the batch. Returns false and
// leaves the batch untouched if they don't fit (unless the batch is empty,
// then the extra bones are dropped).
static bool
addTriangle(Batch *b, Skin *skin, uint32 a, uint32 bb, uint32 c)
{
	uint8 need[12];
	int32 n = 0;
	uint32 vs[3] = { a, bb, c };
	for(int i = 0; i < 3; i++)
		n += vertexBones(skin, vs[i], need + n);

	// count bones that are not in the batch yet
	uint8 fresh[12];
	int32 nf = 0;
	for(int32 i = 0; i < n; i++){
		if(findBone(b, need[i]) >= 0)
			continue;
		bool dup = false;
		for(int32 j = 0; j < nf; j++)
			dup |= fresh[j] == need[i];
		if(!dup)
			fresh[nf++] = need[i];
	}
	if(b->numBones + nf > MAXBONES && b->numTris > 0)
		return false;
	for(int32 i = 0; i < nf && b->numBones < MAXBONES; i++)
		b->bones[b->numBones++] = fresh[i];
	return true;
}

void
skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	Skin *skin = Skin::get(geo);
	assert(skin);

	// Always rebuild from scratch. Skinned geometry is rarely locked.
	if(header->ownsVertices)
		for(uint32 i = 0; i < header->numInst; i++){
			rwFree(header->inst[i].vertices);
			rwFree(header->inst[i].indices);
		}
	rwFree(header->inst);
	header->inst = nil;
	header->ownsVertices = 0;

	MeshHeader *meshh = geo->meshHeader;
	bool strip = meshh->flags == MeshHeader::TRISTRIP;
	Mesh *meshes = meshh->getMeshes();
	header->primType = GU_TRIANGLES;
	header->vtype = 0;
	header->stride = 0;

	// the shared index buffer from instanceMesh is left alone, only the
	// InstanceData array is replaced
	uint32 numVerts = geo->numVertices;
	int32 *local = rwNewT(int32, numVerts, MEMDUR_FUNCTION | ID_GEOMETRY);
	uint32 *stamp = rwNewT(uint32, numVerts, MEMDUR_FUNCTION | ID_GEOMETRY);
	memset(stamp, 0xFF, numVerts*sizeof(uint32));
	uint32 stampId = 0;

	// worst case every triangle is its own batch
	uint32 maxTris = 0;
	for(uint32 m = 0; m < header->numMeshes; m++)
		maxTris += strip ? (meshes[m].numIndices > 2 ? meshes[m].numIndices-2 : 0)
		                 : meshes[m].numIndices/3;
	InstanceData *insts = rwNewT(InstanceData, maxTris + 1, MEMDUR_EVENT | ID_GEOMETRY);
	uint32 numInst = 0;

	for(uint32 m = 0; m < header->numMeshes; m++){
		Mesh *mesh = &meshes[m];

		// Triangle list of this mesh
		uint32 numTris = strip ? (mesh->numIndices > 2 ? mesh->numIndices-2 : 0)
		                       : mesh->numIndices/3;
		uint16 *tris = rwNewT(uint16, numTris*3 + 3, MEMDUR_FUNCTION | ID_GEOMETRY);
		uint32 nt = 0;
		for(uint32 i = 0; i < numTris; i++){
			uint16 a, b, c;
			if(strip){
				a = mesh->indices[i];
				b = mesh->indices[i+1];
				c = mesh->indices[i+2];
				if(a == b || b == c || a == c)
					continue;	// degenerate
				if(i & 1){ uint16 t = a; a = b; b = t; }
			}else{
				a = mesh->indices[i*3];
				b = mesh->indices[i*3+1];
				c = mesh->indices[i*3+2];
			}
			tris[nt*3] = a; tris[nt*3+1] = b; tris[nt*3+2] = c;
			nt++;
		}

		// Cut into batches
		uint32 t = 0;
		while(t < nt){
			Batch batch;
			memset(&batch, 0, sizeof(batch));
			batch.firstTri = t;
			while(t < nt && addTriangle(&batch, skin, tris[t*3], tris[t*3+1], tris[t*3+2])){
				batch.numTris++;
				t++;
			}
			assert(batch.numTris > 0);
			if(batch.numBones == 0)		// only unweighted vertices
				batch.bones[batch.numBones++] = 0;

			// Build the batch: vertices first
			stampId++;
			uint32 nv = 0;
			uint16 *bidx = rwNewT(uint16, batch.numTris*3, MEMDUR_EVENT | ID_GEOMETRY);
			for(uint32 i = 0; i < batch.numTris*3; i++){
				uint32 v = tris[batch.firstTri*3 + i];
				if(stamp[v] != stampId){
					stamp[v] = stampId;
					local[v] = nv++;
				}
				bidx[i] = local[v];
			}
			uint32 wbytes = batch.numBones*4;
			VertexLayout l = makeLayout(geo, wbytes);
			uint8 *vbuf = rwNewT(uint8, nv*l.size, MEMDUR_EVENT | ID_GEOMETRY);
			memset(vbuf, 0, nv*l.size);
			uint32 minV = 0xFFFFFFFF, maxV = 0;
			stampId++;	// reuse the stamps to write each vertex once
			for(uint32 i = 0; i < batch.numTris*3; i++){
				uint32 v = tris[batch.firstTri*3 + i];
				if(stamp[v] == stampId)
					continue;
				stamp[v] = stampId;
				if(v < minV) minV = v;
				if(v > maxV) maxV = v;
				uint8 *dst = vbuf + local[v]*l.size;

				// weights, renormalized over the bones that made it into the batch
				float *w = (float*)dst;
				float sum = 0.0f;
				for(int32 k = 0; k < 4; k++){
					float wk = skin->weights[v*4+k];
					if(wk <= 0.0f)
						continue;
					int32 slot = findBone(&batch, skin->indices[v*4+k]);
					if(slot < 0)
						continue;
					w[slot] += wk;
					sum += wk;
				}
				if(sum > 0.0f)
					for(int32 s = 0; s < batch.numBones; s++)
						w[s] /= sum;
				else
					w[0] = 1.0f;

				if(geo->numTexCoordSets > 0)
					memcpy(dst + l.uv, &geo->texCoords[0][v], 8);
				if(geo->flags & Geometry::PRELIT){
					RGBA *c = &geo->colors[v];
					dst[l.color+0] = c->red;
					dst[l.color+1] = c->green;
					dst[l.color+2] = c->blue;
					dst[l.color+3] = c->alpha;
				}
				if(geo->flags & Geometry::NORMALS)
					memcpy(dst + l.normal, &geo->morphTargets[0].normals[v], 12);
				memcpy(dst + l.pos, &geo->morphTargets[0].vertices[v], 12);
			}
			sceKernelDcacheWritebackRange(vbuf, nv*l.size);
			sceKernelDcacheWritebackRange(bidx, batch.numTris*3*2);

			InstanceData *inst = &insts[numInst++];
			inst->numIndex = batch.numTris*3;
			inst->minVert = minV;
			inst->numVertices = nv;
			inst->material = mesh->material;
			inst->vertexAlpha = 0;
			if(geo->flags & Geometry::PRELIT)
				for(uint32 i = 0; i < nv && !inst->vertexAlpha; i++)
					inst->vertexAlpha = vbuf[i*l.size + l.color + 3] != 0xFF;
			inst->indices = bidx;
			inst->vertices = vbuf;
			inst->numBones = batch.numBones;
			memcpy(inst->boneMap, batch.bones, sizeof(inst->boneMap));
			// identical for all batches, the weights are added in drawInst_simple
			header->vtype = l.vtype;
		}
		rwFree(tris);
	}
	rwFree(local);
	rwFree(stamp);

	header->inst = insts;
	header->numInst = numInst;
	header->ownsVertices = 1;
}

static ScePspFMatrix4 skinMatrices[256];

static void
makeSkinMatrices(Atomic *a)
{
	Skin *skin = Skin::get(a->geometry);
	HAnimHierarchy *hier = Skin::getHierarchy(a);
	int32 n = skin->numBones > 256 ? 256 : skin->numBones;
	Matrix tmp, res;

	if(hier){
		Matrix *invMats = (Matrix*)skin->inverseMatrices;
		assert(skin->numBones == hier->numNodes);
		Matrix invAtmMat;
		if(!(hier->flags & HAnimHierarchy::LOCALSPACEMATRICES))
			Matrix::invert(&invAtmMat, a->getFrame()->getLTM());
		for(int32 i = 0; i < n; i++){
			invMats[i].flags = 0;
			if(hier->flags & HAnimHierarchy::LOCALSPACEMATRICES)
				Matrix::mult(&res, &invMats[i], &hier->matrices[i]);
			else{
				Matrix::mult(&tmp, &hier->matrices[i], &invAtmMat);
				Matrix::mult(&res, &invMats[i], &tmp);
			}
			float *f = (float*)&skinMatrices[i];
			memcpy(f, &res, 64);
			f[3] = f[7] = f[11] = 0.0f;
			f[15] = 1.0f;
		}
	}else{
		res.setIdentity();
		for(int32 i = 0; i < n; i++){
			float *f = (float*)&skinMatrices[i];
			memcpy(f, &res, 64);
			f[3] = f[7] = f[11] = 0.0f;
			f[15] = 1.0f;
		}
	}
}

void
skinRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	lightingCB(atomic);
	makeSkinMatrices(atomic);

	InstanceData *inst = header->inst;
	for(uint32 n = header->numInst; n--; inst++){
		for(int32 s = 0; s < inst->numBones; s++)
			sceGuBoneMatrix(s, &skinMatrices[inst->boneMap[s]]);
		setupMeshState(flags, inst);
		drawInst(header, inst);
	}
}

static void
skinUninstanceCB(Geometry *geo, InstanceDataHeader *header)
{
	assert(0 && "can't uninstance");
}

ObjPipeline*
makeSkinPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = skinInstanceCB;
	pipe->uninstanceCB = skinUninstanceCB;
	pipe->renderCB = skinRenderCB;
	pipe->pluginID = ID_SKIN;
	pipe->pluginData = 1;
	return pipe;
}

static void*
skinOpen(void *o, int32, int32)
{
	skinGlobals.pipelines[PLATFORM_PSP] = makeSkinPipeline();
	return o;
}

static void*
skinClose(void *o, int32, int32)
{
	((ObjPipeline*)skinGlobals.pipelines[PLATFORM_PSP])->destroy();
	skinGlobals.pipelines[PLATFORM_PSP] = nil;
	return o;
}

void
initSkin(void)
{
	Driver::registerPlugin(PLATFORM_PSP, 0, ID_SKIN, skinOpen, skinClose);
}

}
}

#endif
