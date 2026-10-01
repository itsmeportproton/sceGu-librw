// PSP backend (sceGu). Only the GU parts are guarded by RW_PSP;
// everything declared here also exists (as stubs) in other builds,
// just like gl3 does, so the platform-independent code can reference it.

namespace rw {

#ifdef RW_PSP
struct EngineOpenParams
{
};
#endif

namespace psp {

void registerPlatformPlugins(void);
void initSkin(void);
void initMatFX(void);

extern Device renderdevice;

// Vertex structs as used by the platform independent API (im2d/im3d).
// They are repacked into the GE's own vertex layout when drawn.
struct Im3DVertex
{
	V3d     position;
	V3d     normal;		// librw extension
	uint8   r, g, b, a;
	float32 u, v;

	void setX(float32 x) { this->position.x = x; }
	void setY(float32 y) { this->position.y = y; }
	void setZ(float32 z) { this->position.z = z; }
	void setNormalX(float32 x) { this->normal.x = x; }
	void setNormalY(float32 y) { this->normal.y = y; }
	void setNormalZ(float32 z) { this->normal.z = z; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u) { this->u = u; }
	void setV(float32 v) { this->v = v; }

	float getX(void) { return this->position.x; }
	float getY(void) { return this->position.y; }
	float getZ(void) { return this->position.z; }
	float getNormalX(void) { return this->normal.x; }
	float getNormalY(void) { return this->normal.y; }
	float getNormalZ(void) { return this->normal.z; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};

struct Im2DVertex
{
	float32 x, y, z, w;
	uint8   r, g, b, a;
	float32 u, v;

	void setScreenX(float32 x) { this->x = x; }
	void setScreenY(float32 y) { this->y = y; }
	void setScreenZ(float32 z) { this->z = z; }
	void setCameraZ(float32 z) { this->w = z; }
	void setRecipCameraZ(float32 recipz) { this->w = 1.0f/recipz; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u, float recipz) { this->u = u; }
	void setV(float32 v, float recipz) { this->v = v; }

	float getScreenX(void) { return this->x; }
	float getScreenY(void) { return this->y; }
	float getScreenZ(void) { return this->z; }
	float getCameraZ(void) { return this->w; }
	float getRecipCameraZ(void) { return 1.0f/this->w; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};

// Instanced geometry
//
// The GE takes a fixed vertex layout: [weights][texcoord][color][normal][position]
// All components are 32 bit float except the 8888 color, so every vertex is
// 4 byte aligned and the stride is a multiple of 4.
//
// A draw is a InstanceData. Normally that is one per Mesh and all of them
// share the header's vertex/index buffer. The skin pipeline has to split
// meshes into batches of at most 8 bones (the GE has 8 bone matrices and
// weight n always uses matrix n), and every batch owns its vertices.

enum { MAXBONES = 8 };

struct InstanceData
{
	uint32    numIndex;
	uint32    minVert;	// not used for rendering
	int32     numVertices;
	Material *material;
	bool32    vertexAlpha;
	uint16   *indices;
	uint8    *vertices;
	// skin batches only
	int32     numBones;			// weights per vertex, 0 = not skinned
	uint8     boneMap[MAXBONES];		// GE matrix slot -> skin bone index
};

struct InstanceDataHeader : rw::InstanceDataHeader
{
	uint32      serialNumber;
	uint32      numMeshes;
	uint32      primType;		// GU_TRIANGLES or GU_TRIANGLE_STRIP
	uint32      totalNumIndex;
	uint32      totalNumVertex;
	uint16     *indexBuffer;
	uint8      *vertexBuffer;
	uint32      stride;
	uint32      vtype;		// GU vertex type without weights and index bits
	uint32      numInst;		// == numMeshes unless skin batches were made
	bool32      ownsVertices;	// inst[i].vertices are allocated individually
	InstanceData *inst;
};

// Offsets of the components inside a vertex; weightBytes are in front of them
struct VertexLayout
{
	uint32 uv, color, normal, pos, size;
	uint32 vtype;
};
VertexLayout makeLayout(Geometry *geo, uint32 weightBytes);

class ObjPipeline : public rw::ObjPipeline
{
public:
	void init(void);
	static ObjPipeline *create(void);

	void (*instanceCB)(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
	void (*uninstanceCB)(Geometry *geo, InstanceDataHeader *header);
	void (*renderCB)(Atomic *atomic, InstanceDataHeader *header);
};

void *destroyNativeData(void *object, int32, int32);

// Native Texture and Raster

struct PspRaster
{
	int32 psm;		// GU pixel format
	int32 bpp;		// bytes per pixel
	bool  hasAlpha;
	int8  numLevels;
	uint8 *base;		// all levels, 16 byte aligned
	uint32 size;
};

extern int32 nativeRasterOffset;
#define GETPSPRASTEREXT(raster) PLUGINOFFSET(rw::psp::PspRaster, raster, rw::psp::nativeRasterOffset)

void registerNativeRaster(void);
Texture *readNativeTexture(Stream *stream);
void writeNativeTexture(Texture *tex, Stream *stream);
uint32 getSizeNativeTexture(Texture *tex);

#ifdef RW_PSP

// Driver <-> device
Raster *rasterCreate(Raster *raster);
uint8 *rasterLock(Raster*, int32 level, int32 lockMode);
void rasterUnlock(Raster*, int32);
int32 rasterNumLevels(Raster*);
bool32 imageFindRasterFormat(Image *img, int32 type,
	int32 *width, int32 *height, int32 *depth, int32 *format);
bool32 rasterFromImage(Raster *raster, Image *image);
Image *rasterToImage(Raster *raster);

// Device state used by pipelines and immediate mode
void setWorldMatrix(Matrix *mat);
int32 setLights(WorldLights *lightData);	// returns bits below
enum { LIGHT_ACTIVE = 1 };
void setMaterial(uint32 geoflags, const RGBA &color);
void setTexture(Texture *tex);
bool32 getAlphaBlend(void);
void applyTexture(void);			// flush texture state to the GE
void setLighting(bool32 enable, bool32 vertexColors);
int32 lightingCB(Atomic *atomic);
int32 lightingCB(void);
void flushCache(void);				// write back CPU cache (GE reads RAM)
void *getListMemory(uint32 size);		// scratch memory inside the display list

// Immediate mode
extern RGBA im3dMaterialColor;		// settable by user, used for lit im3d
extern SurfaceProperties im3dSurfaceProps;
void openIm2D(void);
void closeIm2D(void);
void im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2);
void im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2, int32 vert3);
void im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices);
void im2DRenderIndexedPrimitive(PrimitiveType primType, void *vertices, int32 numVertices, void *indices, int32 numIndices);
void openIm3D(void);
void closeIm3D(void);
void im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags);
void im3DRenderPrimitive(PrimitiveType primType);
void im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices);
void im3DEnd(void);

// Pipelines
ObjPipeline *makeDefaultPipeline(void);
ObjPipeline *makeSkinPipeline(void);
void defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
void defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header);
void defaultRenderCB(Atomic *atomic, InstanceDataHeader *header);
void skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
void skinRenderCB(Atomic *atomic, InstanceDataHeader *header);
void drawInst(InstanceDataHeader *header, InstanceData *inst);
void drawInst_simple(InstanceDataHeader *header, InstanceData *inst);
void setupMeshState(uint32 geoflags, InstanceData *inst);

#endif

}
}
