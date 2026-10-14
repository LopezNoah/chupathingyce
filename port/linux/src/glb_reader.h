/*
GLB_READER.H

A deliberately small glTF 2.0 binary (.glb) reader for host-local visual
assets. It accepts one subset and rejects everything else with a reason:

- one embedded BIN buffer (no external or data: URIs);
- meshes[0].primitives[0], triangles, float VEC3 POSITION, float VEC2
  TEXCOORD_0 and optional u8/u16/u32 indices (no sparse accessors);
- material: baseColorFactor/Texture, emissiveFactor/Texture, alphaMode,
  alphaCutoff, doubleSided; textures from embedded PNG or JPEG images.

Node transforms, skins, animation, extensions and other primitives are
ignored. All counts and sizes are bounded below; every byte range is checked
against the file before it is read. Pure C11: no engine or GL dependency.
*/

#ifndef GLB_READER_H
#define GLB_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum
{
	GLB_FILE_BYTES_MAX = 64 * 1024 * 1024,
	GLB_JSON_TOKENS_MAX = 8192,
	GLB_JSON_DEPTH_MAX = 32,
	GLB_VERTICES_MAX = 65536,
	GLB_INDICES_MAX = 3 * 131072,
	GLB_ERROR_LENGTH = 160,
};

enum glb_alpha_mode
{
	GLB_ALPHA_OPAQUE = 0,
	GLB_ALPHA_MASK,
	GLB_ALPHA_BLEND,
};

/* an embedded image's encoded bytes (inside the file buffer) */
struct glb_image
{
	bool present;
	const uint8_t *bytes;
	uint32_t byte_count;
};

/* Positions and UVs are copied out (heap, owned; glb_asset_free). Image
 * bytes point into the caller's file buffer, which must outlive the asset. */
struct glb_asset
{
	uint32_t vertex_count;
	uint32_t index_count;
	float *positions;   /* vertex_count * 3, glTF axes (+Y up) */
	float *texcoords;   /* vertex_count * 2 */
	uint32_t *indices;  /* index_count, each < vertex_count */
	float minimum[3];
	float maximum[3];

	float base_color_factor[4];
	float emissive_factor[3];
	enum glb_alpha_mode alpha_mode;
	float alpha_cutoff;
	bool double_sided;
	struct glb_image base_color_image;
	struct glb_image emissive_image;
};

/* false on any unsupported or malformed input, with error set; the asset is
 * then empty (nothing to free, but glb_asset_free is harmless). */
bool glb_read(const uint8_t *file, size_t file_bytes, struct glb_asset *asset,
	char error[GLB_ERROR_LENGTH]);
void glb_asset_free(struct glb_asset *asset);

#endif
