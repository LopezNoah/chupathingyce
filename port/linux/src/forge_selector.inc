/* Native PNG Forge reticles. Two bounded, process-lifetime resources; no tags
 * or GLB mesh are required. Draw in the current player's viewport, RGB only. */
#if defined(HALO_64BIT) && defined(HALO_FEATURE_FORGE) && !defined(HALO_ANDROID)
extern unsigned char *platform_image_decode(const unsigned char *, unsigned int, int *, int *);
extern void platform_image_free(unsigned char *);

static struct {
    GLuint program, vao;
    BOOL attempted;
    struct {
        GLuint texture;
        int width, height;
        BOOL attempted, ready;
    } images[2];
} forge_selector;

static BOOL forge_selector_program(void)
{
    const char *vertex = "#version 410 core\n"
        "out vec2 uv; uniform vec4 extent;"
        "void main(){uv=vec2(gl_VertexID&1,gl_VertexID>>1);"
        "vec2 p=uv*2-1;gl_Position=vec4(p.x*extent.x,-p.y*extent.y,0,1);"
#ifdef HALO_GL_NO_CLIP_CONTROL
        "gl_Position.y=-gl_Position.y;"
#endif
        "}\n";
    const char *fragment = "#version 410 core\n"
        "in vec2 uv; out vec4 color; uniform sampler2D image;"
        "void main(){color=texture(image,uv);}\n";
    GLuint shaders[2] = {glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER)};
    const char *sources[2] = {vertex, fragment};
    GLint ok = GL_TRUE;
    forge_selector.program = glCreateProgram();
    for (int index = 0; index < 2 && ok; index++) {
        glShaderSource(shaders[index], 1, &sources[index], NULL);
        glCompileShader(shaders[index]);
        glGetShaderiv(shaders[index], GL_COMPILE_STATUS, &ok);
        if (ok) glAttachShader(forge_selector.program, shaders[index]);
    }
    if (ok) {
        glLinkProgram(forge_selector.program);
        glGetProgramiv(forge_selector.program, GL_LINK_STATUS, &ok);
    }
    for (int index = 0; index < 2; index++) glDeleteShader(shaders[index]);
    if (!ok) {
        glDeleteProgram(forge_selector.program);
        forge_selector.program = 0;
        platform_log("forge selector: shader failed; using text reticle");
    } else glGenVertexArrays(1, &forge_selector.vao);
    return ok != 0;
}

static unsigned char *forge_selector_read(const char *name, int *width, int *height)
{
    const unsigned char signature[8] = {137,80,78,71,13,10,26,10};
    char path[4096];
    FILE *file;
    long size;
    unsigned char *bytes, *pixels = NULL;
    int length = name[0] == '/' ? snprintf(path, sizeof(path), "%s", name) :
        snprintf(path, sizeof(path), "%s/%s", platform_data_root(), name);
    if (length < 0 || length >= (int)sizeof(path)) return NULL;
    file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 8 || size > 64L * 1024L * 1024L) {
        fclose(file); return NULL;
    }
    rewind(file);
    bytes = malloc((size_t)size);
    if (bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size &&
        !memcmp(bytes, signature, sizeof(signature)))
        pixels = platform_image_decode(bytes, (unsigned int)size, width, height);
    free(bytes);
    fclose(file);
    return pixels;
}

static BOOL forge_selector_load(int index, const char *name)
{
    int width, height;
    unsigned char *pixels = forge_selector_read(name, &width, &height);
    if (!pixels) {
        platform_log("forge selector: cannot decode PNG %s; using text reticle", name);
        return FALSE;
    }
    /* Premultiply before mip generation: transparent black must not create
     * dark fringes when these large source images shrink to HUD size. */
    for (unsigned int pixel = 0; pixel < (unsigned int)width * (unsigned int)height; pixel++)
        for (unsigned int channel = 0; channel < 3; channel++)
            pixels[pixel * 4 + channel] = (unsigned char)
                ((pixels[pixel * 4 + channel] * pixels[pixel * 4 + 3] + 127) / 255);
    glGenTextures(1, &forge_selector.images[index].texture);
    glBindTexture(GL_TEXTURE_2D, forge_selector.images[index].texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_2D);
    platform_image_free(pixels);
    forge_selector.images[index].width = width;
    forge_selector.images[index].height = height;
    platform_log("forge selector: loaded %s (%dx%d, %s)", name, width, height,
        index ? "selected bar" : "unselected cross");
    return TRUE;
}

int platform_forge_selector_draw(const char *cross, const char *bar, int selected,
    int size, int view_width, int view_height)
{
    const char *name = selected ? bar : cross;
    int index = selected ? 1 : 0, maximum;
    GLint previous_vao = 0;
    GLfloat extent[4] = {0};
    BOOL has_depth;
    int drawn = 0;
    if (!device.gl_ready || !name || !name[0] || view_width <= 0 || view_height <= 0) return 0;
    if (!bind_targets(&has_depth)) return 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_vao);
    apply_raster_state(has_depth);
    glActiveTexture(GL_TEXTURE0);
    if (!forge_selector.images[index].attempted) {
        forge_selector.images[index].attempted = TRUE;
        forge_selector.images[index].ready = forge_selector_load(index, name);
    }
    if (!forge_selector.images[index].ready) goto restore;
    if (!forge_selector.attempted) {
        forge_selector.attempted = TRUE;
        forge_selector_program();
    }
    if (!forge_selector.program) goto restore;
    size = size < 8 ? 8 : size > 64 ? 64 : size;
    maximum = forge_selector.images[index].width > forge_selector.images[index].height ?
        forge_selector.images[index].width : forge_selector.images[index].height;
    extent[0] = (float)size * forge_selector.images[index].width / maximum / view_width;
    extent[1] = (float)size * forge_selector.images[index].height / maximum / view_height;
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL); glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glUseProgram(forge_selector.program);
    glUniform4fv(glGetUniformLocation(forge_selector.program, "extent"), 1, extent);
    glUniform1i(glGetUniformLocation(forge_selector.program, "image"), 0);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, forge_selector.images[index].texture);
    glBindVertexArray(forge_selector.vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    drawn = 1;
restore:
    glBindVertexArray((GLuint)previous_vao);
    xgpu_gl_state_invalidate();
    return drawn;
}
#endif
