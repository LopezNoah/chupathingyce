/*
GLB_READER.C

See glb_reader.h for the supported subset. The JSON chunk is tokenized once
into a fixed array (no recursion: an explicit stack of bounded depth), then
read by key. Every accessor and image range is checked against the BIN chunk
with 64-bit arithmetic before any byte is copied. Malformed or unsupported
files are rejected, never partially used.
*/

#if (defined(HALO_64BIT) && defined(HALO_FEATURE_PLATFORM)) || defined(GLB_READER_TEST)
#include "glb_reader.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
	GLB_MAGIC = 0x46546C67,        /* "glTF" */
	GLB_CHUNK_JSON = 0x4E4F534A,
	GLB_CHUNK_BIN = 0x004E4942,
	GLB_HEADER_BYTES = 12,
	GLB_CHUNK_HEADER_BYTES = 8,
	NUMBER_TEXT_MAX = 63,
};

enum json_type { JSON_OBJECT = 1, JSON_ARRAY, JSON_STRING, JSON_PRIMITIVE };

struct json_token
{
	uint8_t type;
	uint32_t start, end;    /* byte range; strings exclude their quotes */
	uint32_t child_count;   /* direct children (an object's are key, value, ...) */
	uint32_t next;          /* the token after this one's whole subtree */
};

struct json
{
	const char *text;
	uint32_t length;
	uint32_t count;
	struct json_token tokens[GLB_JSON_TOKENS_MAX];
};

struct reader
{
	struct json json;
	const uint8_t *bin;
	uint32_t bin_bytes;
	char *error;
};

struct accessor_view
{
	const uint8_t *base;
	uint32_t count, stride, component_type, components;
};

/* ---------- errors */

static bool fail(struct reader *reader, const char *format, ...)
{
	va_list arguments;

	if (reader->error[0])
		return false;
	va_start(arguments, format);
	vsnprintf(reader->error, GLB_ERROR_LENGTH, format, arguments);
	va_end(arguments);
	return false;
}

/* ---------- JSON tokenizer */

static bool json_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int64_t json_new(struct json *json, uint32_t stack[], uint32_t depth, uint8_t type, uint32_t start)
{
	struct json_token *token;

	if (json->count >= GLB_JSON_TOKENS_MAX)
		return -1;
	if (depth == 0 && json->count != 0)
		return -1; /* a second root */
	if (depth > 0)
		json->tokens[stack[depth - 1]].child_count++;
	token = &json->tokens[json->count];
	*token = (struct json_token){ 0 };
	token->type = type;
	token->start = start;
	return json->count++;
}

static bool json_container_valid(const struct json *json, uint32_t index)
{
	const struct json_token *token = &json->tokens[index];
	uint32_t child = index + 1;
	uint32_t member;

	if (token->type != JSON_OBJECT)
		return true;
	if (token->child_count % 2 != 0)
		return false;
	for (member = 0; member < token->child_count / 2; member++)
	{
		if (json->tokens[child].type != JSON_STRING)
			return false;
		child = json->tokens[child + 1].next;
	}
	return true;
}

/* Structural tokenizer. Separators (',' ':') are not position-checked; the
 * key/value pairing of objects is checked when each closes. */
static bool json_parse(struct json *json, const char *text, uint32_t length)
{
	uint32_t stack[GLB_JSON_DEPTH_MAX];
	uint32_t depth = 0;
	uint32_t at = 0;

	json->text = text;
	json->length = length;
	json->count = 0;
	while (at < length)
	{
		char c = text[at];
		int64_t index;

		if (json_space(c) || c == ',' || c == ':' || c == '\0')
		{
			at++;
			continue;
		}
		if (c == '{' || c == '[')
		{
			if (depth >= GLB_JSON_DEPTH_MAX)
				return false;
			index = json_new(json, stack, depth, c == '{' ? JSON_OBJECT : JSON_ARRAY, at);
			if (index < 0)
				return false;
			stack[depth++] = (uint32_t)index;
			at++;
		}
		else if (c == '}' || c == ']')
		{
			struct json_token *token;

			if (depth == 0)
				return false;
			token = &json->tokens[stack[--depth]];
			if (token->type != (c == '}' ? JSON_OBJECT : JSON_ARRAY))
				return false;
			token->end = at + 1;
			token->next = json->count;
			if (!json_container_valid(json, stack[depth]))
				return false;
			at++;
		}
		else if (c == '"')
		{
			uint32_t close = at + 1;

			while (close < length && text[close] != '"')
			{
				if ((unsigned char)text[close] < 0x20)
					return false;
				close += text[close] == '\\' ? 2 : 1;
			}
			if (close >= length)
				return false;
			index = json_new(json, stack, depth, JSON_STRING, at + 1);
			if (index < 0)
				return false;
			json->tokens[index].end = close;
			json->tokens[index].next = json->count;
			at = close + 1;
		}
		else
		{
			uint32_t end = at;

			if (!strchr("-0123456789tfn", c))
				return false;
			while (end < length && !json_space(text[end]) && !strchr(",:]}", text[end]))
				end++;
			index = json_new(json, stack, depth, JSON_PRIMITIVE, at);
			if (index < 0)
				return false;
			json->tokens[index].end = end;
			json->tokens[index].next = json->count;
			at = end;
		}
	}
	return depth == 0 && json->count > 0 && json->tokens[0].type == JSON_OBJECT;
}

/* ---------- JSON queries (indices; -1 is absent) */

static int64_t json_get(const struct json *json, int64_t object, const char *key)
{
	uint32_t child, member;
	size_t key_length = strlen(key);

	if (object < 0 || json->tokens[object].type != JSON_OBJECT)
		return -1;
	child = (uint32_t)object + 1;
	for (member = 0; member < json->tokens[object].child_count / 2; member++)
	{
		const struct json_token *name = &json->tokens[child];

		if (name->end - name->start == key_length &&
			!memcmp(json->text + name->start, key, key_length))
		{
			return child + 1;
		}
		child = json->tokens[child + 1].next;
	}
	return -1;
}

static int64_t json_at(const struct json *json, int64_t array, uint32_t position)
{
	uint32_t child, index;

	if (array < 0 || json->tokens[array].type != JSON_ARRAY ||
		position >= json->tokens[array].child_count)
	{
		return -1;
	}
	child = (uint32_t)array + 1;
	for (index = 0; index < position; index++)
		child = json->tokens[child].next;
	return child;
}

static bool json_number(const struct json *json, int64_t token, double *value)
{
	char text[NUMBER_TEXT_MAX + 1];
	const struct json_token *number;
	uint32_t length;
	char *end;

	if (token < 0 || json->tokens[token].type != JSON_PRIMITIVE)
		return false;
	number = &json->tokens[token];
	length = number->end - number->start;
	if (length == 0 || length > NUMBER_TEXT_MAX)
		return false;
	memcpy(text, json->text + number->start, length);
	text[length] = '\0';
	*value = strtod(text, &end);
	return *end == '\0' && isfinite(*value);
}

/* absent: the default; present: an integer in [0, maximum] */
static bool json_uint(const struct json *json, int64_t token, uint32_t fallback, uint32_t maximum,
	uint32_t *value)
{
	double number;

	if (token < 0)
	{
		*value = fallback;
		return true;
	}
	if (!json_number(json, token, &number) || number < 0.0 || number > (double)maximum ||
		floor(number) != number)
	{
		return false;
	}
	*value = (uint32_t)number;
	return true;
}

static bool json_string_is(const struct json *json, int64_t token, const char *value)
{
	size_t length = strlen(value);

	return token >= 0 && json->tokens[token].type == JSON_STRING &&
		json->tokens[token].end - json->tokens[token].start == length &&
		!memcmp(json->text + json->tokens[token].start, value, length);
}

static bool json_true(const struct json *json, int64_t token)
{
	return token >= 0 && json->tokens[token].type == JSON_PRIMITIVE &&
		json->tokens[token].end - json->tokens[token].start == 4 &&
		!memcmp(json->text + json->tokens[token].start, "true", 4);
}

/* absent: the defaults stay; present: exactly count finite numbers */
static bool json_floats(const struct json *json, int64_t array, uint32_t count, float *values)
{
	uint32_t index;

	if (array < 0)
		return true;
	if (json->tokens[array].type != JSON_ARRAY || json->tokens[array].child_count != count)
		return false;
	for (index = 0; index < count; index++)
	{
		double value;

		if (!json_number(json, json_at(json, array, index), &value))
			return false;
		values[index] = (float)value;
		if (!isfinite(values[index]))
			return false;
	}
	return true;
}

/* ---------- binary layout */

static uint32_t read_u32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/* a bufferView's bytes in the BIN chunk; stride 0 when it has none */
static bool buffer_view(struct reader *reader, uint32_t index, const uint8_t **bytes, uint32_t *length,
	uint32_t *stride)
{
	const struct json *json = &reader->json;
	int64_t view = json_at(json, json_get(json, 0, "bufferViews"), index);
	uint32_t buffer, offset;

	if (view < 0)
		return fail(reader, "bufferView %u missing", index);
	if (!json_uint(json, json_get(json, view, "buffer"), UINT32_MAX, 0, &buffer) ||
		!json_uint(json, json_get(json, view, "byteOffset"), 0, UINT32_MAX, &offset) ||
		!json_uint(json, json_get(json, view, "byteLength"), UINT32_MAX, UINT32_MAX, length) ||
		*length == UINT32_MAX ||
		!json_uint(json, json_get(json, view, "byteStride"), 0, 252, stride))
	{
		return fail(reader, "bufferView %u: unsupported buffer/offset/length/stride", index);
	}
	if (offset > reader->bin_bytes || *length > reader->bin_bytes - offset)
		return fail(reader, "bufferView %u is outside the BIN chunk", index);
	if (*stride != 0 && (*stride < 4 || *stride % 4 != 0))
		return fail(reader, "bufferView %u: invalid byteStride", index);
	*bytes = reader->bin + offset;
	return true;
}

static uint32_t component_bytes(uint32_t component_type)
{
	switch (component_type)
	{
	case 5121: return 1; /* UNSIGNED_BYTE */
	case 5123: return 2; /* UNSIGNED_SHORT */
	case 5125: return 4; /* UNSIGNED_INT */
	case 5126: return 4; /* FLOAT */
	default: return 0;
	}
}

static uint32_t type_components(const struct json *json, int64_t type)
{
	if (json_string_is(json, type, "SCALAR")) return 1;
	if (json_string_is(json, type, "VEC2")) return 2;
	if (json_string_is(json, type, "VEC3")) return 3;
	return 0;
}

static bool accessor(struct reader *reader, uint32_t index, uint32_t components, uint32_t count_max,
	struct accessor_view *view)
{
	const struct json *json = &reader->json;
	int64_t token = json_at(json, json_get(json, 0, "accessors"), index);
	uint32_t view_index, offset, element_bytes, view_length, view_stride;
	const uint8_t *view_bytes;
	uint64_t needed;

	if (token < 0)
		return fail(reader, "accessor %u missing", index);
	if (json_get(json, token, "sparse") >= 0)
		return fail(reader, "accessor %u: sparse accessors are not supported", index);
	if (!json_uint(json, json_get(json, token, "bufferView"), UINT32_MAX, UINT32_MAX - 1, &view_index) ||
		view_index == UINT32_MAX ||
		!json_uint(json, json_get(json, token, "byteOffset"), 0, UINT32_MAX, &offset) ||
		!json_uint(json, json_get(json, token, "componentType"), 0, 5126, &view->component_type) ||
		!json_uint(json, json_get(json, token, "count"), 0, count_max, &view->count) || view->count == 0 ||
		json_true(json, json_get(json, token, "normalized")))
	{
		return fail(reader, "accessor %u: unsupported bufferView/offset/type/count", index);
	}
	view->components = type_components(json, json_get(json, token, "type"));
	if (view->components != components || component_bytes(view->component_type) == 0)
		return fail(reader, "accessor %u: unexpected type or component type", index);
	if (!buffer_view(reader, view_index, &view_bytes, &view_length, &view_stride))
		return false;
	element_bytes = component_bytes(view->component_type) * components;
	view->stride = view_stride ? view_stride : element_bytes;
	if (view->stride < element_bytes)
		return fail(reader, "accessor %u: stride smaller than an element", index);
	needed = (uint64_t)(view->count - 1) * view->stride + element_bytes;
	if (offset > view_length || needed > (uint64_t)(view_length - offset))
		return fail(reader, "accessor %u is outside its bufferView", index);
	view->base = view_bytes + offset;
	return true;
}

static bool read_floats(struct reader *reader, const struct accessor_view *view, float *out,
	const char *what)
{
	uint32_t element, component;

	if (view->component_type != 5126)
		return fail(reader, "%s must be FLOAT", what);
	for (element = 0; element < view->count; element++)
	{
		for (component = 0; component < view->components; component++)
		{
			float value;

			memcpy(&value, view->base + (size_t)element * view->stride + component * 4u, sizeof(value));
			if (!isfinite(value))
				return fail(reader, "%s has a non-finite value", what);
			out[(size_t)element * view->components + component] = value;
		}
	}
	return true;
}

static bool read_indices(struct reader *reader, const struct accessor_view *view, uint32_t vertex_count,
	uint32_t *out)
{
	uint32_t index;

	if (view->component_type == 5126)
		return fail(reader, "indices must be unsigned integers");
	for (index = 0; index < view->count; index++)
	{
		const uint8_t *at = view->base + (size_t)index * view->stride;
		uint32_t value;

		if (view->component_type == 5121)
			value = at[0];
		else if (view->component_type == 5123)
			value = (uint32_t)at[0] | (uint32_t)at[1] << 8;
		else
			value = read_u32(at);
		if (value >= vertex_count)
			return fail(reader, "index %u refers past the vertices", index);
		out[index] = value;
	}
	return true;
}

/* ---------- materials */

static bool texture_image(struct reader *reader, int64_t info, struct glb_image *image)
{
	const struct json *json = &reader->json;
	uint32_t texture_index, coordinate, image_index, length, stride;
	int64_t texture, source;
	const uint8_t *bytes;

	if (info < 0)
		return true;
	if (!json_uint(json, json_get(json, info, "index"), UINT32_MAX, UINT32_MAX - 1, &texture_index) ||
		texture_index == UINT32_MAX ||
		!json_uint(json, json_get(json, info, "texCoord"), 0, 0, &coordinate))
	{
		return fail(reader, "texture reference must have an index and use TEXCOORD_0");
	}
	texture = json_at(json, json_get(json, 0, "textures"), texture_index);
	if (!json_uint(json, json_get(json, texture, "source"), UINT32_MAX, UINT32_MAX - 1, &image_index) ||
		image_index == UINT32_MAX)
	{
		return fail(reader, "texture %u has no supported image source", texture_index);
	}
	source = json_at(json, json_get(json, 0, "images"), image_index);
	if (source < 0 || json_get(json, source, "uri") >= 0)
		return fail(reader, "image %u must be embedded in the GLB", image_index);
	if (!json_string_is(json, json_get(json, source, "mimeType"), "image/png") &&
		!json_string_is(json, json_get(json, source, "mimeType"), "image/jpeg"))
	{
		return fail(reader, "image %u must be PNG or JPEG", image_index);
	}
	if (!json_uint(json, json_get(json, source, "bufferView"), UINT32_MAX, UINT32_MAX - 1, &texture_index) ||
		texture_index == UINT32_MAX || !buffer_view(reader, texture_index, &bytes, &length, &stride))
	{
		return fail(reader, "image %u: invalid bufferView", image_index);
	}
	if (stride != 0 || length == 0)
		return fail(reader, "image %u: invalid image bytes", image_index);
	image->present = true;
	image->bytes = bytes;
	image->byte_count = length;
	return true;
}

static bool material(struct reader *reader, int64_t primitive, struct glb_asset *asset)
{
	const struct json *json = &reader->json;
	int64_t token, pbr, mode;
	uint32_t index;
	double cutoff;

	asset->base_color_factor[0] = asset->base_color_factor[1] = 1.0f;
	asset->base_color_factor[2] = asset->base_color_factor[3] = 1.0f;
	asset->alpha_cutoff = 0.5f;
	token = json_get(json, primitive, "material");
	if (token < 0)
		return true;
	if (!json_uint(json, token, 0, UINT32_MAX - 1, &index))
		return fail(reader, "invalid material index");
	token = json_at(json, json_get(json, 0, "materials"), index);
	if (token < 0)
		return fail(reader, "material %u missing", index);
	pbr = json_get(json, token, "pbrMetallicRoughness");
	mode = json_get(json, token, "alphaMode");
	if (!json_floats(json, json_get(json, pbr, "baseColorFactor"), 4, asset->base_color_factor) ||
		!json_floats(json, json_get(json, token, "emissiveFactor"), 3, asset->emissive_factor))
	{
		return fail(reader, "material %u: invalid color factors", index);
	}
	if (mode < 0 || json_string_is(json, mode, "OPAQUE"))
		asset->alpha_mode = GLB_ALPHA_OPAQUE;
	else if (json_string_is(json, mode, "MASK"))
		asset->alpha_mode = GLB_ALPHA_MASK;
	else if (json_string_is(json, mode, "BLEND"))
		asset->alpha_mode = GLB_ALPHA_BLEND;
	else
		return fail(reader, "material %u: unknown alphaMode", index);
	if (json_get(json, token, "alphaCutoff") >= 0)
	{
		if (!json_number(json, json_get(json, token, "alphaCutoff"), &cutoff) || cutoff < 0.0 || cutoff > 1.0)
			return fail(reader, "material %u: invalid alphaCutoff", index);
		asset->alpha_cutoff = (float)cutoff;
	}
	asset->double_sided = json_true(json, json_get(json, token, "doubleSided"));
	return texture_image(reader, json_get(json, pbr, "baseColorTexture"), &asset->base_color_image) &&
		texture_image(reader, json_get(json, token, "emissiveTexture"), &asset->emissive_image);
}

/* ---------- geometry */

static bool geometry(struct reader *reader, int64_t primitive, struct glb_asset *asset)
{
	const struct json *json = &reader->json;
	int64_t attributes = json_get(json, primitive, "attributes");
	uint32_t mode, position_index, texcoord_index, index_accessor, vertex;
	struct accessor_view positions, texcoords, indices;

	if (!json_uint(json, json_get(json, primitive, "mode"), 4, 6, &mode) || mode != 4)
		return fail(reader, "only triangle-list primitives are supported");
	if (!json_uint(json, json_get(json, attributes, "POSITION"), UINT32_MAX, UINT32_MAX - 1, &position_index) ||
		position_index == UINT32_MAX ||
		!json_uint(json, json_get(json, attributes, "TEXCOORD_0"), UINT32_MAX, UINT32_MAX - 1, &texcoord_index) ||
		texcoord_index == UINT32_MAX)
	{
		return fail(reader, "primitive needs POSITION and TEXCOORD_0");
	}
	if (!accessor(reader, position_index, 3, GLB_VERTICES_MAX, &positions) ||
		!accessor(reader, texcoord_index, 2, GLB_VERTICES_MAX, &texcoords))
	{
		return false;
	}
	if (texcoords.count != positions.count)
		return fail(reader, "POSITION and TEXCOORD_0 counts differ");
	asset->vertex_count = positions.count;
	asset->positions = malloc((size_t)positions.count * 3 * sizeof(float));
	asset->texcoords = malloc((size_t)positions.count * 2 * sizeof(float));
	if (!asset->positions || !asset->texcoords)
		return fail(reader, "out of memory");
	if (!read_floats(reader, &positions, asset->positions, "POSITION") ||
		!read_floats(reader, &texcoords, asset->texcoords, "TEXCOORD_0"))
	{
		return false;
	}

	if (json_get(json, primitive, "indices") >= 0)
	{
		if (!json_uint(json, json_get(json, primitive, "indices"), 0, UINT32_MAX - 1, &index_accessor) ||
			!accessor(reader, index_accessor, 1, GLB_INDICES_MAX, &indices))
		{
			return fail(reader, "invalid indices accessor");
		}
		asset->index_count = indices.count;
	}
	else
	{
		asset->index_count = asset->vertex_count;
	}
	if (asset->index_count % 3 != 0)
		return fail(reader, "index count is not a whole number of triangles");
	asset->indices = malloc((size_t)asset->index_count * sizeof(uint32_t));
	if (!asset->indices)
		return fail(reader, "out of memory");
	if (json_get(json, primitive, "indices") >= 0)
	{
		if (!read_indices(reader, &indices, asset->vertex_count, asset->indices))
			return false;
	}
	else
	{
		for (vertex = 0; vertex < asset->index_count; vertex++)
			asset->indices[vertex] = vertex;
	}

	for (vertex = 0; vertex < asset->vertex_count; vertex++)
	{
		uint32_t axis;

		for (axis = 0; axis < 3; axis++)
		{
			float value = asset->positions[(size_t)vertex * 3 + axis];

			if (vertex == 0 || value < asset->minimum[axis]) asset->minimum[axis] = value;
			if (vertex == 0 || value > asset->maximum[axis]) asset->maximum[axis] = value;
		}
	}
	return true;
}

/* ---------- the file */

static bool chunks(struct reader *reader, const uint8_t *file, size_t file_bytes)
{
	uint32_t json_bytes, bin_offset;

	if (file_bytes < GLB_HEADER_BYTES + 2 * GLB_CHUNK_HEADER_BYTES || file_bytes > GLB_FILE_BYTES_MAX)
		return fail(reader, "not a GLB file, or larger than %u bytes", (unsigned)GLB_FILE_BYTES_MAX);
	if (read_u32(file) != GLB_MAGIC || read_u32(file + 4) != 2 || read_u32(file + 8) != file_bytes)
		return fail(reader, "not a glTF 2.0 binary, or its length is wrong");
	json_bytes = read_u32(file + 12);
	if (read_u32(file + 16) != GLB_CHUNK_JSON || json_bytes == 0 ||
		json_bytes > file_bytes - GLB_HEADER_BYTES - 2 * GLB_CHUNK_HEADER_BYTES)
	{
		return fail(reader, "the first chunk must be JSON and fit in the file");
	}
	bin_offset = GLB_HEADER_BYTES + GLB_CHUNK_HEADER_BYTES + json_bytes;
	reader->bin_bytes = read_u32(file + bin_offset);
	if (read_u32(file + bin_offset + 4) != GLB_CHUNK_BIN ||
		reader->bin_bytes > file_bytes - bin_offset - GLB_CHUNK_HEADER_BYTES)
	{
		return fail(reader, "the second chunk must be BIN and fit in the file");
	}
	reader->bin = file + bin_offset + GLB_CHUNK_HEADER_BYTES;
	if (!json_parse(&reader->json, (const char *)file + GLB_HEADER_BYTES + GLB_CHUNK_HEADER_BYTES, json_bytes))
		return fail(reader, "malformed JSON, or more than %d tokens / %d levels",
			GLB_JSON_TOKENS_MAX, GLB_JSON_DEPTH_MAX);
	return true;
}

static bool document(struct reader *reader, struct glb_asset *asset)
{
	const struct json *json = &reader->json;
	int64_t buffers = json_get(json, 0, "buffers");
	int64_t buffer = json_at(json, buffers, 0);
	int64_t primitive;
	uint32_t buffer_bytes;

	if (!json_string_is(json, json_get(json, json_get(json, 0, "asset"), "version"), "2.0"))
		return fail(reader, "asset.version must be 2.0");
	if (json_get(json, 0, "extensionsRequired") >= 0)
		return fail(reader, "required glTF extensions are not supported");
	if (buffers < 0 || json->tokens[buffers].child_count != 1 || json_get(json, buffer, "uri") >= 0 ||
		!json_uint(json, json_get(json, buffer, "byteLength"), UINT32_MAX, UINT32_MAX - 1, &buffer_bytes) ||
		buffer_bytes > reader->bin_bytes)
	{
		return fail(reader, "exactly one embedded buffer, within the BIN chunk, is required");
	}
	reader->bin_bytes = buffer_bytes;
	primitive = json_at(json, json_get(json, json_at(json, json_get(json, 0, "meshes"), 0), "primitives"), 0);
	if (primitive < 0)
		return fail(reader, "meshes[0].primitives[0] is missing");
	return geometry(reader, primitive, asset) && material(reader, primitive, asset);
}

bool glb_read(const uint8_t *file, size_t file_bytes, struct glb_asset *asset,
	char error[GLB_ERROR_LENGTH])
{
	/* The token array is large (~160 KiB); heap, once per load. */
	struct reader *reader = calloc(1, sizeof(*reader));
	bool ok;

	memset(asset, 0, sizeof(*asset));
	error[0] = '\0';
	if (!reader)
	{
		snprintf(error, GLB_ERROR_LENGTH, "out of memory");
		return false;
	}
	reader->error = error;
	ok = file && chunks(reader, file, file_bytes) && document(reader, asset);
	if (!file)
		snprintf(error, GLB_ERROR_LENGTH, "no file");
	free(reader);
	if (!ok)
		glb_asset_free(asset);
	return ok;
}

void glb_asset_free(struct glb_asset *asset)
{
	free(asset->positions);
	free(asset->texcoords);
	free(asset->indices);
	memset(asset, 0, sizeof(*asset));
}
#endif
