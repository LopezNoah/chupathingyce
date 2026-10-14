/* Optional local GLB visual. Included by d3d8_gl.c to reuse its target and
 * camera state; never writes tags or contributes collision. One cached asset
 * per process; all GPU work happens on the renderer thread. */
#if defined(HALO_64BIT) && defined(HALO_FEATURE_PLATFORM) && !defined(HALO_ANDROID)
#include "glb_reader.h"
extern unsigned char *platform_image_decode(const unsigned char *, unsigned int, int *, int *);
extern void platform_image_free(unsigned char *);

static struct {
    BOOL attempted, ready;
    struct glb_asset asset;
    GLuint program, buffers[3], textures[2];
} platform_mesh;

static GLuint platform_mesh_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint ok;
    char error[512];
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(shader, sizeof(error), NULL, error);
        platform_log("platform: shader failed: %s", error);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static BOOL platform_mesh_program(void)
{
    const char *vertex = "#version 410 core\n"
        "layout(location=0) in vec3 p; layout(location=1) in vec2 uv; out vec2 t;\n"
        "uniform vec4 camera[4]; uniform vec4 model[2]; uniform vec4 pixel; uniform vec4 pose[5];\n"
        "void main(){vec4 w=vec4(p*model[0].xyz+model[1].xyz,1);"
        "vec3 r=w.xyz-pose[0].xyz;"
        "w.xyz=vec3(dot(pose[2].xyz,r),dot(pose[3].xyz,r),dot(pose[4].xyz,r))+pose[1].xyz;"
        "vec4 c=vec4(dot(w,camera[0]),dot(w,camera[1]),dot(w,camera[2]),dot(w,camera[3]));"
        "c.xy+=pixel.xy*c.w;"
#ifdef HALO_GL_NO_CLIP_CONTROL
        "c.y=-c.y; c.z=2*c.z-c.w;"
#endif
        "gl_Position=c;t=uv;}\n";
    const char *fragment = "#version 410 core\n"
        "in vec2 t; out vec4 color; uniform sampler2D base_tex,emit_tex;"
        "uniform vec4 base_factor,emit_factor,alpha;\n"
        "void main(){vec4 b=texture(base_tex,t)*base_factor;"
        "if(alpha.x==1 && b.a<alpha.y) discard;"
        "color=vec4(b.rgb+texture(emit_tex,t).rgb*emit_factor.rgb,alpha.x==2?b.a:1);}\n";
    GLuint vs = platform_mesh_shader(GL_VERTEX_SHADER, vertex);
    GLuint fs = platform_mesh_shader(GL_FRAGMENT_SHADER, fragment);
    GLint ok = 0;
    if (vs && fs) {
        platform_mesh.program = glCreateProgram();
        glAttachShader(platform_mesh.program, vs);
        glAttachShader(platform_mesh.program, fs);
        glLinkProgram(platform_mesh.program);
        glGetProgramiv(platform_mesh.program, GL_LINK_STATUS, &ok);
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return ok != 0;
}

static BOOL platform_mesh_texture(GLuint texture, const struct glb_image *image)
{
    unsigned char white[4] = {255,255,255,255};
    unsigned char *pixels = white;
    int width = 1, height = 1;
    if (image->present) {
        pixels = platform_image_decode(image->bytes, image->byte_count, &width, &height);
        if (!pixels) return FALSE;
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glGenerateMipmap(GL_TEXTURE_2D);
    if (image->present) platform_image_free(pixels);
    return TRUE;
}

static BOOL platform_mesh_load(const char *name)
{
    char path[4096], error[GLB_ERROR_LENGTH];
    FILE *file;
    long size;
    uint8_t *bytes;
    struct glb_asset *asset = &platform_mesh.asset;
    BOOL ok;
    int length = name[0] == '/' ? snprintf(path, sizeof(path), "%s", name) :
        snprintf(path, sizeof(path), "%s/%s", platform_data_root(), name);
    if (length < 0 || length >= (int)sizeof(path)) return FALSE;
    file = fopen(path, "rb");
    if (!file) { platform_log("platform: cannot open %s", path); return FALSE; }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 || size > GLB_FILE_BYTES_MAX) {
        fclose(file); return FALSE;
    }
    rewind(file);
    bytes = malloc((size_t)size);
    if (!bytes) { fclose(file); return FALSE; }
    ok = fread(bytes, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    if (ok) ok = glb_read(bytes, (size_t)size, asset, error);
    else snprintf(error, sizeof(error), "short read");
    if (!ok) { platform_log("platform: GLB rejected: %s", error); free(bytes); return FALSE; }
    /* Bake glTF Y-up into Halo Z-up: (x,y,z) -> (x,-z,y). */
    for (uint32_t i = 0; i < asset->vertex_count; i++) {
        float y = asset->positions[i*3+1];
        asset->positions[i*3+1] = -asset->positions[i*3+2];
        asset->positions[i*3+2] = y;
    }
    glGenBuffers(3, platform_mesh.buffers);
    glBindBuffer(GL_ARRAY_BUFFER, platform_mesh.buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, asset->vertex_count*3*sizeof(float), asset->positions, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, platform_mesh.buffers[1]);
    glBufferData(GL_ARRAY_BUFFER, asset->vertex_count*2*sizeof(float), asset->texcoords, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, platform_mesh.buffers[2]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, asset->index_count*sizeof(uint32_t), asset->indices, GL_STATIC_DRAW);
    glGenTextures(2, platform_mesh.textures);
    glActiveTexture(GL_TEXTURE0);
    ok = platform_mesh_texture(platform_mesh.textures[0], &asset->base_color_image) &&
        platform_mesh_texture(platform_mesh.textures[1], &asset->emissive_image) && platform_mesh_program();
    free(bytes);
    asset->base_color_image.bytes = asset->emissive_image.bytes = NULL;
    free(asset->positions); asset->positions = NULL;
    free(asset->texcoords); asset->texcoords = NULL;
    free(asset->indices); asset->indices = NULL;
    if (!ok) {
        glDeleteBuffers(3, platform_mesh.buffers);
        glDeleteTextures(2, platform_mesh.textures);
        if (platform_mesh.program) glDeleteProgram(platform_mesh.program);
        platform_log("platform: GLB upload failed; keeping procedural board");
    } else platform_log("platform: loaded %s (%u vertices, %u indices)", path,
        asset->vertex_count, asset->index_count);
    return ok;
}

/* Bounds in Halo world axes. The mesh is fitted to the board's top rectangle.
 * Use a temporary VAO (created once) so cached game attribute pointers survive. */
int platform_mesh_draw(const char *path, const float minimum[3], const float maximum[3],
    const float position[3], const float rotation[3][3])
{
    static GLuint vao;
    GLint previous_vao;
    BOOL has_depth;
    int drawn = 0;
    struct glb_asset *asset = &platform_mesh.asset;
    float model[2][4] = {{0}}, pixel[4] = {0}, emission[4], alpha[4], pose[5][4] = {{0}};
    for (int axis = 0; axis < 3; axis++) {
        pose[0][axis] = (minimum[axis] + maximum[axis])*0.5f;
        pose[1][axis] = position[axis];
    }
    for (int row = 0; row < 3; row++)
        for (int column = 0; column < 3; column++) pose[row + 2][column] = rotation[row][column];
    if (!path || !path[0] || !device.gl_ready) return 0;
    if (!bind_targets(&has_depth) || !has_depth) return 0;
    apply_raster_state(has_depth);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_vao);
    if (!vao) glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    if (!platform_mesh.attempted) {
        platform_mesh.attempted = TRUE;
        platform_mesh.ready = platform_mesh_load(path);
    }
    if (!platform_mesh.ready) goto restore;
    if (asset->maximum[0] <= asset->minimum[0] || asset->maximum[2] <= asset->minimum[2]) goto restore;
    model[0][0] = (maximum[0]-minimum[0])/(asset->maximum[0]-asset->minimum[0]);
    model[0][1] = (maximum[1]-minimum[1])/(asset->maximum[2]-asset->minimum[2]);
    model[0][2] = (model[0][0]+model[0][1])*0.5f;
    model[1][0] = minimum[0]-asset->minimum[0]*model[0][0];
    model[1][1] = minimum[1]+asset->maximum[2]*model[0][1];
    model[1][2] = maximum[2]+0.002f-asset->minimum[1]*model[0][2];
    if (device.viewport_scale[0]) pixel[0] = 0.5f/device.viewport_scale[0];
    if (device.viewport_scale[1]) pixel[1] = 0.5f/device.viewport_scale[1];
    memcpy(emission, asset->emissive_factor, 3*sizeof(float)); emission[3] = 0;
    alpha[0] = (float)asset->alpha_mode; alpha[1] = asset->alpha_cutoff; alpha[2] = alpha[3] = 0;
    glUseProgram(platform_mesh.program);
#define MESH_UNIFORM(name, count, values) glUniform4fv(glGetUniformLocation(platform_mesh.program, name), count, values)
    MESH_UNIFORM("camera", 4, device.constants[0]);
    MESH_UNIFORM("pose", 5, pose[0]);
    MESH_UNIFORM("model", 2, model[0]); MESH_UNIFORM("pixel", 1, pixel);
    MESH_UNIFORM("base_factor", 1, asset->base_color_factor);
    MESH_UNIFORM("emit_factor", 1, emission); MESH_UNIFORM("alpha", 1, alpha);
#undef MESH_UNIFORM
    for (int unit = 0; unit < 2; unit++) {
        glActiveTexture(GL_TEXTURE0+unit); glBindSampler(unit, 0);
        glBindTexture(GL_TEXTURE_2D, platform_mesh.textures[unit]);
        glUniform1i(glGetUniformLocation(platform_mesh.program, unit ? "emit_tex" : "base_tex"), unit);
    }
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
    glDepthMask(asset->alpha_mode == GLB_ALPHA_BLEND ? GL_FALSE : GL_TRUE);
    glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL); glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    if (asset->alpha_mode == GLB_ALPHA_BLEND) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (int attribute = 0; attribute < 2; attribute++) {
        glBindBuffer(GL_ARRAY_BUFFER, platform_mesh.buffers[attribute]);
        glEnableVertexAttribArray(attribute);
        glVertexAttribPointer(attribute, attribute ? 2 : 3, GL_FLOAT, GL_FALSE, 0, NULL);
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, platform_mesh.buffers[2]);
    glDrawElements(GL_TRIANGLES, asset->index_count, GL_UNSIGNED_INT, NULL);
    drawn = 1;
restore:
    glBindVertexArray((GLuint)previous_vao);
    xgpu_gl_state_invalidate();
    return drawn;
}
#endif
