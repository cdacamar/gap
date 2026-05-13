#include "cmd-buffer-api.h"

#include <cassert>

#include <algorithm>

#include "cmd-buffer.h"
#include "config.h"
#include "constants.h"
#include "list-helpers.h"
#include "ui-common.h"

namespace CmdBuffer
{
    namespace
    {
        DrawListEntityNode* push_node(DrawList* draw_lst, DrawListEntityList* lst)
        {
            DrawListEntityNode* node = nullptr;
            if (draw_lst->free_entity_list != nullptr)
            {
                node = draw_lst->free_entity_list;
                SLLStackPop(draw_lst->free_entity_list);
            }
            else
            {
                node = Arena::push_array<DrawListEntityNode>(draw_lst->entity_list_arena, 1);
            }
            SLLQueuePushFront(lst->first, lst->last, node);
            ++lst->count;
            return node;
        }

        // This is special because we don't want to push front (since this doesn't act like a stack),
        // we want to push back so our draw commands are read in the order they were added.
        void push_node_draw_cmd(DrawList* draw_lst, const DrawCmd& cmd)
        {
            DrawListEntityNode* node = nullptr;
            if (draw_lst->free_entity_list != nullptr)
            {
                node = draw_lst->free_entity_list;
                SLLStackPop(draw_lst->free_entity_list);
            }
            else
            {
                node = Arena::push_array<DrawListEntityNode>(draw_lst->entity_list_arena, 1);
            }
            node->cmd = cmd;
            SLLQueuePush(draw_lst->cmd_buf.first, draw_lst->cmd_buf.last, node);
            ++draw_lst->cmd_buf.count;
        }

        void pop_node(DrawList* draw_lst, DrawListEntityList* lst)
        {
            assert(lst->count != 0);
            DrawListEntityNode* node = lst->first;
            SLLQueuePop(lst->first, lst->last);
            --lst->count;
            SLLStackPush(draw_lst->free_entity_list, node);
        }

        DrawListEntityNode* top(const DrawListEntityList& lst)
        {
            return lst.first;
        }

        DrawVertex* bump_vert_buf(DrawList* lst, uint64_t x)
        {
            constexpr auto align = Arena::Alignment{ alignof(DrawVertex) };
            DrawVertex* next_segment = Arena::push_array_no_zero_aligned<DrawVertex>(lst->vert_buf_arena, x, align);
            // Note: we don't set the 'buf' here because it's seeded at the beginning of the frame.
            assert(lst->vert_buf.buf == nullptr or (lst->vert_buf.buf + lst->vert_buf.count == next_segment));
            lst->vert_buf.count += x;
            return next_segment;
        }

        Index* bump_index_buf(DrawList* lst, uint64_t x)
        {
            constexpr auto align = Arena::Alignment{ alignof(Index) };
            Index* next_segment = Arena::push_array_no_zero_aligned<Index>(lst->idx_buf_arena, x, align);
            // Note: we don't set the 'buf' here because it's seeded at the beginning of the frame.
            assert(lst->idx_buf.buf == nullptr or (lst->idx_buf.buf + lst->idx_buf.count == next_segment));
            lst->idx_buf.count += x;
            return next_segment;
        }

        bool header_match(const DrawCmd& a, const DrawCmd& b)
        {
            constexpr auto header_off = offsetof(DrawCmd, std_data.vert_off);
            return memcmp(&a, &b, header_off) == 0;
        }

        void setup_draw_cmd(DrawList* lst, DrawCmd* cmd)
        {
            cmd->std_data.vert_off = VertexOffset(lst->vert_buf.count);
            cmd->std_data.idx_off = IndexOffset(lst->idx_buf.count);
            cmd->std_data.idx_count = Cardinality{};
        }

        void bump_index(DrawList* lst, PrimitiveType<Index> dest)
        {
            // Don't allow this to overflow.
            assert(rep(lst->last_index) + dest > rep(lst->last_index));
            lst->last_index = extend(lst->last_index, dest);
        }

        struct VertFlags
        {
            float take_texel;
            float blend_black;
        };

        constexpr VertFlags no_vert_flags = { 0.f, 0.f };

        // 0 - 1  |  a - b
        // | \ |  |  | \ |
        // 3 - 2  |  d - c
        void render_quad(DrawList* lst,
                        const Vec2f& a, const Vec2f& c,
                        const Vec4f& col,
                        const Vec2f& uv_a, const Vec2f& uv_c,
                        const VertFlags& flags)
        {
            Vec2f b{ c.x, a.y };
            Vec2f d{ a.x, c.y };
            Vec2f uv_b{ uv_c.x, uv_a.y };
            Vec2f uv_d{ uv_a.x, uv_c.y };

            DrawVertex* vbuf = bump_vert_buf(lst, 4);
            vbuf[0] = { .pos = a, .color = col, .uv = uv_a, .cust1 = flags.take_texel, .cust2 = flags.blend_black };
            vbuf[1] = { .pos = b, .color = col, .uv = uv_b, .cust1 = flags.take_texel, .cust2 = flags.blend_black };
            vbuf[2] = { .pos = c, .color = col, .uv = uv_c, .cust1 = flags.take_texel, .cust2 = flags.blend_black };
            vbuf[3] = { .pos = d, .color = col, .uv = uv_d, .cust1 = flags.take_texel, .cust2 = flags.blend_black };

            // Indices.
            auto idx = lst->last_index;
            Index* ibuf = bump_index_buf(lst, 6);
            ibuf[0] = idx;
            ibuf[1] = extend(idx, 1);
            ibuf[2] = extend(idx, 2);

            ibuf[3] = idx;
            ibuf[4] = extend(idx, 2);
            ibuf[5] = extend(idx, 3);
            bump_index(lst, 4);
            // One per index.
            lst->current.std_data.idx_count = extend(lst->current.std_data.idx_count, 6);
        }

        // 0 - 1  |  a - b
        // | \ |  |  | \ |
        // 3 - 2  |  d - c
        void render_quad(DrawList* lst,
                        const QuadInput& q,
                        const Vec4f& col,
                        const Vec2f& uv_a, const Vec2f& uv_c,
                        const VertFlags& flags)
        {
            Vec2f uv_b{ uv_c.x, uv_a.y };
            Vec2f uv_d{ uv_a.x, uv_c.y };

            DrawVertex* vbuf = bump_vert_buf(lst, 4);
            vbuf[0] = { .pos = q.p0123[0], .color = col, .uv = uv_a, .cust1 = flags.take_texel, .cust2 = flags.blend_black };
            vbuf[1] = { .pos = q.p0123[1], .color = col, .uv = uv_b, .cust1 = flags.take_texel, .cust2 = flags.blend_black };
            vbuf[2] = { .pos = q.p0123[2], .color = col, .uv = uv_c, .cust1 = flags.take_texel, .cust2 = flags.blend_black };
            vbuf[3] = { .pos = q.p0123[3], .color = col, .uv = uv_d, .cust1 = flags.take_texel, .cust2 = flags.blend_black };

            // Indices.
            auto idx = lst->last_index;
            Index* ibuf = bump_index_buf(lst, 6);
            ibuf[0] = idx;
            ibuf[1] = extend(idx, 1);
            ibuf[2] = extend(idx, 2);

            ibuf[3] = idx;
            ibuf[4] = extend(idx, 2);
            ibuf[5] = extend(idx, 3);
            bump_index(lst, 4);
            // One per index.
            lst->current.std_data.idx_count = extend(lst->current.std_data.idx_count, 6);
        }

        // 0 - 1  |  a - b
        // | \ |  |  | \ |
        // 3 - 2  |  d - c
        void render_quad(DrawList* lst,
                        const QuadInput& q,
                        const QuadColors& col,
                        const Vec2f& uv_a, const Vec2f& uv_c)
        {
            Vec2f uv_b{ uv_c.x, uv_a.y };
            Vec2f uv_d{ uv_a.x, uv_c.y };

            DrawVertex* vbuf = bump_vert_buf(lst, 4);
            vbuf[0] = { .pos = q.p0123[0], .color = col.c0123[0], .uv = uv_a };
            vbuf[1] = { .pos = q.p0123[1], .color = col.c0123[1], .uv = uv_b };
            vbuf[2] = { .pos = q.p0123[2], .color = col.c0123[2], .uv = uv_c };
            vbuf[3] = { .pos = q.p0123[3], .color = col.c0123[3], .uv = uv_d };

            // Indices.
            auto idx = lst->last_index;
            Index* ibuf = bump_index_buf(lst, 6);
            ibuf[0] = idx;
            ibuf[1] = extend(idx, 1);
            ibuf[2] = extend(idx, 2);

            ibuf[3] = idx;
            ibuf[4] = extend(idx, 2);
            ibuf[5] = extend(idx, 3);
            bump_index(lst, 4);
            // One per index.
            lst->current.std_data.idx_count = extend(lst->current.std_data.idx_count, 6);
        }

        // 0 - 1  |  a - b
        // | \ |  |  | \ |
        // 3 - 2  |  d - c
        void render_multi_color_quad(DrawList* lst,
                        const Vec2f& a, const Vec2f& c,
                        const MultiColorInput& colors,
                        const Vec2f& uv_a, const Vec2f& uv_c)
        {
            Vec2f b{ c.x, a.y };
            Vec2f d{ a.x, c.y };
            Vec2f uv_b{ uv_c.x, uv_a.y };
            Vec2f uv_d{ uv_a.x, uv_c.y };

            DrawVertex* vbuf = bump_vert_buf(lst, 4);
            vbuf[0] = { .pos = a, .color = colors.lower_left,  .uv = uv_a };
            vbuf[1] = { .pos = b, .color = colors.lower_right, .uv = uv_b };
            vbuf[2] = { .pos = c, .color = colors.upper_right, .uv = uv_c };
            vbuf[3] = { .pos = d, .color = colors.upper_left,  .uv = uv_d };

            // Indices.
            auto idx = lst->last_index;
            Index* ibuf = bump_index_buf(lst, 6);
            ibuf[0] = idx;
            ibuf[1] = extend(idx, 1);
            ibuf[2] = extend(idx, 2);

            ibuf[3] = idx;
            ibuf[4] = extend(idx, 2);
            ibuf[5] = extend(idx, 3);
            bump_index(lst, 4);
            // One per index.
            lst->current.std_data.idx_count = extend(lst->current.std_data.idx_count, 6);
        }

        // 0 - 1  |  a - b
        // | \ |  |  | \ |
        // 3 - 2  |  d - c
        void render_quad_subpixel_adjust(DrawList* lst,
                        const Vec2f& a, const Vec2f& c,
                        const Vec4f& col,
                        const Vec2f& uv_a, const Vec2f& uv_c,
                        float subpixel_adjust)
        {
            Vec2f b{ c.x, a.y };
            Vec2f d{ a.x, c.y };
            Vec2f uv_b{ uv_c.x, uv_a.y };
            Vec2f uv_d{ uv_a.x, uv_c.y };

            DrawVertex* vbuf = bump_vert_buf(lst, 4);
            vbuf[0] = { .pos = a, .color = col, .uv = uv_a, .cust1 = subpixel_adjust };
            vbuf[1] = { .pos = b, .color = col, .uv = uv_b, .cust1 = subpixel_adjust };
            vbuf[2] = { .pos = c, .color = col, .uv = uv_c, .cust1 = subpixel_adjust };
            vbuf[3] = { .pos = d, .color = col, .uv = uv_d, .cust1 = subpixel_adjust };

            // Indices.
            auto idx = lst->last_index;
            Index* ibuf = bump_index_buf(lst, 6);
            ibuf[0] = idx;
            ibuf[1] = extend(idx, 1);
            ibuf[2] = extend(idx, 2);

            ibuf[3] = idx;
            ibuf[4] = extend(idx, 2);
            ibuf[5] = extend(idx, 3);
            bump_index(lst, 4);
            // One per index.
            lst->current.std_data.idx_count = extend(lst->current.std_data.idx_count, 6);
        }

        constexpr void normalize_vec_pts(float* dx, float* dy)
        {
            float n2 = *dx * *dx + *dy * *dy;
            if (n2 > 0.f)
            {
                n2 = 1.f / sqrtf(n2);
                *dx *= n2;
                *dy *= n2;
            }
        }

        constexpr void invert_normals(Vec2f* v)
        {
            float n2 = v->x * v->x + v->y * v->y;
            if (n2 > 0.000001f)
            {
                n2 = 1.f / n2;
                // Constrain the inverted normal to within a sane
                // threshold so we don't have runaway fringes.
                // Cribbed from ImGui IM_FIXNORMAL2F.
                constexpr float inv_norm_max = 100.f;
                if (n2 > inv_norm_max)
                {
                    n2 = inv_norm_max;
                }
                v->x *= n2;
                v->y *= n2;
            }
        }

        void apply_anti_alias(DrawList* lst, const QuadInput& in, const QuadColors& colors, float)
        {
            // We first need an array of normals and arrays of new quads to apply this to.
            Vec2f normals[4];
            // Quads are always 4 vertices.
            constexpr int count_n = 4;
            // Calculate normals (perpendicular) for each line segment.
            for (int i = 0, i_prev = count_n - 1; i < count_n; i_prev = i++)
            {
                // Since normals swap x and y, we'll flip them here.
                float dx = in.p0123[i].x - in.p0123[i_prev].x;
                float dy = in.p0123[i].y - in.p0123[i_prev].y;
                normalize_vec_pts(&dx, &dy);
                normals[i_prev].x = -dy;
                normals[i_prev].y = dx;
            }

            // We're creating an additional set of enclosing vertices which capture the
            // fringe edges.
            constexpr int vtx_count = 4 * 2;
            // For indices.  We need the initial 6 + 6 (per additional quad) * 4 (fringe quads).
            constexpr int idx_count = 6 + 6 * 4;
            DrawVertex* vbuf = bump_vert_buf(lst, vtx_count);
            auto idx = lst->last_index;
            Index* ibuf = bump_index_buf(lst, idx_count);
            // Add indices for the fill part.
            Index idx_inner = idx;
            Index idx_outer = extend(idx);
            for (int i = 2; i < count_n; ++i)
            {
                // Note: The even indices are inner-vertices.
                ibuf[0] = idx_inner;
                ibuf[1] = extend(idx_inner, (i - 1) * 2);
                ibuf[2] = extend(idx_inner, i * 2);
                ibuf += 3;
            }

            Vec2f norm;
            Vec4f col_trans = colors.c0123[0];
            col_trans.a = 0.f;
            for (int i = 0, i_prev = count_n - 1; i < count_n; i_prev = i++)
            {
                // Average the normals.
                norm.x = (normals[i_prev].x + normals[i].x) * .5f;
                norm.y = (normals[i_prev].y + normals[i].y) * .5f;
                invert_normals(&norm);
                float dm_x = norm.x * Constants::aa_fringe_scale * .5f;
                float dm_y = norm.y * Constants::aa_fringe_scale * .5f;

                col_trans = colors.c0123[i];
                col_trans.a = 0.f;
                // Vertices.
                vbuf[0] = { .pos = { (in.p0123[i].x - dm_x),
                                     (in.p0123[i].y - dm_y) },
                            .color = colors.c0123[i],
                            .uv = 0.f }; // Inner.
                vbuf[1] = { .pos = { (in.p0123[i].x + dm_x),
                                     (in.p0123[i].y + dm_y) },
                            .color = col_trans,
                            .uv = 0.f }; // Outer.
                vbuf += 2;

                // Indices for fringes.
                ibuf[0] = extend(idx_inner, i * 2);
                ibuf[1] = extend(idx_inner, i_prev * 2);
                ibuf[2] = extend(idx_outer, i_prev * 2);
                ibuf[3] = extend(idx_outer, i_prev * 2);
                ibuf[4] = extend(idx_outer, i * 2);
                ibuf[5] = extend(idx_inner, i * 2);
                ibuf += 6;
            }
            // Finally, bump the indices.
            bump_index(lst, vtx_count);
            // One per index.
            lst->current.std_data.idx_count = extend(lst->current.std_data.idx_count, idx_count);
        }

        void swap_shader(DrawList* lst, Render::FragShader frag)
        {
            auto current = lst->current;
            commit_current(lst);
            lst->current = current;
            lst->current.std_data.frag = frag;
            setup_draw_cmd(lst, &lst->current);
        }

        DrawListAllocator* draw_lst_alloc;
    } // namespace [anon]

    void commit_current(DrawList* lst)
    {
        if (lst->current.std_data.idx_count != Cardinality{ 0 })
        {
            push_node_draw_cmd(lst, lst->current);
        }
        setup_draw_cmd(lst, &lst->current);
    }

    void new_frame(DrawList* lst, const ScreenDimensions& screen, TimeUpdate t)
    {
        lst->current = {};
        lst->last_index = {};
        lst->vert_buf = VertexBuffer{};
        lst->idx_buf = IndexBuffer{};
        lst->cmd_buf = DrawListEntityList{};
        lst->clip_rects = DrawListEntityList{};
        lst->texture_stack = DrawListEntityList{};
        lst->texture_batch = DrawListEntityList{};
        lst->color_palettes = DrawListEntityList{};
        lst->free_entity_list = nullptr;
        lst->screen = screen;
        lst->delta_time = t.dt;
        lst->app_time = t.app_time;
        // Initialize the first clipping rect to be the screen.
        lst->current.clip_rect = ClipRect::basic(screen);
        lst->current.sort = CmdSort::Standard;
        // Clear arenas.
        Arena::clear(lst->entity_list_arena);
        Arena::clear(lst->vert_buf_arena);
        Arena::clear(lst->idx_buf_arena);
        // Seed the buffers.
        lst->vert_buf.buf = bump_vert_buf(lst, 0);
        lst->idx_buf.buf = bump_index_buf(lst, 0);
    }

    void end_frame(DrawList* lst)
    {
        // If there's an outstanding draw, commit it.
        if (lst->current.std_data.idx_count != Cardinality{ 0 })
        {
            commit_current(lst);
        }

        // Let's assert we don't have any outstanding stacks.
        assert(lst->clip_rects.count == 0);
        assert(lst->texture_stack.count == 0);
        assert(lst->color_palettes.count == 0);
    }

    void end_frame(CmdList* cmd_lst)
    {
        for EachIndex(i, count_of<DrawListLayer>)
        {
            DrawListCollection* lst = &cmd_lst->draw_list[i];
            for EachNode(n, lst->first)
            {
                end_frame(n->lst);
            }
        }
    }

    void init_alloc(DrawListAllocator* alloc)
    {
        draw_lst_alloc = alloc;
    }

    DrawList* alloc_draw_list()
    {
        DrawListAllocNode* node = nullptr;
        if (draw_lst_alloc->free_lst != nullptr)
        {
            node = draw_lst_alloc->free_lst;
            SLLStackPop(draw_lst_alloc->free_lst);
            node->next = node->prev = nullptr;
            node->lst = DrawList{};
        }
        else
        {
            node = Arena::push_array<DrawListAllocNode>(draw_lst_alloc->alloc_arena, 1);
        }
        DLLPushBack(draw_lst_alloc->alive.first, draw_lst_alloc->alive.last, node);
        ++draw_lst_alloc->alive.count;
        DrawList* lst = &node->lst;
        auto buf_arena_params = Arena::default_params;
        // We do not want these arenas to chain.  If there's breakage, let it crash and we'll deal with it later.
        buf_arena_params.flags |= Arena::Flags::NoChain;
        lst->entity_list_arena = Arena::alloc(Arena::default_params);
        lst->vert_buf_arena = Arena::alloc(buf_arena_params);
        lst->idx_buf_arena = Arena::alloc(buf_arena_params);
        return lst;
    }

    void release_draw_list(DrawList* lst)
    {
        // We need to tear this out of the alive list and place it on the dead list.
        DrawListAllocNode* found = nullptr;
        for EachNode(n, draw_lst_alloc->alive.first)
        {
            if (lst == &n->lst)
            {
                found = n;
                break;
            }
        }
        assert(found != nullptr);
        DLLRemove(draw_lst_alloc->alive.first, draw_lst_alloc->alive.last, found);
        found->next = found->prev = nullptr;
        DLLPushBack(draw_lst_alloc->dead.first, draw_lst_alloc->dead.last, found);
        --draw_lst_alloc->alive.count;
        ++draw_lst_alloc->dead.count;
    }

    ClipRect current_clip(const DrawList& lst)
    {
        return lst.current.clip_rect;
    }

    Render::BasicTexture current_texture(const DrawList& lst)
    {
        return lst.current.std_data.tex;
    }

    ScreenDimensions screen(const DrawList& lst)
    {
        return lst.screen;
    }

    Vec2f screen_vec(const DrawList& lst)
    {
        return { rep(lst.screen.width) + 0.f, rep(lst.screen.height) + 0.f };
    }

    float delta_time(const DrawList& lst)
    {
        return lst.delta_time;
    }

    float app_time(const DrawList& lst)
    {
        return lst.app_time;
    }

    const ColorPalette* current_palette(const DrawList& lst)
    {
        assert(lst.color_palettes.count != 0);
        return &top(lst.color_palettes)->color_palette;
    }

    void push_draw_list(CmdList* cmd_lst, DrawListLayer layer, DrawList* lst)
    {
        DrawListCollectionNode* node = Arena::push_array<DrawListCollectionNode>(cmd_lst->cmd_list_arena, 1);
        node->lst = lst;
        DrawListCollection* lst_c = &cmd_lst->draw_list[rep(layer)];
        SLLQueuePush(lst_c->first, lst_c->last, node);
        ++lst_c->count;
    }

    void cmd_list_consumed(CmdList* cmd_lst)
    {
        // We just clear the arena.
        zero_bytes(cmd_lst->draw_list, count_of<DrawListLayer>);
        Arena::clear(cmd_lst->cmd_list_arena);
        // Remove all staged deallocations.
        while (draw_lst_alloc->dead.first != nullptr)
        {
            DrawListAllocNode* node = draw_lst_alloc->dead.first;
            DrawList* lst = &node->lst;
            // We're just going to release the underlying arenas and put the draw list on the free list.
            // TODO: Maybe we should free some of these?  Idk...
            Arena::release(lst->entity_list_arena);
            Arena::release(lst->vert_buf_arena);
            Arena::release(lst->idx_buf_arena);
            DLLRemove(draw_lst_alloc->dead.first, draw_lst_alloc->dead.last, node);
            SLLStackPush(draw_lst_alloc->free_lst, node);
        }
        draw_lst_alloc->dead = DrawListAllocList{};
    }

    void push_clip(DrawList* lst, ClipRect clip)
    {
        // Commit previous if necessary.
        commit_current(lst);

        DrawListEntityNode* node = push_node(lst, &lst->clip_rects);
        node->clip_rect = clip;
        lst->current.clip_rect = clip;
    }

    void pop_clip(DrawList* lst)
    {
        // Commit previous if necessary.
        commit_current(lst);

        assert(lst->clip_rects.count != 0);
        pop_node(lst, &lst->clip_rects);
        if (lst->clip_rects.count == 0)
        {
            lst->current.clip_rect = ClipRect::basic(lst->screen);
        }
        else
        {
            lst->current.clip_rect = top(lst->clip_rects)->clip_rect;
        }
    }

    void push_texture(DrawList* lst, Render::BasicTexture tex)
    {
        // Commit previous if necessary.
        if (lst->current.std_data.tex != tex)
        {
            commit_current(lst);
        }

        DrawListEntityNode* node = push_node(lst, &lst->texture_stack);
        node->tex = tex;
        lst->current.std_data.tex = tex;
    }

    void pop_texture(DrawList* lst)
    {
        assert(lst->texture_stack.count != 0);
        pop_node(lst, &lst->texture_stack);
        auto tex = Render::BasicTexture::Invalid;
        if (lst->texture_stack.count != 0)
        {
            tex = top(lst->texture_stack)->tex;
        }

        // Commit previous if necessary.
        if (tex != lst->current.std_data.tex)
        {
            commit_current(lst);
        }
        lst->current.std_data.tex = tex;
    }

    void push_color_palette(DrawList* lst, const ColorPalette& palette)
    {
        DrawListEntityNode* node = push_node(lst, &lst->color_palettes);
        node->color_palette = palette;
    }

    void pop_color_palette(DrawList* lst)
    {
        assert(lst->color_palettes.count != 0);
        pop_node(lst, &lst->color_palettes);
    }

    bool pos_outside_clip(DrawList* lst, Vec2f pos)
    {
        return pos.x > rep(lst->current.clip_rect.width);
    }

    bool box_outside_clip(DrawList* lst, const Vec4f& box)
    {
        auto p_b = UI::unadjusted_clip_as_vec(lst->current.clip_rect);
        return not UI::boxed_aabb(box, p_b);
    }

    void start_glyph_run(DrawList* lst, Render::VertShader vert)
    {
        Render::FragShader shader[] =
        {
            Render::FragShader::Text,
            Render::FragShader::TextSubpixel,
        };

        Render::BlendingMode blend[] =
        {
            Render::BlendingMode::Default,
            Render::BlendingMode::DualSourceBlend,
        };

        const bool subpix = Config::system_effects().subpixel_font_aa;
        DrawCmd draw{
            .sort = CmdSort::Standard,
            .clip_rect = lst->current.clip_rect,
            .std_data {
                .tex = lst->current.std_data.tex,
                .blend = blend[subpix],
                .vert = vert,
                .frag = shader[subpix],
            }
        };

        // Riff off the previous run.
        if (header_match(lst->current, draw))
            return;

        commit_current(lst);
        lst->current = draw;
        setup_draw_cmd(lst, &lst->current);
    }

    void start_images(DrawList* lst, Render::VertShader vert)
    {
        DrawCmd draw{
            .sort = CmdSort::Standard,
            .clip_rect = lst->current.clip_rect,
            .std_data {
                .tex = lst->current.std_data.tex,
                .blend = Render::BlendingMode::Default,
                .vert = vert,
                .frag = Render::FragShader::BasicColor,
            }
        };

        // Riff off the previous run.
        if (header_match(lst->current, draw))
            return;

        commit_current(lst);
        lst->current = draw;
        setup_draw_cmd(lst, &lst->current);
    }

    void start_icon_image_batch(DrawList* lst, Render::VertShader vert)
    {
        DrawCmd draw{
            .sort = CmdSort::Standard,
            .clip_rect = lst->current.clip_rect,
            .std_data {
                .tex = lst->current.std_data.tex,
                .blend = Render::BlendingMode::Default,
                .vert = vert,
                .frag = Render::FragShader::BasicColor,
            }
        };

        // Riff off the previous run.
        if (header_match(lst->current, draw))
            return;

        commit_current(lst);
        lst->current = draw;
        setup_draw_cmd(lst, &lst->current);

        // Clear the batch list.
        while (lst->texture_batch.count != 0)
        {
            pop_node(lst, &lst->texture_batch);
        }
    }

    void complete_icon_image_batch(DrawList* lst, Render::FragShader frag)
    {
        // No work to do.
        if (lst->texture_batch.count == 0)
            return;
        // Our process is:
        // 1. Save current header.
        // 2. Sort by texture.
        // 3. On each texture change, commit result.
        // 4. Reset current back to original.
        auto saved_current = lst->current;

        // Setup header.
        lst->current.std_data.frag = frag;

        // Let's set up an array we can easily sort.
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        BatchedTexture** batches = Arena::push_array<BatchedTexture*>(scratch.arena, lst->texture_batch.count);
        // Copy over.
        uint64_t idx = 0;
        for EachNode(n, lst->texture_batch.first)
        {
            batches[idx++] = &n->batch_tex;
        }
        BatchedTexture** batches_last = batches + lst->texture_batch.count;

        std::sort(batches, batches_last,
                    [](const BatchedTexture* a, const BatchedTexture* b)
                    {
                        return a->tex < b->tex;
                    });
        // Get the first entry and pivot.
        auto current_tex = (batches[0])->tex;
        // If the standard data texture does not match the one we want, we need to commit.
        if (lst->current.std_data.tex != current_tex)
        {
            commit_current(lst);
        }
        lst->current.std_data.tex = current_tex;
        for (idx = 0; idx < lst->texture_batch.count; ++idx)
        {
            BatchedTexture* tex = batches[idx];
            if (current_tex != tex->tex)
            {
                commit_current(lst);
                current_tex = tex->tex;
                lst->current.std_data.tex = tex->tex;
            }

            render_icon_image(lst, frag, tex->pos, tex->size, tex->uv_start, tex->uv_end, tex->color);
        }
        // Commit any outstanding.
        commit_current(lst);

        // Terminate the scratch arena.
        Arena::scratch_end(scratch);

        // Copy out the buffer indices.
        saved_current.std_data.vert_off = lst->current.std_data.vert_off;
        saved_current.std_data.idx_off = lst->current.std_data.idx_off;
        saved_current.std_data.idx_count = lst->current.std_data.idx_count;
        lst->current = saved_current;
    }

    // Shapes.
    void start_shapes(DrawList* lst, Render::VertShader vert)
    {
        DrawCmd draw{
            .sort = CmdSort::Standard,
            .clip_rect = lst->current.clip_rect,
            .std_data = {
                .tex = lst->current.std_data.tex,
                .blend = Render::BlendingMode::Default,
                .vert = vert,
                .frag = Render::FragShader::BasicColor,
            }
        };

        // Riff off the previous run.
        if (header_match(lst->current, draw))
            return;

        commit_current(lst);
        lst->current = draw;
        setup_draw_cmd(lst, &lst->current);
    }

    void solid_rect(DrawList* lst, Render::FragShader frag, const Vec2f& top_left, const Vec2f& size, const Vec4f& color)
    {
        if (lst->current.std_data.frag != frag)
        {
            swap_shader(lst, frag);
        }
        render_quad(lst, top_left, top_left + size, color, 0.f, 0.f, no_vert_flags);
    }

    void multi_color_solid_rect(DrawList* lst, Render::FragShader frag, const Vec2f& top_left, const Vec2f& size, const MultiColorInput& colors)
    {
        if (lst->current.std_data.frag != frag)
        {
            swap_shader(lst, frag);
        }
        render_multi_color_quad(lst, top_left, top_left + size, colors, 0.f, 0.f);
    }

    void solid_rect_uv_size(DrawList* lst, Render::FragShader frag, const Vec2f& top_left, const Vec2f& size, const Vec4f& color)
    {
        if (lst->current.std_data.frag != frag)
        {
            swap_shader(lst, frag);
        }
        constexpr Vec2f uv_pos{-1.f, 1.f};
        constexpr Vec2f uv_size{ 2.f, -2.f };
        render_quad(lst, top_left, top_left + size, color, uv_pos, uv_pos + uv_size, no_vert_flags);
    }

    void strike_rect(DrawList* lst, Render::FragShader frag, const Vec2f& top_left, const Vec2f& size, float thickness, const Vec4f& color)
    {
        if (lst->current.std_data.frag != frag)
        {
            swap_shader(lst, frag);
        }
        auto strike_pos = top_left;
        auto strike_size = size;
        //      A
        //   ----------
        //   |        |
        // D |        | B
        //   |        |
        //   ----------
        //     C
        //
        // A
        strike_size.y = thickness;
        solid_rect(lst, frag, strike_pos, strike_size, color);
        // C
        strike_pos.y = top_left.y + size.y - thickness;
        solid_rect(lst, frag, strike_pos, strike_size, color);
        // D
        strike_pos.y = top_left.y + thickness;
        strike_size.y = size.y - thickness * 2.f;
        strike_size.x = thickness;
        solid_rect(lst, frag, strike_pos, strike_size, color);
        // B
        strike_pos.x = top_left.x + size.x - thickness;
        solid_rect(lst, frag, strike_pos, strike_size, color);
    }

    void line(DrawList* lst, Render::FragShader frag, const Vec2f& a, const Vec2f& b, float thickness, const Vec4f& color)
    {
        if (lst->current.std_data.frag != frag)
        {
            swap_shader(lst, frag);
        }
        Vec2f quad_x = { a.x, b.x };
        Vec2f quad_y = { a.y, b.y };
        Vec2f norm;
        const bool r_to_l_x = quad_x.xy[0] < quad_x.xy[1];
        // Find the unit vector for the distance.
        {
            float dx = quad_x.xy[r_to_l_x] - quad_x.xy[not r_to_l_x];
            float dy = quad_y.xy[r_to_l_x] - quad_y.xy[not r_to_l_x];
            float n = dx * dx + dy * dy;
            if (n > 0.f)
            {
                n = 1.f / sqrtf(n);
                // Get perpendicular.
                norm.x = -dy * n;
                norm.y = dx * n;
            }
            norm.x *= thickness;
            norm.y *= thickness;
        }
        // Render the quad in order:
        // 0 - 1  |  a - b
        // | \ |  |  | \ |
        // 3 - 2  |  d - c
        CmdBuffer::QuadInput in = {
            .p0123 = {
                { quad_x.xy[not r_to_l_x] + norm.x, quad_y.xy[not r_to_l_x] + norm.y },
                { quad_x.xy[r_to_l_x]     + norm.x, quad_y.xy[r_to_l_x]     + norm.y },
                { quad_x.xy[r_to_l_x],              quad_y.xy[r_to_l_x] },
                { quad_x.xy[not r_to_l_x],          quad_y.xy[not r_to_l_x] }
            }
        };
        QuadColors col_in = {};
        col_in.c0123[0] = color;
        col_in.c0123[1] = color;
        col_in.c0123[2] = color;
        col_in.c0123[3] = color;
        apply_anti_alias(lst, in, col_in, 0.f);
    }

    void quad(DrawList* lst, Render::FragShader frag, const QuadInput& in, const Vec4f& color)
    {
        if (lst->current.std_data.frag != frag)
        {
            swap_shader(lst, frag);
        }
        QuadColors col_in = {};
        col_in.c0123[0] = color;
        col_in.c0123[1] = color;
        col_in.c0123[2] = color;
        col_in.c0123[3] = color;
        apply_anti_alias(lst, in, col_in, 2.f);
    }

    void multi_color_quad(DrawList* lst, Render::FragShader frag, const QuadInput& in, const QuadColors& colors)
    {
        if (lst->current.std_data.frag != frag)
        {
            swap_shader(lst, frag);
        }
        apply_anti_alias(lst, in, colors, 2.f);
    }

    void quad_image(DrawList* lst, Render::FragShader frag, const QuadInput& in, const Vec2f& uv_pos, const Vec2f& uv_size, const Vec4f& color)
    {
        if (lst->current.std_data.frag != frag)
        {
            swap_shader(lst, frag);
        }
        render_quad(lst, in, color, uv_pos, uv_pos + uv_size, no_vert_flags);
    }

    // General rendering.
    void render_icon_image(DrawList* lst, Render::FragShader frag, const Vec2f& pos, const Vec2f& size, const Vec2f& uv_pos, const Vec2f& uv_size, const Vec4f& color)
    {
        if (lst->current.std_data.frag != frag)
        {
            swap_shader(lst, frag);
        }
        VertFlags flgs = {
            .take_texel = 1.f,
            .blend_black = 1.f,
        };
        render_quad(lst, pos, pos + size, color, uv_pos, uv_pos + uv_size, flgs);
    }

    void render_image(DrawList* lst, Render::FragShader frag, const Vec2f& pos, const Vec2f& size, const Vec2f& uv_pos, const Vec2f& uv_size, const Vec4f& color)
    {
        if (lst->current.std_data.frag != frag)
        {
            swap_shader(lst, frag);
        }
        VertFlags flgs = {
            .take_texel = 1.f
        };
        render_quad(lst, pos, pos + size, color, uv_pos, uv_pos + uv_size, flgs);
    }

    void render_glyph(DrawList* lst, Render::FragShader frag, const Vec2f& pos, const Vec2f& size, const Vec2f& uv_pos, const Vec2f& uv_size, const Vec4f& color)
    {
        if (lst->current.std_data.frag != frag)
        {
            swap_shader(lst, frag);
        }
        render_quad(lst, pos, pos + size, color, uv_pos, uv_pos + uv_size, no_vert_flags);
    }

    void render_subpixel_glyph(DrawList* lst, Render::FragShader frag, const Vec2f& pos, const Vec2f& size, const Vec2f& uv_pos, const Vec2f& uv_size, const Vec4f& color, float px_offset)
    {
        if (lst->current.std_data.frag != frag)
        {
            swap_shader(lst, frag);
        }
        render_quad_subpixel_adjust(lst, pos, pos + size, color, uv_pos, uv_pos + uv_size, px_offset);
    }

    void batch_image(DrawList* lst, Render::BasicTexture tex, const Vec2f& pos, const Vec2f& size, const Vec2f& uv_pos, const Vec2f& uv_size, const Vec4f& color)
    {
        BatchedTexture batch{
            .tex = tex,
            .pos = pos,
            .size = size,
            .uv_start = uv_pos,
            .uv_end = uv_size,
            .color = color,
        };
        DrawListEntityNode* node = push_node(lst, &lst->texture_batch);
        node->batch_tex = batch;
    }

    void background_color(DrawList* lst, const Vec4f& color)
    {
        start_shapes(lst, Render::VertShader::OneOneTransform);
        const auto clip = current_clip(*lst);
        solid_rect(lst, Render::FragShader::BasicColor, 0.f, { rep(clip.width) + 0.f, rep(clip.height) + 0.f }, color);
    }

    // Effects rendering.
    void standard_window_blur(DrawList* lst, ClipRect clip)
    {
        commit_current(lst);
        // Just insert a new unique draw command.
        DrawCmd draw {
            .sort = CmdSort::Blur,
            .clip_rect = clip,
        };
        push_node_draw_cmd(lst, draw);
    }

    // Shader manipulation.
    void push_camera(DrawList* lst, const Render::Camera& camera)
    {
        commit_current(lst);
        // Just insert a new unique draw command.
        DrawCmd draw {
            .sort = CmdSort::CameraUpdate,
            .clip_rect = {},
            .camera_up = camera
        };
        push_node_draw_cmd(lst, draw);
    }

    void push_resolution(DrawList* lst, const ScreenDimensions& res)
    {
        commit_current(lst);
        // Just insert a new unique draw command.
        DrawCmd draw {
            .sort = CmdSort::ResolutionUpdate,
            .clip_rect = {},
            .res_up = res
        };
        push_node_draw_cmd(lst, draw);
    }
} // namespace CmdBuffer