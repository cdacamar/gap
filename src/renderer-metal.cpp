#include "renderer.h"

#include "assets.h"
#include "cmd-buffer.h"
#include "config.h"
#include "constants.h"
#include "feed.h"
#include "util.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace Render
{
  namespace
  {
    struct MetalWindowData
    {
      CAMetalLayer *layer;
      id<MTLTexture> scratch_color;
      id<MTLTexture> stage_color;
      id<CAMetalDrawable> drawable;
      id<MTLCommandBuffer> cmd_buffer;
    };

    struct MetalEntity
    {
      MetalEntity *next;
      MetalEntity *prev;
      union
      {
        id<MTLTexture> texture;
        id<MTLBuffer> buffer;
      };
      uint64_t gen;
    };

    struct MetalEntityList
    {
      MetalEntity *first;
      MetalEntity *last;
      uint64_t count;
    };

    struct MetalArena
    {
      MetalArena *prev;
      MetalArena *current;
      uint64_t pos;
      uint64_t res;
      id<MTLBuffer> mtl_buffer;
    };

    struct MetalAlloc
    {
      uint64_t offset_in_buffer;
      id<MTLBuffer> buffer;
      uint8_t *v;
    };

    struct MetalUniforms
    {
      Vec2f resolution;
      float time;
      float camera_coord_factor;
      Vec2f camera_scale;
      Vec2f camera_pos;
      Vec2f custom_vec2_value1;
      Vec2f custom_vec2_value2;
      Vec2f custom_vec2_value3;
    };

    struct MetalRenderData
    {
      MetalArena *metal_arena;
      Arena::Arena *render_arena;
      ScreenDimensions screen_size;
      MetalEntity *entity_free_lst;
      MetalEntityList tex_list;
      Arena::Arena *flush_arena;
      MetalEntityList flush_buffer_list;
      uint64_t entity_gen;
      id<MTLDevice> mtl_device;
      id<MTLCommandQueue> mtl_command_queue;
      id<MTLRenderPipelineState> pipelines[rep(FragShader::Count)];
      id<MTLBuffer> scratch_buffer_64k;
      int requested_frames;

      //- brt: uniforms
      float time;
      float camera_coord_factor;
      Vec2f camera_scale;
      Vec2f camera_pos;
      Vec2f resolution;
    };

    MetalWindowData impl_metal_wnd_data;
    MetalRenderData impl_metal_rend_data;

    MetalWindowData *metal_wind_backend_data()
    {
      return &impl_metal_wnd_data;
    }

    MetalRenderData *metal_render_backend_data()
    {
      return &impl_metal_rend_data;
    }

    MetalEntity *push_render_entity(MetalRenderData *data, MetalEntityList *lst)
    {
      MetalEntity *node = data->entity_free_lst;
      if (node != nullptr)
      {
        SLLStackPop(data->entity_free_lst);
        zero_bytes(node);
      }
      else
      {
        node = Arena::push_array<MetalEntity>(data->render_arena, 1);
      }
      node->gen = data->entity_gen++;
      DLLPushBack(lst->first, lst->last, node);
      ++lst->count;
      return node;
    }

    void release_render_entity(MetalRenderData *data, MetalEntityList *lst, MetalEntity *e)
    {
      DLLRemove(lst->first, lst->last, e);
      --lst->count;
      SLLStackPush(data->entity_free_lst, e);
    }

    MetalAlloc metal_push_aligned(uint64_t size, uint64_t align)
    {
      MetalRenderData *data = metal_render_backend_data();
      if (data->metal_arena == nullptr)
      {
        data->metal_arena = Arena::push_array<MetalArena>(data->flush_arena, 1);
        data->metal_arena->current = data->metal_arena;
        data->metal_arena->mtl_buffer = data->scratch_buffer_64k;
        data->metal_arena->res = KB(64);
      }

      MetalArena *current = data->metal_arena->current;
      uint64_t pos_pre = Arena::align_pow_2(current->pos, align);
      uint64_t pos_pst = pos_pre + size;

      //- brt: chain if needed
      if (current->res < pos_pst)
      {
        MetalArena *new_block = Arena::push_array<MetalArena>(data->flush_arena, 1);
        {
          uint64_t flushed_buffer_size = size;
          flushed_buffer_size += MB(1) - 1;
          flushed_buffer_size -= flushed_buffer_size % MB(1);
          new_block->res = flushed_buffer_size;
          new_block->mtl_buffer = [data->mtl_device newBufferWithLength:new_block->res
                                                                options:MTLResourceStorageModeShared];
        }
        SLLStackPush_N(data->metal_arena->current, new_block, prev);
        current = new_block;
        pos_pre = Arena::align_pow_2(current->pos, align);
        pos_pst = pos_pre+size;

        //- brt: push buffer to flush list
        MetalEntity *buf = push_render_entity(data, &data->flush_buffer_list);
        buf->buffer = new_block->mtl_buffer;
      }

      //- brt: push onto current block
      MetalAlloc result = {};
      if (current->res >= pos_pst)
      {
        result.buffer = current->mtl_buffer;
        result.offset_in_buffer = pos_pre;
        result.v = (uint8_t *)current->mtl_buffer.contents + pos_pre;
        current->pos = pos_pst;
      }

      return result;
    }

    BasicTexture to_basic_texture(MetalEntity *e)
    {
      return BasicTexture{ reinterpret_cast<PrimitiveType<BasicTexture>>(e) };
    }

    MetalEntity *basic_texture_metal(BasicTexture tex)
    {
      return reinterpret_cast<MetalEntity *>(tex);
    }

    // brt: FIXME: reorder this crap
    NSString *ns_string_from_str8(String8 string)
    {
      NSString *result = [[NSString alloc] initWithBytes:string.str
                                                  length:string.size
                                                encoding:NSUTF8StringEncoding];
      return result;
    }
  }

  struct FrameRenderer { };

  bool os_init_renderer_window(OS::OSWindow wind)
  {
    MetalWindowData *wnd_data = metal_wind_backend_data();
    MetalRenderData *rend_data = metal_render_backend_data();
    NSWindow *ns_win = reinterpret_cast<NSWindow *>(wind);
    wnd_data->layer = [CAMetalLayer layer];
    wnd_data->layer.autoresizingMask = kCALayerHeightSizable | kCALayerWidthSizable;
    wnd_data->layer.needsDisplayOnBoundsChange = YES;
    // brt: FIXME: this is created before the call to Render::init. I will need to move some things around
    wnd_data->layer.device = MTLCreateSystemDefaultDevice();
    wnd_data->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    wnd_data->layer.framebufferOnly = YES;
    wnd_data->layer.maximumDrawableCount = 2;
    ns_win.contentView.layer = wnd_data->layer;
    bool good = true;
    return good;
  }

  void os_swap_buffers(OS::OSWindow)
  {
    MetalWindowData *wnd_data = metal_wind_backend_data();
    MetalRenderData *rend_data = metal_render_backend_data();
  }

  FrameRenderer* make_platform_renderer(Arena::Arena* arena)
  {
    return Arena::push_array<FrameRenderer>(arena, 1);
  }

  void update_resolution(FrameRenderer*, Vec2f new_res)
  {
    MetalRenderData *rend_data = metal_render_backend_data();
    rend_data->resolution = new_res;
  }

  void update_time(FrameRenderer*, float app_time, float /*dt*/)
  {
    MetalRenderData *rend_data = metal_render_backend_data();
    rend_data->time = app_time;
  }

  bool init(const ScreenDimensions& screen)
  {
    MetalRenderData *rend_data = metal_render_backend_data();
    auto scratch = Arena::scratch_begin(Arena::no_conflicts);
    bool good = true;
    
    //- brt: init metal
    rend_data->mtl_device = MTLCreateSystemDefaultDevice();
    rend_data->mtl_command_queue = [rend_data->mtl_device newCommandQueue];
    rend_data->scratch_buffer_64k = [rend_data->mtl_device newBufferWithLength:KB(16) options:MTLResourceStorageModeShared];

    //- brt: setup uniform defaults
    {
      rend_data->camera_coord_factor = Constants::shader_scale_factor;
      rend_data->camera_scale = 3.0f;
    }

    @autoreleasepool
    {
      //- brt: compile shaders
      id<MTLLibrary> mtl_library = nil;
      {
        auto tmp = Arena::temp_begin(scratch.arena);
        Assets::AssetID asset_id = Assets::AssetID::ShadersMSL;
        Assets::AssetBuffer ass_buf = {};
        ass_buf.len = rep(Assets::asset_length(asset_id));
        ass_buf.buf = Arena::push_array_no_zero<uint8_t>(tmp.arena, ass_buf.len);
        if (not Assets::populate_asset(ass_buf, asset_id))
        {
          Assets::AssetDescription desc = Assets::describe(asset_id);
          String8 msg = str8_fmt(scratch.arena, "Failed to load shader asset: '%S'", desc.proxy_file);
          fprintf(stderr, "%.*s\n", static_cast<int>(msg.size), msg.str);
          good = false;
        }
        if (good)
        {
          NSError *error = nil;
          NSString *shaders_src = ns_string_from_str8(str8((char *)ass_buf.buf, ass_buf.len));
          MTLCompileOptions *options = [[[MTLCompileOptions alloc] init] autorelease];
          mtl_library = [rend_data->mtl_device newLibraryWithSource:shaders_src
                                                            options:options
                                                              error:&error];
          if (error)
          {
            Assets::AssetDescription desc = Assets::describe(asset_id);
            String8 reason = str8_cstr((char *)[[error localizedDescription] cStringUsingEncoding:NSUTF8StringEncoding]);
            String8 msg = str8_fmt(scratch.arena, "Failed to compile shader file '%s': '%S'", desc.proxy_file, reason);
            fprintf(stderr, "%.*s\n", static_cast<int>(msg.size), msg.str);
            good = false;
          }
        }
        Arena::temp_end(tmp);
      }

      //- brt: create PSOs
      if (good)
        for EachIndex(frag_idx, rep(FragShader::Count))
      {
        NSString *frag_name = nil;
        switch (FragShader(frag_idx))
        {
          case FragShader::Image:         frag_name = @"fs_image"; break;
          case FragShader::Text:          frag_name = @"fs_text"; break;
          case FragShader::TextSubpixel:  frag_name = @"fs_text_subpixel"; break;
          case FragShader::BlurHorizVert: frag_name = @"fs_blur_horiz"; break;
        }

        MTLRenderPipelineDescriptor *mtl_pipeline_state_desc = [[MTLRenderPipelineDescriptor alloc] init];
        mtl_pipeline_state_desc.vertexFunction = [mtl_library newFunctionWithName:@"vs_main"];
        mtl_pipeline_state_desc.fragmentFunction = [mtl_library newFunctionWithName:frag_name];
        mtl_pipeline_state_desc.vertexBuffers[0].mutability = MTLMutabilityImmutable;
        mtl_pipeline_state_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        if (FragShader(frag_idx) == FragShader::TextSubpixel)
        {
          mtl_pipeline_state_desc.colorAttachments[0].blendingEnabled = YES;
          mtl_pipeline_state_desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
          mtl_pipeline_state_desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSource1Color;
          mtl_pipeline_state_desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
          mtl_pipeline_state_desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
          mtl_pipeline_state_desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
          mtl_pipeline_state_desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        }
        else if (FragShader(frag_idx) == FragShader::Image)
        {
          mtl_pipeline_state_desc.colorAttachments[0].blendingEnabled = NO;
        }
        else
        {
          mtl_pipeline_state_desc.colorAttachments[0].blendingEnabled = YES;
          mtl_pipeline_state_desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
          mtl_pipeline_state_desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
          mtl_pipeline_state_desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
          mtl_pipeline_state_desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
          mtl_pipeline_state_desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
          mtl_pipeline_state_desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        }

        NSError *error = nil;
        rend_data->pipelines[frag_idx] = [rend_data->mtl_device newRenderPipelineStateWithDescriptor:mtl_pipeline_state_desc error:&error];
        if (error)
        {
          String8 reason = str8_cstr((char *)[[error localizedDescription] cStringUsingEncoding:NSUTF8StringEncoding]);
          String8 msg = str8_fmt(scratch.arena, "Failed to create render pipeline: '%S'", reason);
          fprintf(stderr, "%.*s\n", static_cast<int>(msg.size), msg.str);
          good = false;
        }
      }
    }

    if (good)
    {
      screen_resize(screen);
    }
    Arena::scratch_end(scratch);
    return good;
  }

  BasicTexture create_basic_texture(const ScreenDimensions& size)
  {
    MetalRenderData *rend_data = metal_render_backend_data();

    //- brt: allocate
    MetalEntity *tex = push_render_entity(rend_data, &rend_data->tex_list);

    //- brt: create texture
    MTLTextureDescriptor *desc =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:rep(size.width)
                                                        height:rep(size.height)
                                                     mipmapped:NO];
    // brt: NOTE: this is works on Apple CPUs
    desc.storageMode = MTLStorageModeShared;
    tex->texture = [rend_data->mtl_device newTextureWithDescriptor:desc];
    return to_basic_texture(tex);
  }

  void delete_basic_texture(BasicTexture tex)
  {
    MetalRenderData *data = metal_render_backend_data();
    MetalEntity *entity_tex = basic_texture_metal(tex);
    [entity_tex->texture release];
    entity_tex->texture = nil;
    release_render_entity(data, &data->tex_list, entity_tex);
  }

  void submit_basic_texture_data(BasicTexture tex, BasicTextureEntry entry)
  {
    MetalEntity *entity_tex = basic_texture_metal(tex);
    MTLRegion region = MTLRegionMake2D(rep(entry.offset_x),
                                       rep(entry.offset_y),
                                       rep(entry.width),
                                       rep(entry.height));
    if (rep(entry.width) > 0 && rep(entry.height) > 0)
    {
      [entity_tex->texture replaceRegion:region
                             mipmapLevel:0
                               withBytes:entry.buffer
                             bytesPerRow:rep(entry.width) * 4];
    }
  }

  BasicTexture create_glyph_texture(const ScreenDimensions& dim)
  {
    MetalRenderData *rend_data = metal_render_backend_data();

    //- brt: allocate
    MetalEntity *tex = push_render_entity(rend_data, &rend_data->tex_list);

    //- brt: create texture
    MTLTextureDescriptor *desc =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:rep(dim.width)
                                                        height:rep(dim.height)
                                                     mipmapped:NO];
    // brt: NOTE: this is works on Apple CPUs
    desc.storageMode = MTLStorageModeShared;
    tex->texture = [rend_data->mtl_device newTextureWithDescriptor:desc];
    return to_basic_texture(tex);
  }

  void submit_glyph_data(BasicTexture tex, GlyphEntry entry)
  {
    MetalEntity *entity_tex = basic_texture_metal(tex);
    MTLRegion region = MTLRegionMake2D(rep(entry.offset_x),
                                       rep(entry.offset_y),
                                       rep(entry.width),
                                       rep(entry.height));
    if (rep(entry.width) > 0 && rep(entry.height) > 0)
    {
      [entity_tex->texture replaceRegion:region
                             mipmapLevel:0
                               withBytes:entry.buffer
                             bytesPerRow:rep(entry.width) * 4];
    }
  }

  void request_frames()
  {
    MetalRenderData *data = metal_render_backend_data();
    data->requested_frames = 4;
  }

  int frames_remaining()
  {
    MetalRenderData *data = metal_render_backend_data();
    return data->requested_frames;
  }

  void consume_frame()
  {
    MetalRenderData *data = metal_render_backend_data();
    if (data->requested_frames > 0)
    {
      --data->requested_frames;
    }
  }

  void draw_cmd_list(FrameRenderer*, CmdBuffer::CmdList* cmd_lst)
  {
    MetalWindowData *wnd_data = metal_wind_backend_data();
    MetalRenderData *rend_data = metal_render_backend_data();

    id<MTLDevice> mtl_device = rend_data->mtl_device;
    id<MTLCommandBuffer> mtl_cmd_buffer = [rend_data->mtl_command_queue commandBuffer];
    wnd_data->cmd_buffer = mtl_cmd_buffer;
    MTLRenderPassDescriptor *mtl_pass_desc = [MTLRenderPassDescriptor renderPassDescriptor];
    mtl_pass_desc.colorAttachments[0].texture = wnd_data->stage_color;
    mtl_pass_desc.colorAttachments[0].loadAction = MTLLoadActionClear;
    mtl_pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
    const Vec4f& bg = Config::diff_colors().background;
    mtl_pass_desc.colorAttachments[0].clearColor = MTLClearColorMake(bg.x, bg.y, bg.z, bg.a);
    id<MTLRenderCommandEncoder> mtl_encoder = [mtl_cmd_buffer renderCommandEncoderWithDescriptor:mtl_pass_desc];

    // brt: setup primary viewport
    MTLViewport viewport =
    {
      0.0, 0.0,
      static_cast<float>(rep(rend_data->screen_size.width)),
      static_cast<float>(rep(rend_data->screen_size.height)),
      0.0, 1.0
    };
    [mtl_encoder setViewport:viewport];

    for EachIndex(idx, count_of<CmdBuffer::DrawListLayer>)
    {
      CmdBuffer::DrawListCollection* lst_c = &cmd_lst->draw_list[idx];
      for EachNode(lst_n, lst_c->first)
      {
        CmdBuffer::DrawList* lst = lst_n->lst;

        MetalAlloc mtl_verts = metal_push_aligned(lst->vert_buf.count * sizeof(CmdBuffer::DrawVertex), 256);
        memcpy(mtl_verts.v, lst->vert_buf.buf, lst->vert_buf.count * sizeof(CmdBuffer::DrawVertex));

        MetalAlloc mtl_indices = metal_push_aligned(lst->idx_buf.count * sizeof(CmdBuffer::Index), 256);
        memcpy(mtl_indices.v, lst->idx_buf.buf, lst->idx_buf.count * sizeof(CmdBuffer::Index));

        ScreenDimensions res = lst->screen;
        for EachNode(cmd_n, lst->cmd_buf.first)
        {
          const CmdBuffer::DrawCmd *cmd = &cmd_n->cmd;
          switch(cmd->sort)
          {
            case CmdBuffer::CmdSort::Standard:
            {
              const CmdBuffer::StandardData *std_data = &cmd->std_data;
              //- brt: setup viewport
              viewport.width   = static_cast<float>(rep(res.width));
              viewport.height  = static_cast<float>(rep(res.height));
              viewport.znear   = 0.0f;
              viewport.zfar    = 1.0f;
              viewport.originX = static_cast<float>(rep(cmd->clip_rect.offset_x));
              viewport.originY = rep(rend_data->screen_size.height) - static_cast<float>(rep(cmd->clip_rect.offset_y)) - rep(res.height);
              [mtl_encoder setViewport:viewport];

              //- brt: setup scissor
              MTLScissorRect rect = {0};
              rect.x = rep(cmd->clip_rect.offset_x);
              rect.y = rep(cmd->clip_rect.offset_y);
              rect.width = rep(cmd->clip_rect.width);
              rect.height = rep(cmd->clip_rect.height);
              [mtl_encoder setScissorRect:rect];

              //- brt: setup pipeline
              [mtl_encoder setRenderPipelineState:rend_data->pipelines[rep(std_data->frag)]]; 

              //- brt: bind resources
              //- brt: TODO: FIXME: switch these out according to vert type
              MetalUniforms uniforms = {};
              uniforms.resolution = rend_data->resolution;
              uniforms.time = rend_data->time;
              uniforms.camera_pos = rend_data->camera_pos;
              uniforms.camera_scale = rend_data->camera_scale;
              uniforms.camera_coord_factor = rend_data->camera_coord_factor;

              [mtl_encoder setVertexBytes:&uniforms
                                   length:sizeof(uniforms)
                          attributeStride:sizeof(uniforms)
                                  atIndex:0];

              [mtl_encoder setVertexBuffer:mtl_verts.buffer
                                    offset:mtl_verts.offset_in_buffer
                                   atIndex:1];

              MetalEntity *tex = basic_texture_metal(std_data->tex);
              [mtl_encoder setFragmentTexture:tex->texture
                                      atIndex:0];

              //- brt: draw
              [mtl_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                      indexCount:rep(std_data->idx_count)
                                       indexType:MTLIndexTypeUInt32
                                     indexBuffer:mtl_indices.buffer
                               indexBufferOffset:mtl_indices.offset_in_buffer + rep(std_data->idx_off) * 4];
            }break;
            case CmdBuffer::CmdSort::Blur:
            {
            }break;
            case CmdBuffer::CmdSort::CameraUpdate:
            {
              const Camera *camera = &cmd->camera_up;
              rend_data->camera_pos = camera->pos;
              rend_data->camera_scale = camera->scale;
              rend_data->camera_coord_factor = Constants::shader_scale_factor;
            }break;
            case CmdBuffer::CmdSort::ResolutionUpdate:
            {
              rend_data->resolution = { rep(cmd->res_up.width) + 0.0f, rep(cmd->res_up.height) + 0.0f };
              res = cmd->res_up;
            }break;
          }
        }
      }
    }

    [mtl_encoder endEncoding];
  }

  void apply_framebuffer(Render::FrameRenderer*, const ScreenDimensions& screen)
  {
  @autoreleasepool
  { 
    MetalWindowData *wnd_data = metal_wind_backend_data();
    MetalRenderData *rend_data = metal_render_backend_data();

    wnd_data->drawable = [wnd_data->layer nextDrawable];
    id<CAMetalDrawable> mtl_drawable = wnd_data->drawable;
    id<MTLDevice> mtl_device = rend_data->mtl_device;
    id<MTLCommandBuffer> mtl_cmd_buffer = wnd_data->cmd_buffer;
    MTLRenderPassDescriptor *mtl_pass_desc = [MTLRenderPassDescriptor renderPassDescriptor];
    mtl_pass_desc.colorAttachments[0].texture = mtl_drawable.texture;
    mtl_pass_desc.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    mtl_pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> mtl_encoder = [mtl_cmd_buffer renderCommandEncoderWithDescriptor:mtl_pass_desc];

    //- brt: setup viewport
    MTLViewport viewport = {};
    viewport.width   = static_cast<float>(rep(screen.width));
    viewport.height  = static_cast<float>(rep(screen.height));
    viewport.znear   = 0.0f;
    viewport.zfar    = 1.0f;
    viewport.originX = 0.0f;
    viewport.originY = 0.0f;
    [mtl_encoder setViewport:viewport];

    //- brt: setup scissor
    MTLScissorRect rect = {0};
    rect.x = 0;
    rect.y = 0;
    rect.width = rep(screen.width);
    rect.height = rep(screen.height);
    [mtl_encoder setScissorRect:rect];

    //- brt: setup pipeline
    [mtl_encoder setRenderPipelineState:rend_data->pipelines[rep(FragShader::Image)]]; 

    //- brt: create draw data
    MetalAlloc mtl_verts = metal_push_aligned(4 * sizeof(CmdBuffer::DrawVertex), 256);
    MetalAlloc mtl_indices = metal_push_aligned(6 * sizeof(CmdBuffer::Index), 256);
    {
      CmdBuffer::DrawVertex *verts = (CmdBuffer::DrawVertex *)mtl_verts.v;
      uint32_t *indices = (uint32_t*)mtl_indices.v;

      // 0 - 1  |  a - b
      // | \ |  |  | \ |
      // 3 - 2  |  d - c
      Vec2f a{ 0.f, 0.f };
      Vec2f c{ rep(screen.width)+0.0f, rep(screen.height)+0.0f };
      Vec2f b{ c.x, a.y };
      Vec2f d{ a.x, c.y };
      Vec2f uv_a{ 0.0f, 1.0f };
      Vec2f uv_c{ 1.0f, 0.0f };
      Vec2f uv_b{ uv_c.x, uv_a.y };
      Vec2f uv_d{ uv_a.x, uv_c.y };

      Vec4f color = hex_to_vec4f(0xffffffff);

      verts[0] = { .pos = a, .color = color, .uv = uv_a };
      verts[1] = { .pos = b, .color = color, .uv = uv_b };
      verts[2] = { .pos = c, .color = color, .uv = uv_c };
      verts[3] = { .pos = d, .color = color, .uv = uv_d };

      indices[0] = 0;
      indices[1] = 1;
      indices[2] = 2;
      indices[3] = 0;
      indices[4] = 2;
      indices[5] = 3;
    }

    //- brt: bind resources
    MetalUniforms uniforms = {};
    uniforms.resolution = rend_data->resolution;
    uniforms.time = rend_data->time;
    uniforms.camera_pos = rend_data->camera_pos;
    uniforms.camera_scale = rend_data->camera_scale;
    uniforms.camera_coord_factor = rend_data->camera_coord_factor;

    [mtl_encoder setVertexBytes:&uniforms
                         length:sizeof(uniforms)
                attributeStride:sizeof(uniforms)
                        atIndex:0];

    [mtl_encoder setVertexBuffer:mtl_verts.buffer
                          offset:mtl_verts.offset_in_buffer
                         atIndex:1];

    [mtl_encoder setFragmentTexture:wnd_data->stage_color atIndex:0];

    //- brt: draw
    [mtl_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:6
                             indexType:MTLIndexTypeUInt32
                           indexBuffer:mtl_indices.buffer
                     indexBufferOffset:mtl_indices.offset_in_buffer];
    [mtl_encoder endEncoding];
    [wnd_data->cmd_buffer presentDrawable:wnd_data->drawable];
    [wnd_data->cmd_buffer commit];
    [wnd_data->cmd_buffer waitUntilCompleted];
  }
  }

  void window_end_frame(OS::OSWindow window)
  {
    MetalRenderData *rend_data = metal_render_backend_data();
    for (;rend_data->flush_buffer_list.first != nullptr;)
    {
      MetalEntity *n = rend_data->flush_buffer_list.first;
      [n->buffer release];
      n->buffer = nil;
      release_render_entity(rend_data, &rend_data->flush_buffer_list, n);
    }
    rend_data->flush_buffer_list = {};
    rend_data->metal_arena = nullptr;
    Arena::clear(rend_data->flush_arena);
    OS::swap_buffers(window);
  }

  void screen_resize(const ScreenDimensions& screen)
  {
    MetalWindowData *wnd_data = metal_wind_backend_data();
    MetalRenderData *rend_data = metal_render_backend_data();

    //- brt: destroy old resources
    [wnd_data->scratch_color release];
    [wnd_data->stage_color release];
    wnd_data->scratch_color = nil;
    wnd_data->stage_color = nil;

    //- brt: create new resources
    MTLTextureDescriptor *color_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:(NSUInteger)screen.width
                                                                                         height:(NSUInteger)screen.height
                                                                                      mipmapped:NO];
    color_desc.textureType = MTLTextureType2D;
    color_desc.usage |= MTLTextureUsageRenderTarget;
    //color_desc.storageMode = MTLStorageModeMemoryless;
    color_desc.storageMode = MTLStorageModePrivate;
    wnd_data->scratch_color = [rend_data->mtl_device newTextureWithDescriptor:color_desc];
    wnd_data->stage_color = [rend_data->mtl_device newTextureWithDescriptor:color_desc];

    //- brt: resize drawable
    rend_data->screen_size = screen;
    wnd_data->layer.drawableSize = CGSizeMake(rep(screen.width), rep(screen.height));
  }

  bool init_renderer_arenas()
  {
    MetalRenderData *rend_data = metal_render_backend_data();
    rend_data->render_arena = Arena::alloc(Arena::default_params);
    rend_data->flush_arena = Arena::alloc(Arena::default_params);
    return true;
  }

  void display_renderer_version()
  {
    printf("Metal\n");
  }
}
