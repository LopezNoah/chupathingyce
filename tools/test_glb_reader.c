/* GLB reader tests: the real Forge grid asset, plus malformed/hostile inputs
 * built by mutating it. Run via tools/test_glb_reader.py (ASan/UBSan). */
#include "../port/linux/src/glb_reader.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *load(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    uint8_t *bytes;
    long length;
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    assert(length > 0);
    rewind(file);
    bytes = malloc((size_t)length);
    assert(bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    *size = (size_t)length;
    return bytes;
}

static void put_u32(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)value; at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16); at[3] = (uint8_t)(value >> 24);
}

static void expect_reject(const uint8_t *bytes, size_t size, const char *why)
{
    struct glb_asset asset;
    char error[GLB_ERROR_LENGTH];
    assert(!glb_read(bytes, size, &asset, error));
    assert(error[0] != '\0');
    assert(asset.positions == NULL && asset.indices == NULL);
    printf("  rejected (%s): %s\n", why, error);
}

/* Replace one JSON substring in place with same-length text. */
static uint8_t *patched(const uint8_t *original, size_t size, const char *from, const char *to)
{
    uint8_t *copy = malloc(size);
    uint32_t json_bytes;
    char *json;
    char *at;
    assert(copy && strlen(from) == strlen(to));
    memcpy(copy, original, size);
    json_bytes = (uint32_t)copy[12] | (uint32_t)copy[13] << 8;
    json = (char *)copy + 20;
    at = memmem(json, json_bytes, from, strlen(from));
    assert(at);
    memcpy(at, to, strlen(to));
    return copy;
}

static void test_forge_grid(const uint8_t *bytes, size_t size)
{
    struct glb_asset asset;
    char error[GLB_ERROR_LENGTH];
    bool ok = glb_read(bytes, size, &asset, error);
    if (!ok) fprintf(stderr, "forge_grid.glb: %s\n", error);
    assert(ok);
    assert(asset.vertex_count == 4 && asset.index_count == 6);
    assert(asset.minimum[0] == -16 && asset.maximum[0] == 16);
    assert(asset.minimum[1] == 0 && asset.maximum[1] == 0);   /* flat in glTF XZ */
    assert(asset.minimum[2] == -16 && asset.maximum[2] == 16);
    for (uint32_t i = 0; i < asset.index_count; i++) assert(asset.indices[i] < 4);
    for (uint32_t i = 0; i < asset.vertex_count * 2; i++)
        assert(asset.texcoords[i] >= 0.0f && asset.texcoords[i] <= 1.0f);
    assert(asset.alpha_mode == GLB_ALPHA_BLEND && asset.double_sided);
    assert(asset.emissive_factor[0] == 1 && asset.base_color_factor[3] == 1);
    assert(asset.base_color_image.present && asset.emissive_image.present);
    assert(!memcmp(asset.base_color_image.bytes, "\x89PNG", 4));
    assert(!memcmp(asset.emissive_image.bytes, "\x89PNG", 4));
    assert(asset.base_color_image.bytes >= bytes &&
        asset.base_color_image.bytes + asset.base_color_image.byte_count <= bytes + size);
    glb_asset_free(&asset);
    glb_asset_free(&asset); /* idempotent */
    printf("  forge_grid.glb: 4 vertices, 2 triangles, BLEND, double-sided, 2 PNGs\n");
}

static void test_malformed(const uint8_t *bytes, size_t size)
{
    uint8_t *copy = malloc(size);
    assert(copy);

    expect_reject(NULL, 0, "null");
    expect_reject(bytes, 10, "truncated header");
    for (size_t cut = 21; cut < 400 && cut < size; cut += 37)
        expect_reject(bytes, cut, "truncated file"); /* header length mismatch */

    memcpy(copy, bytes, size); copy[0] = 'x';
    expect_reject(copy, size, "bad magic");
    memcpy(copy, bytes, size); put_u32(copy + 4, 1);
    expect_reject(copy, size, "glTF 1");
    memcpy(copy, bytes, size); put_u32(copy + 12, 0xFFFFFFF0u);
    expect_reject(copy, size, "JSON chunk past end");
    memcpy(copy, bytes, size); copy[20] = '[';
    expect_reject(copy, size, "unbalanced JSON");
    free(copy);

    /* Same-length edits that make ranges/indices invalid. */
    struct { const char *from, *to, *why; } cases[] = {
        {"\"byteOffset\":413824", "\"byteOffset\":999999", "view past BIN"},
        {"\"count\":6,", "\"count\":9,", "accessor past view"},
        {"\"componentType\":5123", "\"componentType\":5122", "signed indices"},
        {"\"type\":\"VEC2\"", "\"type\":\"VEC4\"", "UV type"},
        {"\"alphaMode\":\"BLEND\"", "\"alphaMode\":\"OTHER\"", "alpha mode"},
        {"\"mimeType\":\"image/png\"", "\"mimeType\":\"image/gif\"", "image type"},
        {"\"version\":\"2.0\"", "\"version\":\"3.0\"", "asset version"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t *mutated = patched(bytes, size, cases[i].from, cases[i].to);
        expect_reject(mutated, size, cases[i].why);
        free(mutated);
    }
}

int main(int argc, char **argv)
{
    size_t size;
    uint8_t *bytes;
    assert(argc == 2);
    bytes = load(argv[1], &size);
    test_forge_grid(bytes, size);
    test_malformed(bytes, size);
    free(bytes);
    puts("glb reader: PASS");
    return 0;
}
