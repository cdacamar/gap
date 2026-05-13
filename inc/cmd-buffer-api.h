#pragma once

#include "types.h"
#include "vec.h"

namespace CmdBuffer
{
    struct DrawList;
    struct DrawListAllocator;
    struct CmdList;

    struct TimeUpdate
    {
        float dt;
        float app_time;
    };

    struct MultiColorInput
    {
        Vec4f upper_left;
        Vec4f upper_right;
        Vec4f lower_left;
        Vec4f lower_right;
    };

    struct QuadInput
    {
        Vec2f p0123[4];
    };

    struct QuadColors
    {
        Vec4f c0123[4];
    };

    // General.
    void commit_current(DrawList* lst);
    void new_frame(DrawList* lst, const ScreenDimensions& screen, TimeUpdate t);
    void end_frame(DrawList* lst);
    void end_frame(CmdList* cmd_lst);
    void init_alloc(DrawListAllocator* alloc);
    DrawList* alloc_draw_list();
    // This API may not release the draw list right away as it might be consumed during a render pass for the
    // current frame.
    void release_draw_list(DrawList* lst);

    // Queries.
    ClipRect current_clip(const DrawList& lst);
    Render::BasicTexture current_texture(const DrawList& lst);
    ScreenDimensions screen(const DrawList& lst);
    Vec2f screen_vec(const DrawList& lst);
    float delta_time(const DrawList& lst);
    float app_time(const DrawList& lst);
    const ColorPalette* current_palette(const DrawList& lst);

    // State interaction.
    void push_draw_list(CmdList* cmd_lst, DrawListLayer layer, DrawList* lst);
    void cmd_list_consumed(CmdList* cmd_lst);
    void push_clip(DrawList* lst, ClipRect clip);
    void pop_clip(DrawList* lst);
    void push_texture(DrawList* lst, Render::BasicTexture tex);
    void pop_texture(DrawList* lst);
    void push_color_palette(DrawList* lst, const ColorPalette& palette);
    void pop_color_palette(DrawList* lst);

    // Helpers.
    bool pos_outside_clip(DrawList* lst, Vec2f pos);
    bool box_outside_clip(DrawList* lst, const Vec4f& box);

    // Text.
    void start_glyph_run(DrawList* lst, Render::VertShader vert);

    // Images.
    void start_images(DrawList* lst, Render::VertShader vert);
    void start_icon_image_batch(DrawList* lst, Render::VertShader vert);
    void complete_icon_image_batch(DrawList* lst, Render::FragShader frag);

    // Shapes.
    void start_shapes(DrawList* lst, Render::VertShader vert);
    void solid_rect(DrawList* lst, Render::FragShader frag, const Vec2f& top_left, const Vec2f& size, const Vec4f& color);
    void multi_color_solid_rect(DrawList* lst, Render::FragShader frag, const Vec2f& top_left, const Vec2f& size, const MultiColorInput& colors);
    // Like the API above, but populates UV coords.  Do not use with BasicColor shader otherwise this will sample from the atlas texture.
    void solid_rect_uv_size(DrawList* lst, Render::FragShader frag, const Vec2f& top_left, const Vec2f& size, const Vec4f& color);
    void strike_rect(DrawList* lst, Render::FragShader frag, const Vec2f& top_left, const Vec2f& size, float thickness, const Vec4f& color);
    void line(DrawList* lst, Render::FragShader frag, const Vec2f& a, const Vec2f& b, float thickness, const Vec4f& color);
    void quad(DrawList* lst, Render::FragShader frag, const QuadInput& in, const Vec4f& color);
    void multi_color_quad(DrawList* lst, Render::FragShader frag, const QuadInput& in, const QuadColors& colors);
    void quad_image(DrawList* lst, Render::FragShader frag, const QuadInput& in, const Vec2f& uv_pos, const Vec2f& uv_size, const Vec4f& color);

    // General rendering.
    void render_icon_image(DrawList* lst, Render::FragShader frag, const Vec2f& pos, const Vec2f& size, const Vec2f& uv_pos, const Vec2f& uv_size, const Vec4f& color);
    void render_image(DrawList* lst, Render::FragShader frag, const Vec2f& pos, const Vec2f& size, const Vec2f& uv_pos, const Vec2f& uv_size, const Vec4f& color);
    void render_glyph(DrawList* lst, Render::FragShader frag, const Vec2f& pos, const Vec2f& size, const Vec2f& uv_pos, const Vec2f& uv_size, const Vec4f& color);
    void render_subpixel_glyph(DrawList* lst, Render::FragShader frag, const Vec2f& pos, const Vec2f& size, const Vec2f& uv_pos, const Vec2f& uv_size, const Vec4f& color, float px_offset);
    void batch_image(DrawList* lst, Render::BasicTexture tex, const Vec2f& pos, const Vec2f& size, const Vec2f& uv_pos, const Vec2f& uv_size, const Vec4f& color);
    void background_color(DrawList* lst, const Vec4f& color);

    // Effects rendering.
    void standard_window_blur(DrawList* lst, ClipRect clip);

    // Shader manipulation.
    void push_camera(DrawList* lst, const Render::Camera& camera);
    void push_resolution(DrawList* lst, const ScreenDimensions& res);
} // namespace CmdBuffer