/* =============================================================================
 * xr_menu_quad_patch.c
 *
 * DROP-IN REPLACEMENTS for the cube-demo sections of main.c.
 * Replace each numbered block in your file with the matching block below.
 * Everything else (XR session init, swapchains, event loop) stays identical.
 *
 * Strategy
 * --------
 * The game renders its menu into `surface` (SDL_Surface 640×480 RGB565).
 * FlipBuffers() already uploads that into `FullscreenTexture` (an OpenGL ES
 * texture).  The SDL-GPU / OpenXR renderer lives in a *separate* GL context,
 * so it can't bind that texture directly.
 *
 * Bridge:  each XR frame we glReadPixels the game surface pixels from the
 * shared CPU `surface->pixels` buffer (no GL readback needed — the data is
 * already in system RAM in FlipBuffers!), then upload into an SDL_GPUTexture
 * that we keep alive between frames.  The quad pipeline samples that texture
 * and places it 2 m in front of the local-space origin.
 *
 * Shaders
 * -------
 * The existing load_shader() stub expects compiled SPIR-V (Vulkan) or DXIL
 * (D3D12) blobs.  We need a *textured* shader instead of the colour shader.
 * Minimal GLSL sources are provided in comments so you can compile them with
 * glslangValidator or shaderc; the load_quad_shader() helper below follows
 * the same pattern as load_shader().
 *
 * Quad layout
 * -----------
 * A single 1.6 × 0.9 m billboard (matching the 16:9 aspect of the 640×480
 * menu surface) placed at Z = -2.0 m in LOCAL space.  Both eyes see the same
 * quad geometry; each gets its own view/projection transform.
 * ============================================================================= */


/* =============================================================================
 * BLOCK 1 — Replace the vertex type and cube constants at the top of the file.
 *
 * Original lines to remove (roughly lines 249-329):
 *   typedef struct { float x, y, z; Uint8 r, g, b, a; } PositionColorVertex;
 *   static const float CUBE_HALF_SIZE = 0.25f;
 *   #define NUM_CUBES 5
 *   static Vec3 cube_positions[...] = { ... };
 *   static float cube_scales[...] = { ... };
 *   static float cube_speeds[...] = { ... };
 * ============================================================================= */

/* Textured vertex: position + UV */
typedef struct {
    float x, y, z;
    float u, v;
} PositionUVVertex;

/* Quad dimensions: 1.6 m wide × 0.9 m tall, placed 2 m ahead */
#define QUAD_HALF_W  0.80f   /* half-width  in metres */
#define QUAD_HALF_H  0.45f   /* half-height in metres */
#define QUAD_DEPTH  -2.00f   /* Z offset from local-space origin */

/* GPU texture that receives the menu pixels each frame */
static SDL_GPUTexture *menu_gpu_texture = NULL;
static SDL_GPUSampler  *menu_sampler    = NULL;

/* CPU staging: RGB565 pixels from surface, converted to RGBA8 for upload */
#define MENU_W 640
#define MENU_H 480
static Uint8 menu_rgba[MENU_W * MENU_H * 4];  /* RGBA8 conversion buffer */


/* =============================================================================
 * BLOCK 2 — Replace the pipeline and buffer GPU state declarations.
 *
 * Original lines to remove (roughly lines 309-317):
 *   static SDL_GPUBuffer *vertex_buffer = NULL;
 *   static SDL_GPUBuffer *index_buffer  = NULL;
 *   static float anim_time = 0.0f;
 *   static Uint64 last_ticks = 0;
 * ============================================================================= */

/* Keep: static SDL_GPUDevice *gpu_device = NULL;           */
/* Keep: static SDL_GPUGraphicsPipeline *pipeline = NULL;   */
static SDL_GPUBuffer *vertex_buffer = NULL;   /* quad vertices */
static SDL_GPUBuffer *index_buffer  = NULL;   /* quad indices  */
/* anim_time / last_ticks are no longer needed; remove them  */


/* =============================================================================
 * BLOCK 3 — Replace create_cube_buffers() with create_quad_buffers().
 *
 * Original function: create_cube_buffers()  (~lines 557-630)
 * ============================================================================= */

static bool create_quad_buffers(void)
{
    /* Four corners of the menu quad in LOCAL space.
     * UV (0,0) = top-left of the 640×480 surface.
     * The surface pixel origin is top-left so V is NOT flipped here;
     * if your image appears upside-down just swap t0/t1 below. */
    PositionUVVertex vertices[4] = {
        /* top-left  */ { -QUAD_HALF_W,  QUAD_HALF_H, QUAD_DEPTH,  0.0f, 0.0f },
        /* top-right */ {  QUAD_HALF_W,  QUAD_HALF_H, QUAD_DEPTH,  1.0f, 0.0f },
        /* bot-right */ {  QUAD_HALF_W, -QUAD_HALF_H, QUAD_DEPTH,  1.0f, 1.0f },
        /* bot-left  */ { -QUAD_HALF_W, -QUAD_HALF_H, QUAD_DEPTH,  0.0f, 1.0f },
    };

    /* Two triangles, CCW winding when viewed from -Z (front face) */
    Uint16 indices[6] = { 0, 1, 2,  0, 2, 3 };

    SDL_GPUBufferCreateInfo vb_info = {
        .usage = SDL_GPU_BUFFERUSAGE_VERTEX,
        .size  = sizeof(vertices)
    };
    vertex_buffer = SDL_CreateGPUBuffer(gpu_device, &vb_info);
    if (!vertex_buffer) {
        SDL_Log("create_quad_buffers: vertex buffer failed: %s", SDL_GetError());
        return false;
    }

    SDL_GPUBufferCreateInfo ib_info = {
        .usage = SDL_GPU_BUFFERUSAGE_INDEX,
        .size  = sizeof(indices)
    };
    index_buffer = SDL_CreateGPUBuffer(gpu_device, &ib_info);
    if (!index_buffer) {
        SDL_Log("create_quad_buffers: index buffer failed: %s", SDL_GetError());
        return false;
    }

    /* Upload via transfer buffer */
    SDL_GPUTransferBufferCreateInfo tb_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = sizeof(vertices) + sizeof(indices)
    };
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(gpu_device, &tb_info);
    if (!tb) {
        SDL_Log("create_quad_buffers: transfer buffer failed: %s", SDL_GetError());
        return false;
    }

    void *data = SDL_MapGPUTransferBuffer(gpu_device, tb, false);
    SDL_memcpy(data,                        vertices, sizeof(vertices));
    SDL_memcpy((Uint8*)data + sizeof(vertices), indices,  sizeof(indices));
    SDL_UnmapGPUTransferBuffer(gpu_device, tb);

    SDL_GPUCommandBuffer *cmd  = SDL_AcquireGPUCommandBuffer(gpu_device);
    SDL_GPUCopyPass      *pass = SDL_BeginGPUCopyPass(cmd);

    SDL_UploadToGPUBuffer(pass,
        &(SDL_GPUTransferBufferLocation){ .transfer_buffer = tb, .offset = 0 },
        &(SDL_GPUBufferRegion){ .buffer = vertex_buffer, .offset = 0, .size = sizeof(vertices) },
        false);

    SDL_UploadToGPUBuffer(pass,
        &(SDL_GPUTransferBufferLocation){ .transfer_buffer = tb, .offset = sizeof(vertices) },
        &(SDL_GPUBufferRegion){ .buffer = index_buffer, .offset = 0, .size = sizeof(indices) },
        false);

    SDL_EndGPUCopyPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(gpu_device, tb);

    SDL_Log("create_quad_buffers: %.0fx%.0f m quad ready", QUAD_HALF_W*2, QUAD_HALF_H*2);
    return true;
}


/* =============================================================================
 * BLOCK 4 — Menu GPU texture + sampler creation.
 *           Call this once after create_quad_buffers() (e.g. inside
 *           create_swapchains() where create_cube_buffers() was called).
 * ============================================================================= */

static bool create_menu_texture(void)
{
    SDL_GPUTextureCreateInfo tex_info = {
        .type               = SDL_GPU_TEXTURETYPE_2D,
        .format             = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .width              = MENU_W,
        .height             = MENU_H,
        .layer_count_or_depth = 1,
        .num_levels         = 1,
        .sample_count       = SDL_GPU_SAMPLECOUNT_1,
        .usage              = SDL_GPU_TEXTUREUSAGE_SAMPLER |
                              SDL_GPU_TEXTUREUSAGE_TRANSFER_DST,
        .props              = 0
    };
    menu_gpu_texture = SDL_CreateGPUTexture(gpu_device, &tex_info);
    if (!menu_gpu_texture) {
        SDL_Log("create_menu_texture: %s", SDL_GetError());
        return false;
    }

    SDL_GPUSamplerCreateInfo samp_info = {
        .min_filter        = SDL_GPU_FILTER_LINEAR,
        .mag_filter        = SDL_GPU_FILTER_LINEAR,
        .mipmap_mode       = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
        .address_mode_u    = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v    = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_w    = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    };
    menu_sampler = SDL_CreateGPUSampler(gpu_device, &samp_info);
    if (!menu_sampler) {
        SDL_Log("create_menu_texture: sampler: %s", SDL_GetError());
        return false;
    }

    SDL_Log("create_menu_texture: %dx%d RGBA8 texture ready", MENU_W, MENU_H);
    return true;
}


/* =============================================================================
 * BLOCK 5 — Upload the current menu frame from the SDL surface into the GPU
 *           texture.  Call this once per XR frame, before the render pass.
 *
 * The `surface` global (640×480 RGB565) is always written by FlipBuffers()
 * before the XR path runs, so the pixels are up-to-date.
 * ============================================================================= */

static void upload_menu_texture(SDL_GPUCommandBuffer *cmd)
{
    if (!surface || !menu_gpu_texture) return;

    /* Convert RGB565 → RGBA8 in software.
     * This runs on the CPU; it's ~1.2 MB of work (~300k pixels × 4 bytes).
     * Acceptable at 72 Hz for a menu screen; revisit for in-game use. */
    const Uint16 *src = (const Uint16 *)surface->pixels;
    Uint8        *dst = menu_rgba;

    for (int i = 0; i < MENU_W * MENU_H; i++) {
        Uint16 p = src[i];
        dst[0] = (Uint8)(((p >> 11) & 0x1F) * 255 / 31);  /* R */
        dst[1] = (Uint8)(((p >>  5) & 0x3F) * 255 / 63);  /* G */
        dst[2] = (Uint8)(((p      ) & 0x1F) * 255 / 31);  /* B */
        dst[3] = 255;                                        /* A */
        dst += 4;
    }

    /* Upload via a per-frame transfer buffer (small alloc, released immediately) */
    Uint32 pixel_bytes = MENU_W * MENU_H * 4;
    SDL_GPUTransferBufferCreateInfo tb_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = pixel_bytes
    };
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(gpu_device, &tb_info);
    if (!tb) return;

    void *mapped = SDL_MapGPUTransferBuffer(gpu_device, tb, false);
    SDL_memcpy(mapped, menu_rgba, pixel_bytes);
    SDL_UnmapGPUTransferBuffer(gpu_device, tb);

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo src_info = {
        .transfer_buffer = tb,
        .offset          = 0,
        .pixels_per_row  = MENU_W,
        .rows_per_layer  = MENU_H,
    };
    SDL_GPUTextureRegion dst_region = {
        .texture  = menu_gpu_texture,
        .mip_level = 0,
        .layer    = 0,
        .x = 0, .y = 0, .z = 0,
        .w = MENU_W, .h = MENU_H, .d = 1
    };
    SDL_UploadToGPUTexture(copy, &src_info, &dst_region, false);

    SDL_EndGPUCopyPass(copy);

    /* Release immediately — the copy has been recorded into cmd */
    SDL_ReleaseGPUTransferBuffer(gpu_device, tb);
}


/* =============================================================================
 * BLOCK 6 — Replace load_shader() / create_pipeline() for the textured quad.
 *
 * The textured pipeline needs:
 *   Vertex shader:   in vec3 pos, in vec2 uv  → out vec2 vUV; gl_Position = mvp * vec4(pos,1)
 *   Fragment shader: uniform sampler2D uTex    → outColor = texture(uTex, vUV)
 *   Uniform buffer 0 (vertex):  mat4 mvp  (64 bytes)
 *   Sampler binding 0 (fragment): the menu texture
 *
 * GLSL sources to compile offline:
 * ---------------------------------
 * -- quad.vert --
 * #version 450
 * layout(location=0) in vec3 aPos;
 * layout(location=1) in vec2 aUV;
 * layout(set=1, binding=0) uniform UBO { mat4 mvp; };
 * layout(location=0) out vec2 vUV;
 * void main() { vUV = aUV; gl_Position = mvp * vec4(aPos, 1.0); }
 *
 * -- quad.frag --
 * #version 450
 * layout(location=0) in  vec2 vUV;
 * layout(location=0) out vec4 outColor;
 * layout(set=2, binding=0) uniform sampler2D uTex;
 * void main() { outColor = texture(uTex, vUV); }
 *
 * Compile with glslangValidator:
 *   glslangValidator -V quad.vert -o quad_vert.spv
 *   glslangValidator -V quad.frag -o quad_frag.spv
 * Then xxd -i quad_vert.spv > quad_vert_spv.h  (etc.)
 * and #include them above this file.
 *
 * The load_quad_shader() below mirrors load_shader() exactly — swap in your
 * real blob arrays once you have them compiled.
 * ============================================================================= */

/* Forward-declare your shader blobs here once compiled:
 *   extern const unsigned char quad_vert_spv[];
 *   extern const unsigned int  quad_vert_spv_len;
 *   extern const unsigned char quad_frag_spv[];
 *   extern const unsigned int  quad_frag_spv_len;
 */

static SDL_GPUShader *load_quad_shader(bool is_vertex)
{
    SDL_GPUShaderCreateInfo ci = {
        .num_samplers        = is_vertex ? 0 : 1,   /* frag samples menu tex */
        .num_uniform_buffers = is_vertex ? 1 : 0,   /* vert receives MVP     */
        .num_storage_buffers = 0,
        .num_storage_textures = 0,
        .stage = is_vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT,
        .props = 0,
    };

    SDL_GPUShaderFormat fmt = SDL_GetGPUShaderFormats(gpu_device);
    if (fmt & SDL_GPU_SHADERFORMAT_SPIRV) {
        ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
        if (is_vertex) {
            /* TODO: ci.code = quad_vert_spv; ci.code_size = quad_vert_spv_len; */
            ci.entrypoint = "main";
        } else {
            /* TODO: ci.code = quad_frag_spv; ci.code_size = quad_frag_spv_len; */
            ci.entrypoint = "main";
        }
    } else if (fmt & SDL_GPU_SHADERFORMAT_DXIL) {
        ci.format = SDL_GPU_SHADERFORMAT_DXIL;
        /* TODO: fill DXIL blobs */
        ci.entrypoint = "main";
    } else {
        SDL_Log("load_quad_shader: no supported format");
        return NULL;
    }

    return SDL_CreateGPUShader(gpu_device, &ci);
}

static bool create_quad_pipeline(SDL_GPUTextureFormat color_format)
{
    SDL_GPUShader *vert = load_quad_shader(true);
    SDL_GPUShader *frag = load_quad_shader(false);
    if (!vert || !frag) {
        if (vert) SDL_ReleaseGPUShader(gpu_device, vert);
        if (frag) SDL_ReleaseGPUShader(gpu_device, frag);
        return false;
    }

    SDL_GPUGraphicsPipelineCreateInfo pi = {
        .vertex_shader   = vert,
        .fragment_shader = frag,
        .target_info = {
            .num_color_targets = 1,
            .color_target_descriptions = (SDL_GPUColorTargetDescription[]){{
                .format = color_format,
                /* Straight alpha-over (pre-multiplied not needed for opaque menu) */
                .blend_state = {
                    .enable_blend        = true,
                    .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                    .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                    .color_blend_op      = SDL_GPU_BLENDOP_ADD,
                    .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                    .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO,
                    .alpha_blend_op      = SDL_GPU_BLENDOP_ADD,
                },
            }},
            .has_depth_stencil_target = true,
            .depth_stencil_format     = DEPTH_FORMAT,
        },
        .depth_stencil_state = {
            .enable_depth_test  = true,
            .enable_depth_write = true,
            .compare_op         = SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
        },
        .rasterizer_state = {
            /* No back-face culling on a flat quad — both eyes can see either face */
            .cull_mode  = SDL_GPU_CULLMODE_NONE,
            .fill_mode  = SDL_GPU_FILLMODE_FILL,
        },
        .vertex_input_state = {
            .num_vertex_buffers = 1,
            .vertex_buffer_descriptions = (SDL_GPUVertexBufferDescription[]){{
                .slot       = 0,
                .pitch      = sizeof(PositionUVVertex),
                .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
            }},
            .num_vertex_attributes = 2,
            .vertex_attributes = (SDL_GPUVertexAttribute[]){{
                /* location 0: vec3 position */
                .location   = 0,
                .buffer_slot = 0,
                .format     = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
                .offset     = offsetof(PositionUVVertex, x),
            }, {
                /* location 1: vec2 uv */
                .location   = 1,
                .buffer_slot = 0,
                .format     = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
                .offset     = offsetof(PositionUVVertex, u),
            }},
        },
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
    };

    pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pi);

    SDL_ReleaseGPUShader(gpu_device, vert);
    SDL_ReleaseGPUShader(gpu_device, frag);

    if (!pipeline) {
        SDL_Log("create_quad_pipeline: %s", SDL_GetError());
        return false;
    }
    SDL_Log("create_quad_pipeline: OK (format %d)", color_format);
    return true;
}


/* =============================================================================
 * BLOCK 7 — Updated create_swapchains() tail section.
 *
 * In the original create_swapchains(), the end of the function reads:
 *
 *     if (view_count > 0 && pipeline == NULL) {
 *         if (!create_pipeline(vr_swapchains[0].format)) return false;
 *         if (!create_cube_buffers())                    return false;
 *     }
 *
 * Replace those 4 lines with:
 * ============================================================================= */

    if (view_count > 0 && pipeline == NULL) {
        if (!create_quad_pipeline(vr_swapchains[0].format)) return false;
        if (!create_quad_buffers())                          return false;
        if (!create_menu_texture())                          return false;
    }


/* =============================================================================
 * BLOCK 8 — Replace the render body inside render_frame().
 *
 * Replace everything from:
 *     if (pipeline && vertex_buffer && index_buffer) {
 *         ...cube loop...
 *     }
 * with the textured-quad draw below.  The surrounding render-pass setup
 * (acquire swapchain image, build view/proj matrices, begin/end pass,
 *  release swapchain, build proj_views[i]) is unchanged.
 * ============================================================================= */

        /* --- Upload menu pixels once per frame (before the per-eye loop) ---
         * Move this call to just BEFORE the "for (Uint32 i = 0; i < view_count; i++)"
         * loop inside render_frame(), using the same cmd_buf.              */
        upload_menu_texture(cmd_buf);

        /* --- Inside the per-eye loop, replace the cube draw section --- */
        if (pipeline && vertex_buffer && index_buffer && menu_gpu_texture && menu_sampler) {
            SDL_BindGPUGraphicsPipeline(render_pass, pipeline);

            SDL_GPUViewport viewport = {
                0, 0,
                (float)swapchain->size.width,
                (float)swapchain->size.height,
                0.0f, 1.0f
            };
            SDL_SetGPUViewport(render_pass, &viewport);

            SDL_Rect scissor = { 0, 0, swapchain->size.width, swapchain->size.height };
            SDL_SetGPUScissor(render_pass, &scissor);

            /* Bind the vertex and index buffers */
            SDL_BindGPUVertexBuffers(render_pass, 0,
                &(SDL_GPUBufferBinding){ vertex_buffer, 0 }, 1);
            SDL_BindGPUIndexBuffer(render_pass,
                &(SDL_GPUBufferBinding){ index_buffer, 0 },
                SDL_GPU_INDEXELEMENTSIZE_16BIT);

            /* Bind the menu texture to sampler slot 0 */
            SDL_GPUTextureSamplerBinding tsb = {
                .texture = menu_gpu_texture,
                .sampler = menu_sampler,
            };
            SDL_BindGPUFragmentSamplers(render_pass, 0, &tsb, 1);

            /* Build model matrix: identity (quad is already in LOCAL space).
             * If you later want head-locked placement, multiply by the inverse
             * of the view matrix here. */
            Mat4 model = (Mat4){{ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }};  /* identity */

            Mat4 mv  = Mat4_Multiply(model, view_matrix);
            Mat4 mvp = Mat4_Multiply(mv,    proj_matrix);

            /* Push MVP into uniform buffer slot 0 (vertex stage) */
            SDL_PushGPUVertexUniformData(cmd_buf, 0, &mvp, sizeof(mvp));

            /* Draw the quad: 6 indices, 1 instance */
            SDL_DrawGPUIndexedPrimitives(render_pass, 6, 1, 0, 0, 0);
        }


/* =============================================================================
 * BLOCK 9 — Cleanup additions for quit().
 *
 * In quit(), after the existing pipeline/vertex/index buffer release blocks,
 * add:
 * ============================================================================= */

    if (menu_sampler) {
        SDL_ReleaseGPUSampler(gpu_device, menu_sampler);
        menu_sampler = NULL;
    }
    if (menu_gpu_texture) {
        SDL_ReleaseGPUTexture(gpu_device, menu_gpu_texture);
        menu_gpu_texture = NULL;
    }


/* =============================================================================
 * INTEGRATION CHECKLIST
 * =====================
 * 1. Remove / comment out all cube-related code:
 *      - PositionColorVertex typedef
 *      - CUBE_HALF_SIZE, NUM_CUBES, cube_positions[], cube_scales[], cube_speeds[]
 *      - anim_time, last_ticks globals
 *      - create_cube_buffers()
 *      - load_shader() (if only used for cubes)
 *      - create_pipeline()  (replaced by create_quad_pipeline)
 *
 * 2. Add BLOCK 1 globals near the top (after the XR state block).
 *
 * 3. Add BLOCK 2 buffer/state declarations (replace existing ones).
 *
 * 4. Add functions BLOCK 3–6 before create_swapchains().
 *
 * 5. In create_swapchains(), swap the pipeline+buffer creation tail (BLOCK 7).
 *
 * 6. In render_frame():
 *    a. Before the per-eye loop, acquire cmd_buf then call upload_menu_texture(cmd_buf).
 *    b. Inside the per-eye loop, replace the cube draw with BLOCK 8.
 *
 * 7. In quit(), add BLOCK 9 sampler/texture release.
 *
 * 8. Compile your quad.vert / quad.frag shaders to SPIR-V, include the blobs,
 *    and wire them into load_quad_shader() (the TODO stubs in BLOCK 6).
 *
 * SHADER NOTE — Android / OpenXR on Quest
 * ----------------------------------------
 * Quest uses Vulkan under OpenXR, so SDL GPU picks SPIR-V.
 * glslangValidator flags for mobile-safe SPIR-V:
 *   glslangValidator -V --target-env vulkan1.1 quad.vert -o quad_vert.spv
 *   glslangValidator -V --target-env vulkan1.1 quad.frag -o quad_frag.spv
 *
 * PERFORMANCE NOTE
 * ----------------
 * upload_menu_texture() does a CPU RGB565→RGBA8 conversion + GPU transfer
 * every frame.  For the main menu (30–60 Hz) this is fine.  For in-game use,
 * consider:
 *   - Rendering the game directly into an SDL_GPUTexture via a framebuffer
 *     instead of going through the OpenGL ES surface.
 *   - Or using an EGLImage / VkExternalMemory extension to share the texture
 *     handle between the two contexts without a CPU copy.
 * ============================================================================= */
