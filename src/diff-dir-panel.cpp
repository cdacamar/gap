#include "diff-dir-panel.h"

#include <cassert>

#include "basic-button.h"
#include "basic-window.h"
#include "concurrent-queue.h"
#include "diff-dir-list.h"
#include "diff-panel.h"
#include "diff-text.h"
#include "gap-core.h"
#include "os.h"
#include "thread.h"
#include "timers.h"
#include "tooltips.h"

namespace Diff
{
    namespace
    {
        struct DiffDirPanelUIData
        {
            float wheel_offset_amount;
        };

        struct PartitionDirPanel
        {
            static constexpr float padding = 2.f;

            PartitionDirPanel* sib_next;
            PartitionDirPanel* sib_prev;
            CmdBuffer::DrawList* draw_lst;
            Arena::Arena* text_file_arena;
            UI::Widgets::ID id;
            DiffDirListView* view;
            float pct_of_parent;
            float ease_offset;
        };

        read_only PartitionDirPanel null_dir_panel_inst = {
            .sib_next = &null_dir_panel_inst,
            .sib_prev = &null_dir_panel_inst,
        };

        struct DiffDirDiffCache
        {
            // These arrays are the same size as our merged files lists.
            // One for A and one for B.
            MergedDiffView* file_line_diffs[2];
            MergedTextBlocks* file_text_block_diffs[2];
            uint64_t size;
        };

        struct DiffDirThreadChunk
        {
            Arena::Arena* arena;
            Thread::ConcurrentQueue* ccq;
            DiffCount* diff_counts_slice;
            MergedDiffView* diff_lines_slice[2];
            MergedTextBlocks* diff_text_slice[2];
            TextFile* files_slice_A;
            TextFile* files_slice_B;
            uint64_t slice_count;
            uint64_t largest_ins; // Atomically updated/read.
            uint64_t largest_del; // Atomically updated/read.
            bool word_based_diff;
        };

        enum class DiffDirThreadState : uint32_t
        {
            Computed,
            Computing
        };

        struct DiffDirFiles
        {
            TextFile* files;
            uint64_t size;
        };

        struct DiffDirThreadData
        {
            DiffDirThreadData* next;
            DiffDirThreadData* prev;
            Arena::Arena** arenas;
            Thread::ConcurrentQueue* queues;     // These provide indices into computed_diff_counts.
            DiffDirThreadChunk* diff_chunks;
            DiffCountArray computed_diff_counts; // Completion markers.
            DiffDirDiffCache diff_cache;
            uint64_t size;
            DiffDirFiles files_A;
            DiffDirFiles files_B;
            Thread::TaskHandle* async_tasks;
            Timers::Stopwatch sw;
            DiffDirThreadState state;
        };

        struct DiffDirThreadDataList
        {
            DiffDirThreadData* first;
            DiffDirThreadData* last;
            uint64_t count;
        };

        PartitionDirPanel* null_dir_panel()
        {
            return &null_dir_panel_inst;
        }

        bool null_dir_panel(PartitionDirPanel* panel)
        {
            return panel == &null_dir_panel_inst;
        }

        CmdBuffer::ClipRect clip_from_parent(CmdBuffer::ClipRect parent_clip, PartitionDirPanel* first, PartitionDirPanel* target)
        {
            Vec4f clip = UI::clip_as_vec(parent_clip);
            Vec2f parent_size{ rep(parent_clip.width) + 0.f, rep(parent_clip.height) + 0.f };
            // Make the width the same as the start offset (so we can sum widths based on %).
            clip.p1[0] = clip.p0[0];
            // Note: We only layout on one axis so this loop is simplified.
            for (;not null_dir_panel(first); first = first->sib_next)
            {
                clip.p1[0] += parent_size.xy[0] * first->pct_of_parent;
                if (first == target)
                    break;
                clip.p0[0] = clip.p1[0];
            }
            return UI::vec_as_clip(clip);
        }

        void init_dir_panel(PartitionDirPanel* panel, UI::Widgets::ID seed_id, uint32_t seed_idx, Glyph::Atlas* atlas)
        {
            panel->id = UI::Widgets::make_id_seed_idx(seed_id, seed_idx);
            panel->draw_lst = CmdBuffer::alloc_draw_list();
            panel->text_file_arena = Arena::alloc(Arena::default_params);
            panel->ease_offset = 1.f;
            panel->pct_of_parent = .5f;
            panel->sib_next = panel->sib_prev = null_dir_panel();
            // Allocate the diff dir view.
            panel->view = make_diff_dir_list_view(atlas, UI::Widgets::make_id_seed(panel->id, "dir_view"));
        }

        Render::RenderViewport initial_window_viewport(const ScreenDimensions& screen)
        {
            auto viewport = Render::RenderViewport::basic(screen);
            Vec2f center = UI::center_clip(UI::convert(viewport));
            viewport.width = Width{ static_cast<int>(rep(viewport.width) * 0.75) };
            viewport.height = Height{ static_cast<int>(rep(viewport.height) * 0.75) };
            viewport.offset_x = Render::ViewportOffsetX{ static_cast<int>(center.x - rep(viewport.width) / 2) };
            viewport.offset_y = Render::ViewportOffsetY{ static_cast<int>(center.y - rep(viewport.height) / 2) };
            return viewport;
        }

        void insert_flat_dir_entry_sorted(Arena::Arena* arena, FlatDirEntryList* lst, OS::DirIterResult item)
        {
            FlatDirEntry* e = Arena::push_array<FlatDirEntry>(arena, 1);
            e->item = item;
            e->item.path = str8_copy(arena, e->item.path);
            // Find the insert point.
            FlatDirEntry* prev = nullptr;
            uint64_t node_depth = depth_of_file(e->item.path);
            uint64_t n_depth = 0;
            bool inserted = false;
            for EachNode(n, lst->first)
            {
                n_depth = depth_of_file(n->item.path);
                if (node_depth < n_depth
                    or (node_depth == n_depth and str8_compare(e->item.path, n->item.path) < 0))
                {
                    DLLInsert(lst->first, lst->last, prev, e);
                    inserted = true;
                    break;
                }
                prev = n;
            }

            if (not inserted)
            {
                DLLPushBack(lst->first, lst->last, e);
            }
            ++lst->count;
        }

        void populate_flattend_dir_entries(Arena::Arena* arena, FlatDirEntryList* lst, String8 path, Feed::MessageFeed* feed)
        {
            if (not OS::directory_exists(path))
            {
                String8 msg = str8_fmt(arena, "Dropped path '%S' is not a directory.", path);
                feed->queue_error(msg);
                return;
            }
            auto scratch = Arena::scratch_begin({ &arena, 1 });
            // First, we need to canonicalize the path.
            String8 canon_path = str8_empty;
            // Just allocate to output arena for list.
            OS::Error err = OS::canonical_file_path(arena, &canon_path, path);
            bool good = true;
            if (err != OS::Error::None)
            {
                String8 msg = str8_fmt(scratch.arena, "Unable to resolve path '%S': %S", path, OS::error_text(err));
                feed->queue_error(msg);
                good = false;
            }

            if (good)
            {
                lst->base_dir = canon_path;
                OS::DirIterResult item;
                // Create a queue for recursive directories.
                String8Node* head = Arena::push_array<String8Node>(arena, 1);
                String8Node* free_lst = nullptr;
                head->string = canon_path;
                do
                {
                    String8 dir = head->string;
                    {
                        String8Node* n = head;
                        SLLStackPop(head);
                        SLLStackPush(free_lst, n);
                    }
                    OS::DirIter os_itr = OS::open_dir_iter(dir, OS::DirIterFlags::None);
                    // Do we have a good iterator?
                    if (os_itr != OS::DirIter::Sentinel)
                    {
                        do
                        {
                            if (not OS::dir_iter_next(scratch.arena, &item, os_itr))
                                break;
                            if (implies(item.props.props, OS::FileProperty::Directory))
                            {
                                String8Node* next_dir = free_lst;
                                if (next_dir != nullptr)
                                {
                                    SLLStackPop(free_lst);
                                    zero_bytes(next_dir);
                                }
                                else
                                {
                                    next_dir = Arena::push_array<String8Node>(scratch.arena, 1);
                                }
                                // Note: This string lives in the scratch arena with our stack nodes.
                                next_dir->string = OS::combine_paths(scratch.arena, dir, item.path);
                                SLLStackPush(head, next_dir);
                            }
                            // Otherwise, regular file.
                            else
                            {
                                // Fake the relative path by providing the dir but chopping the base_dir.
                                String8 rel_dir = str8_chop_prefix(dir, canon_path);
                                item.path = OS::combine_paths(scratch.arena, rel_dir, item.path);
                                insert_flat_dir_entry_sorted(arena, lst, item);
                            }
                        } while (true);
                        OS::close_dir_iter(os_itr);
                    }
                } while (head != nullptr);
            }
            Arena::scratch_end(scratch);
        }
    } // namespace [anon]

    struct DiffDirPanel
    {
        Arena::Arena* arena;
        Arena::Arena* text_diffs_arena;
        Glyph::Atlas* atlas;
        CmdBuffer::DrawList* frame_lst;
        UI::Widgets::ID id;
        UI::Widgets::BasicWindow* window;
        PartitionDirPanel A;
        PartitionDirPanel B;
        DiffDirDiffCache diff_cache;
        SelectedDiffFile selected_file;
        DiffDirThreadData* thread_data;
        DiffDirThreadData* free_thread_lst;
        DiffDirThreadDataList thread_data_lst;
        DiffDirThreadDataList cancelled_thread_data_lst;
        DiffDirPanelUIData ui_data;
        bool word_based_diff;
    };

    namespace
    {
        DiffDirThreadData* diff_dir_panel_make_thread_data(DiffDirPanel* panel)
        {
            DiffDirThreadData* result = panel->free_thread_lst;
            if (result != nullptr)
            {
                SLLStackPop(panel->free_thread_lst);
                for EachIndex(i, result->size)
                {
                    Arena::clear(result->arenas[i]);
                }
                // Reset state.
                result->state = DiffDirThreadState::Computed;
                result->next = result->prev = nullptr;
            }
            else
            {
                result = Arena::push_array<DiffDirThreadData>(panel->arena, 1);
                // Allocate pools based on thread concurrency.
                Thread::ThreadPool* pool = Thread::system_thread_pool();
                result->size = pool->thread_count();
                Arena::Arena* first_arena = Arena::alloc(Arena::default_params);
                result->arenas = Arena::push_array<Arena::Arena*>(first_arena, result->size);
                result->arenas[0] = first_arena;
                for (uint64_t i = 1; i < result->size; ++i)
                {
                    result->arenas[i] = Arena::alloc(Arena::default_params);
                }
            }
            DLLPushBack(panel->thread_data_lst.first, panel->thread_data_lst.last, result);
            ++panel->thread_data_lst.count;
            return result;
        }

        void diff_dir_panel_release_thread_data(DiffDirPanel* panel, DiffDirThreadDataList* lst, DiffDirThreadData* td)
        {
            DLLRemove(lst->first, lst->last, td);
            --lst->count;
            SLLStackPush(panel->free_thread_lst, td);
        }

        void diff_dir_panel_cancel_thread_data(DiffDirPanel* panel, DiffDirThreadData* td)
        {
            // Kill all working threads.
            Thread::ThreadPool* pool = Thread::system_thread_pool();
            for EachIndex(i, td->size)
            {
                Thread::TaskHandle handle = td->async_tasks[i];
                if (handle != Thread::TaskHandle::Sentinel)
                {
                    pool->cancel_task(handle);
                }
            }
            // Remove from active list and place on cancelled list.
            DLLRemove(panel->thread_data_lst.first, panel->thread_data_lst.last, td);
            --panel->thread_data_lst.count;
            DLLPushBack(panel->cancelled_thread_data_lst.first, panel->cancelled_thread_data_lst.last, td);
            ++panel->cancelled_thread_data_lst.count;
        }

        void diff_dir_panel_populate_thread_data_text_file_paths(Arena::Arena* arena, DiffDirPanel* panel, DiffDirThreadData* td)
        {
            MergedFileArray merged_files_A = diff_dir_list_view_merged_file_array(panel->A.view);
            MergedFileArray merged_files_B = diff_dir_list_view_merged_file_array(panel->B.view);
            td->files_A.size = merged_files_A.size;
            td->files_B.size = merged_files_B.size;
            td->files_A.files = Arena::push_array<TextFile>(arena, td->files_A.size);
            td->files_B.files = Arena::push_array<TextFile>(arena, td->files_B.size);
            assert(td->files_A.size == td->files_B.size);
            for EachIndex(i, td->files_A.size)
            {
                td->files_A.files[i] = merged_files_A.array[i].file;
                td->files_B.files[i] = merged_files_B.array[i].file;
                // Copy the paths to our arena.
                td->files_A.files[i].path = str8_copy(arena, td->files_A.files[i].path);
                td->files_B.files[i].path = str8_copy(arena, td->files_B.files[i].path);
            }
        }

        void diff_dir_panel_thread_work(Thread::ThreadWorkData* td)
        {
            DiffDirThreadChunk* td_chunk = static_cast<DiffDirThreadChunk*>(td->data);
            auto scratch = Arena::scratch_begin({ &td_chunk->arena, 1 });
            for EachIndex(i, td_chunk->slice_count)
            {
                auto tmp = Arena::temp_begin(scratch.arena);
                TextFile* file_A = &td_chunk->files_slice_A[i];
                TextFile* file_B = &td_chunk->files_slice_B[i];
                if (file_A->path.size != 0)
                {
                    *file_A = text_file_read(td_chunk->arena, file_A->path);
                }

                if (file_B->path.size != 0)
                {
                    *file_B = text_file_read(td_chunk->arena, file_B->path);
                }

                DiffFileForViewInput in = {
                    .A = file_A,
                    .B = file_B,
                    .word_based_diff = td_chunk->word_based_diff,
                };
                DiffFileForViewResult result = diff_panel_diff_files_for_view(tmp.arena, in);
                // Cache it.
                td_chunk->diff_lines_slice[0][i] = diff_text_view_join_merged_line_list(td_chunk->arena, result.lst_A);
                td_chunk->diff_lines_slice[1][i] = diff_text_view_join_merged_line_list(td_chunk->arena, result.lst_B);
                td_chunk->diff_text_slice[0][i] = diff_text_view_join_merged_text_blocks_list(td_chunk->arena, result.merged_txt_A, *file_A);
                td_chunk->diff_text_slice[1][i] = diff_text_view_join_merged_text_blocks_list(td_chunk->arena, result.merged_txt_B, *file_B);
                // Sum diffs.
                for EachIndex(j, td_chunk->diff_lines_slice[0][i].size)
                {
                    MergedLine* line_A = &td_chunk->diff_lines_slice[0][i].lines[j];
                    MergedLine* line_B = &td_chunk->diff_lines_slice[1][i].lines[j];
                    td_chunk->diff_counts_slice[i].ins += line_B->type == EditType::Ins;
                    td_chunk->diff_counts_slice[i].del += line_A->type == EditType::Del;
                }
                // Emit maximums.
                // Note: The read does not need to be atomic on this sice (since we're the only writer)
                // but the write-back does.
                uint64_t largest_ins = td_chunk->largest_ins;
                uint64_t largest_del = td_chunk->largest_del;
                largest_ins = std::max(largest_ins, td_chunk->diff_counts_slice[i].ins);
                largest_del = std::max(largest_del, td_chunk->diff_counts_slice[i].del);
                // Atomic time!
                os_atomic_u64_eval_assign(&td_chunk->largest_ins, largest_ins);
                os_atomic_u64_eval_assign(&td_chunk->largest_del, largest_del);
                // Emit to queue.
                assert(not Thread::ccq_prod_full(td_chunk->ccq));
                assert(not Thread::ccq_cons_full(td_chunk->ccq));
                // Note: The index returned here is immaterial.  All we care about is that we move the
                // producer side forward.
                Thread::ccq_prod_push(td_chunk->ccq);
                Thread::ccq_prod_commit_push(td_chunk->ccq);

                Arena::temp_end(tmp);
                if (os_atomic_u32_eval(&td->cancellation_flag) != 0)
                    break;
            }
            Arena::scratch_end(scratch);
        }

        void diff_dir_panel_diff_files_threaded(DiffDirPanel* panel)
        {
            if (panel->thread_data == nullptr)
            {
                panel->thread_data = diff_dir_panel_make_thread_data(panel);
            }

            if (panel->thread_data->state != DiffDirThreadState::Computed)
            {
                // Cancel existing work and open a new one.
                diff_dir_panel_cancel_thread_data(panel, panel->thread_data);
                panel->thread_data = diff_dir_panel_make_thread_data(panel);
            }

            DiffDirThreadData* td = panel->thread_data;

            // Begin our timer.
            td->sw.start();

            // Clear old text file caches.
            Arena::Arena* arena = td->arenas[0];
            td->files_A = {};
            td->files_B = {};

            // Reset all arenas.
            for EachIndex(i, td->size)
            {
                Arena::clear(td->arenas[i]);
                // Reclaim commits.
                Arena::shrink_cmt_to_pos(td->arenas[i]);
            }

            // Populate text files.
            diff_dir_panel_populate_thread_data_text_file_paths(arena, panel, td);

            // Create diff cache.
            td->diff_cache = {};
            Arena::clear(arena);
            td->diff_cache.size = td->files_A.size;
            td->diff_cache.file_line_diffs[0] = Arena::push_array<MergedDiffView>(arena, td->diff_cache.size);
            td->diff_cache.file_line_diffs[1] = Arena::push_array<MergedDiffView>(arena, td->diff_cache.size);
            td->diff_cache.file_text_block_diffs[0] = Arena::push_array<MergedTextBlocks>(arena, td->diff_cache.size);
            td->diff_cache.file_text_block_diffs[1] = Arena::push_array<MergedTextBlocks>(arena, td->diff_cache.size);

            // Set up chunks.
            uint64_t diff_chunk_size = td->files_A.size / td->size;

            // Note: This must account for leftovers.
            uint64_t mod_val = std::max(diff_chunk_size, td->size);
            uint64_t diff_chunk_leftover = td->files_A.size % mod_val;

            // Create output queues.
            uint64_t thread_ccq_size = diff_chunk_size + diff_chunk_leftover;
            td->queues = Arena::push_array<Thread::ConcurrentQueue>(arena, td->size);
            td->computed_diff_counts.size = td->files_A.size;
            td->computed_diff_counts.array = Arena::push_array<DiffCount>(arena, td->computed_diff_counts.size);
            for EachIndex(i, td->size)
            {
                td->queues[i] = Thread::make_concurrent_queue(static_cast<uint32_t>(thread_ccq_size));
            }
            // Note: We actually don't care about the diff count state.  That property is marked as we complete them.
            td->diff_chunks = Arena::push_array<DiffDirThreadChunk>(arena, td->size);
            for EachIndex(i, td->size)
            {
                td->diff_chunks[i].arena = td->arenas[i];
                td->diff_chunks[i].ccq = &td->queues[i];
                td->diff_chunks[i].diff_counts_slice = td->computed_diff_counts.array + i * diff_chunk_size;
                td->diff_chunks[i].diff_lines_slice[0] = td->diff_cache.file_line_diffs[0] + i * diff_chunk_size;
                td->diff_chunks[i].diff_lines_slice[1] = td->diff_cache.file_line_diffs[1] + i * diff_chunk_size;
                td->diff_chunks[i].diff_text_slice[0] = td->diff_cache.file_text_block_diffs[0] + i * diff_chunk_size;
                td->diff_chunks[i].diff_text_slice[1] = td->diff_cache.file_text_block_diffs[1] + i * diff_chunk_size;
                td->diff_chunks[i].files_slice_A = td->files_A.files + i * diff_chunk_size;
                td->diff_chunks[i].files_slice_B = td->files_B.files + i * diff_chunk_size;
                td->diff_chunks[i].slice_count = diff_chunk_size;
                td->diff_chunks[i].largest_ins = 0;
                td->diff_chunks[i].largest_del = 0;
            }
            // Add leftovers.
            td->diff_chunks[td->size - 1].slice_count += diff_chunk_leftover;

            // Allocate thread handles.
            Thread::ThreadPool* pool = Thread::system_thread_pool();
            td->async_tasks = Arena::push_array<Thread::TaskHandle>(arena, td->size);
            Thread::ThreadWorkFn work_fn = diff_dir_panel_thread_work;
            for EachIndex(i, td->size)
            {
                void* thread_work = &td->diff_chunks[i];
                td->async_tasks[i] = pool->background_task(thread_work, work_fn);
            }
            td->state = DiffDirThreadState::Computing;
        }
    } // namespace [anon]

    // Creation.
    DiffDirPanel* make_diff_dir_panel(Glyph::Atlas* atlas)
    {
        Arena::Arena* arena = Arena::alloc(Arena::default_params);
        DiffDirPanel* panel = Arena::push_array<DiffDirPanel>(arena, 1);
        panel->arena = arena;
        panel->text_diffs_arena = Arena::alloc(Arena::default_params);
        panel->atlas = atlas;
        panel->frame_lst = CmdBuffer::alloc_draw_list();
        panel->id = UI::Widgets::ID::DiffDirPanel;
        panel->selected_file = SelectedDiffFile::Sentinel;
        {
            uint8_t* blob = Arena::push_array_no_zero_aligned<uint8_t>(arena,
                                                                        sizeof(UI::Widgets::BasicWindow),
                                                                        Arena::Alignment{ alignof(UI::Widgets::BasicWindow) });
            panel->window = new(blob) UI::Widgets::BasicWindow{ panel->id };
            panel->window->title("Diff Directories");
        }
        init_dir_panel(&panel->A, panel->id, 0, atlas);
        init_dir_panel(&panel->B, panel->id, 1, atlas);
        // Connect A and B.
        panel->A.sib_next = &panel->B;
        panel->B.sib_prev = &panel->A;
        return panel;
    }

    // Cleanup.
    void release_diff_dir_panel(DiffDirPanel* panel)
    {
        using Wind = UI::Widgets::BasicWindow;
        panel->window->~Wind();

        for (PartitionDirPanel* child = &panel->A;
            not null_dir_panel(child);
            child = child->sib_next)
        {
            release_diff_dir_list_view(child->view);
        }
        CmdBuffer::release_draw_list(panel->frame_lst);
        CmdBuffer::release_draw_list(panel->A.draw_lst);
        CmdBuffer::release_draw_list(panel->B.draw_lst);
        Arena::Arena* arena = panel->arena;
        Arena::release(arena);
    }

    // Interaction.
    void diff_dir_panel_start(DiffDirPanel* panel, const ScreenDimensions& screen, UI::UIState* state)
    {
        UI::Widgets::ShowWindowData show_data{
            .initial_viewport = initial_window_viewport(screen),
            .expand_point = { 0.5f, 0.5f }
        };
        panel->window->show(show_data);
        panel->window->background_alpha(0.8f);
        UI::set_focus_window(state, UI::Widgets::ID::ConfigExplorer);
        panel->ui_data = {};
        panel->ui_data.wheel_offset_amount = UI::standard_font_padding(Glyph::FontSize{ Config::diff_state().diff_font_size }) * 2;
    }

    void diff_dir_panel_sync_config(DiffDirPanel* panel)
    {
        // Sync the confdig with widgets.
        diff_dir_list_view_sync_config(panel->A.view);
        diff_dir_list_view_sync_config(panel->B.view);
        panel->word_based_diff = Config::diff_state().word_based_diff;
        panel->window->sync_config(panel->atlas);
    }

    void diff_dir_panel_dir_A(DiffDirPanel* panel, String8 path, Feed::MessageFeed* feed)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        FlatDirEntryList dir_entries = {};
        populate_flattend_dir_entries(scratch.arena, &dir_entries, path, feed);
        diff_dir_list_view_populate_files(panel->A.view, dir_entries);
        Arena::scratch_end(scratch);
    }

    void diff_dir_panel_dir_B(DiffDirPanel* panel, String8 path, Feed::MessageFeed* feed)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        FlatDirEntryList dir_entries = {};
        populate_flattend_dir_entries(scratch.arena, &dir_entries, path, feed);
        diff_dir_list_view_populate_files(panel->B.view, dir_entries);
        Arena::scratch_end(scratch);
    }

    void diff_dir_panel_apply_diff(DiffDirPanel* panel, Feed::MessageFeed* feed)
    {
        Timers::Stopwatch sw;
        sw.start();
        DirFileArray files_A = diff_dir_list_view_file_array(panel->A.view);
        DirFileArray files_B = diff_dir_list_view_file_array(panel->B.view);
        if (files_A.size == 0)
        {
            feed->queue_info("Please profide a directory for the left side comparison.");
            return;
        }

        if (files_B.size == 0)
        {
            feed->queue_info("Please profide a directory for the right side comparison.");
            return;
        }
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        // Reset the selection.
        panel->selected_file = SelectedDiffFile::Sentinel;
        // Clear panel text file arenas.
        Arena::clear(panel->A.text_file_arena);
        Arena::clear(panel->B.text_file_arena);
        uint64_t idx_A = 0;
        uint64_t idx_B = 0;
        // The lists to place our files.
        MergedFileList dirs_A = {};
        MergedFileList dirs_B = {};
        while (idx_A < files_A.size and idx_B < files_B.size)
        {
            OS::DirIterResult* file_A = &files_A.array[idx_A];
            OS::DirIterResult* file_B = &files_B.array[idx_B];
            // We use the same comparison function as the one in 'insert_flat_dir_entry_sorted'.
            uint64_t depth_A = depth_of_file(file_A->path);
            uint64_t depth_B = depth_of_file(file_B->path);
            int same = int(depth_A) - int(depth_B);
            if (same == 0)
            {
                same = str8_compare(file_A->path, file_B->path);
            }

            // Just insert 1-1.
            if (same == 0)
            {
                MergedFileNode* entries = Arena::push_array<MergedFileNode>(scratch.arena, 2);
                entries[0].merged.file.path = file_A->path;
                entries[1].merged.file.path = file_B->path;
                entries[0].merged.type = EditType::Eq;
                entries[1].merged.type = EditType::Eq;
                SLLQueuePush(dirs_A.first, dirs_A.last, &entries[0]);
                SLLQueuePush(dirs_B.first, dirs_B.last, &entries[1]);
                ++dirs_A.count;
                ++dirs_B.count;
                // Move both iterators forward.
                ++idx_A;
                ++idx_B;
            }
            // The file entry for A has no entry in B.  We need to create a deletion
            // in A and continue to create deletions until our comparison is no longer
            // less-than.
            else if (same < 0)
            {
                do
                {
                    MergedFileNode* entries = Arena::push_array<MergedFileNode>(scratch.arena, 2);
                    entries[0].merged.file.path = file_A->path;
                    entries[1].merged.file.path = str8_empty;
                    entries[0].merged.type = EditType::Del;
                    entries[1].merged.type = EditType::Invalid;
                    SLLQueuePush(dirs_A.first, dirs_A.last, &entries[0]);
                    SLLQueuePush(dirs_B.first, dirs_B.last, &entries[1]);
                    ++dirs_A.count;
                    ++dirs_B.count;

                    // Setup for the next file.
                    ++idx_A;
                    if (idx_A < files_A.size)
                    {
                        file_A = &files_A.array[idx_A];
                        depth_A = depth_of_file(file_A->path);
                        same = int(depth_A) - int(depth_B);
                        if (same == 0)
                        {
                            same = str8_compare(file_A->path, file_B->path);
                        }
                    }
                } while (same < 0 and idx_A < files_A.size);
            }
            // The file entry for B has no entry in A.  We need to create an insertion
            // in B and continue to create insertions until our comparison is no longer
            // greater-than.
            else
            {
                do
                {
                    MergedFileNode* entries = Arena::push_array<MergedFileNode>(scratch.arena, 2);
                    entries[0].merged.file.path = str8_empty;
                    entries[1].merged.file.path = file_B->path;
                    entries[0].merged.type = EditType::Invalid;
                    entries[1].merged.type = EditType::Ins;
                    SLLQueuePush(dirs_A.first, dirs_A.last, &entries[0]);
                    SLLQueuePush(dirs_B.first, dirs_B.last, &entries[1]);
                    ++dirs_A.count;
                    ++dirs_B.count;

                    // Setup for the next file.
                    ++idx_B;
                    if (idx_B < files_B.size)
                    {
                        file_B = &files_B.array[idx_B];
                        depth_B = depth_of_file(file_B->path);
                        same = int(depth_A) - int(depth_B);
                        if (same == 0)
                        {
                            same = str8_compare(file_A->path, file_B->path);
                        }
                    }
                } while (same > 0 and idx_B < files_B.size);
            }
        }
        // Cleanup remaining files from each bucket.
        // Deletions.
        for (;idx_A < files_A.size; ++idx_A)
        {
            OS::DirIterResult* file_A = &files_A.array[idx_A];
            MergedFileNode* entries = Arena::push_array<MergedFileNode>(scratch.arena, 2);
            entries[0].merged.file.path = file_A->path;
            entries[1].merged.file.path = str8_empty;
            entries[0].merged.type = EditType::Del;
            entries[1].merged.type = EditType::Invalid;
            SLLQueuePush(dirs_A.first, dirs_A.last, &entries[0]);
            SLLQueuePush(dirs_B.first, dirs_B.last, &entries[1]);
            ++dirs_A.count;
            ++dirs_B.count;
        }
        // Insertions.
        for (;idx_B < files_B.size; ++idx_B)
        {
            OS::DirIterResult* file_B = &files_B.array[idx_B];
            MergedFileNode* entries = Arena::push_array<MergedFileNode>(scratch.arena, 2);
            entries[0].merged.file.path = str8_empty;
            entries[1].merged.file.path = file_B->path;
            entries[0].merged.type = EditType::Invalid;
            entries[1].merged.type = EditType::Ins;
            SLLQueuePush(dirs_A.first, dirs_A.last, &entries[0]);
            SLLQueuePush(dirs_B.first, dirs_B.last, &entries[1]);
            ++dirs_A.count;
            ++dirs_B.count;
        }
        assert(dirs_A.count == dirs_B.count);
        // Populate.
        diff_dir_list_view_populate_merged_files(panel->A.view, dirs_A);
        diff_dir_list_view_populate_merged_files(panel->B.view, dirs_B);

        sw.stop();
        String8 msg = str8_fmt(scratch.arena, "File list merged in %ums", sw.to_ms());
        feed->queue_info(msg);
        Arena::scratch_end(scratch);

        // Phase 2: compute all diffs between corresponding files.
        sw.start();
        panel->diff_cache = {};
        Arena::clear(panel->text_diffs_arena);
        panel->diff_cache.size = dirs_A.count;
        panel->diff_cache.file_line_diffs[0] = Arena::push_array<MergedDiffView>(panel->text_diffs_arena, panel->diff_cache.size);
        panel->diff_cache.file_line_diffs[1] = Arena::push_array<MergedDiffView>(panel->text_diffs_arena, panel->diff_cache.size);
        panel->diff_cache.file_text_block_diffs[0] = Arena::push_array<MergedTextBlocks>(panel->text_diffs_arena, panel->diff_cache.size);
        panel->diff_cache.file_text_block_diffs[1] = Arena::push_array<MergedTextBlocks>(panel->text_diffs_arena, panel->diff_cache.size);
        MergedFileArray merged_files_A = diff_dir_list_view_merged_file_array(panel->A.view);
        MergedFileArray merged_files_B = diff_dir_list_view_merged_file_array(panel->B.view);
        scratch = Arena::scratch_begin(Arena::no_conflicts);
        // Allocate the sidebar diff counts.
        DiffCountArray diff_counts = {};
        diff_counts.size = merged_files_A.size;
        diff_counts.array = Arena::push_array<DiffCount>(scratch.arena, diff_counts.size);
        Arena::Position scratch_pop_pos = Arena::pos(scratch.arena);
        for EachIndex(i, merged_files_A.size)
        {
            // Step 1: we need to read the file and populate the text because up until this point,
            // we haven't hit the disk for file content.
            MergedFile* file_A = &merged_files_A.array[i];
            MergedFile* file_B = &merged_files_B.array[i];
            if (file_A->file.path.size != 0)
            {
                file_A->file = text_file_read(panel->A.text_file_arena, file_A->file.path);
            }

            if (file_B->file.path.size != 0)
            {
                file_B->file = text_file_read(panel->B.text_file_arena, file_B->file.path);
            }

            DiffFileForViewInput in = {
                .A = &file_A->file,
                .B = &file_B->file,
                .word_based_diff = panel->word_based_diff,
            };
            DiffFileForViewResult result = diff_panel_diff_files_for_view(scratch.arena, in);
            // Cache it.
            panel->diff_cache.file_line_diffs[0][i] = diff_text_view_join_merged_line_list(panel->text_diffs_arena, result.lst_A);
            panel->diff_cache.file_line_diffs[1][i] = diff_text_view_join_merged_line_list(panel->text_diffs_arena, result.lst_B);
            panel->diff_cache.file_text_block_diffs[0][i] = diff_text_view_join_merged_text_blocks_list(panel->text_diffs_arena, result.merged_txt_A, file_A->file);
            panel->diff_cache.file_text_block_diffs[1][i] = diff_text_view_join_merged_text_blocks_list(panel->text_diffs_arena, result.merged_txt_B, file_B->file);
            // Sum diffs.
            for EachIndex(j, panel->diff_cache.file_line_diffs[0][i].size)
            {
                MergedLine* line_A = &panel->diff_cache.file_line_diffs[0][i].lines[j];
                MergedLine* line_B = &panel->diff_cache.file_line_diffs[1][i].lines[j];
                diff_counts.array[i].ins += line_B->type == EditType::Ins;
                diff_counts.array[i].del += line_A->type == EditType::Del;
            }
            // Max diffs.
            diff_counts.largest_ins = std::max(diff_counts.largest_ins, diff_counts.array[i].ins);
            diff_counts.largest_del = std::max(diff_counts.largest_del, diff_counts.array[i].del);
            // Clear scratch so we can start again at the beginning of the loop.
            Arena::pop_to(scratch.arena, scratch_pop_pos);
        }
        // Populate the diff counts, but only to A.
        diff_dir_list_view_apply_diff_count_sidebar(panel->A.view, diff_counts);

        sw.stop();
        msg = str8_fmt(scratch.arena, "File diffs computed in %ums", sw.to_ms());
        feed->queue_info(msg);
        Arena::scratch_end(scratch);
    }

    void diff_dir_panel_try_dir_drop(DiffDirPanel* panel, String8 path, UI::UIState* state, Feed::MessageFeed* feed)
    {
        bool dir_evaluated = false;
        CmdBuffer::ClipRect clip = UI::convert(panel->window->content_viewport(panel->window->window_viewport()));
        // Test to see which panel the mouse is over, populate the file and reapply diffs.
        for (PartitionDirPanel* child = &panel->A;
            not null_dir_panel(child);
            child = child->sib_next)
        {
            CmdBuffer::ClipRect child_clip = clip_from_parent(clip, &panel->A, child);
            if (mouse_in_clip(state->mouse.ui_mouse, child_clip))
            {
                dir_evaluated = true;
                auto scratch = Arena::scratch_begin(Arena::no_conflicts);
                FlatDirEntryList dir_entries = {};
                populate_flattend_dir_entries(scratch.arena, &dir_entries, path, feed);
                diff_dir_list_view_populate_files(child->view, dir_entries);
                Arena::scratch_end(scratch);
                // Now try to apply diffs.
                diff_dir_panel_apply_diff(panel, feed);
                break;
            }
        }

        if (not dir_evaluated)
        {
            feed->queue_warning("Please drop file over specific side to apply directory diffs.");
        }
    }

    bool diff_dir_panel_nav_diff(DiffDirPanel* panel, NextDiffOrder order, Feed::MessageFeed* feed)
    {
        if (panel->diff_cache.size == 0)
        {
            feed->queue_info("No files in directory to navigate to.");
            return false;
        }

        SelectedDiffFile next_sel = SelectedDiffFile::Sentinel;
        switch (order)
        {
        case NextDiffOrder::Next:
            next_sel = SelectedDiffFile{ (rep(panel->selected_file) + 1) % panel->diff_cache.size };
            if (next_sel < panel->selected_file)
            {
                feed->queue_info("Back to first diff.");
            }
            break;
        case NextDiffOrder::Previous:
            next_sel = SelectedDiffFile{ (rep(panel->selected_file) + panel->diff_cache.size - 1) % panel->diff_cache.size };
            if (next_sel > panel->selected_file)
            {
                feed->queue_info("Back to last diff.");
            }
            break;
        }
        panel->selected_file = next_sel;
        return panel->selected_file != SelectedDiffFile::Sentinel;
    }

    void diff_dir_panel_sync_thread_data(DiffDirPanel* panel, Feed::MessageFeed* feed)
    {
        if (panel->thread_data == nullptr)
            return;
        if (panel->thread_data->state == DiffDirThreadState::Computed)
            return;
        // Identify handles still in-progress.
        Thread::ThreadPool* pool = Thread::system_thread_pool();
        bool all_completed = true;
        for EachIndex(i, panel->thread_data->size)
        {
            Thread::TaskHandle async_work = panel->thread_data->async_tasks[i];
            // This thread is done.
            if (async_work == Thread::TaskHandle::Sentinel)
                continue;
            Thread::TaskResult result = pool->result_if_complete(async_work);
            if (result.task_data)
            {
                {
                    char fmt_buf[100];
                    String8 msg = fmt_string(fmt_buf, "Thread[%I64d] computed diffs in %I64dms.", i, rep(result.ms));
                    feed->queue_info(msg);
                }
                // Clear the work.
                panel->thread_data->async_tasks[i] = Thread::TaskHandle::Sentinel;
            }
            else
            {
                // Still working...
                all_completed = false;
            }
        }
        // Pull results from the queues.
        // Pull a max of the first queue (they're all the same size).
        uint32_t max_ccq_check = panel->thread_data->queues[0].capacity;
        {
            PROF_SCOPE();
            for EachIndex(i, panel->thread_data->size)
            {
                DiffDirThreadChunk* chunk = &panel->thread_data->diff_chunks[i];
                for EachIndex(j, max_ccq_check)
                {
                    if (not Thread::ccq_cons_empty(chunk->ccq))
                    {
                        uint32_t idx = Thread::ccq_cons_shift(chunk->ccq);
                        DiffCount count = chunk->diff_counts_slice[idx];
                        Thread::ccq_cons_commit_shift(chunk->ccq);
                        // Create full index.
                        uint64_t full_idx = idx + (chunk->diff_counts_slice - panel->thread_data->computed_diff_counts.array);
                        // Update UI.
                        diff_dir_list_view_update_diff_count(panel->A.view, count, full_idx);
                    }
                    else
                    {
                        break;
                    }
                }
            }
        }

        if (all_completed)
        {
            panel->thread_data->sw.stop();
            char fmt_buf[100];
            String8 msg = fmt_string(fmt_buf, "Diffs completed in %I64dms.", panel->thread_data->sw.to_ms());
            feed->queue_info(msg);
#if BUILD_DEBUG
            // Verify we actually pulled everything.
            for EachIndex(i, panel->thread_data->size)
            {
                assert(Thread::ccq_cons_empty(&panel->thread_data->queues[i]));
            }
#endif // BUILD_DEBUG
        }
        else
        {
            // Keep requesting frames until done.
            Render::request_frames();
        }
    }

    // Queries.
    DiffDirDiffResults diff_dir_panel_cached_diffs(DiffDirPanel* panel, uint64_t diff_idx)
    {
        DiffDirDiffResults result = {};
        if (diff_idx >= panel->diff_cache.size)
            return result;
        MergedFileArray files_A = diff_dir_list_view_merged_file_array(panel->A.view);
        MergedFileArray files_B = diff_dir_list_view_merged_file_array(panel->B.view);
        if (diff_idx >= files_A.size or diff_idx >= files_B.size)
            return result;
        result.A.file = &files_A.array[diff_idx].file;
        result.B.file = &files_B.array[diff_idx].file;
        result.A.file_line_diffs = panel->diff_cache.file_line_diffs[0][diff_idx];
        result.B.file_line_diffs = panel->diff_cache.file_line_diffs[1][diff_idx];
        result.A.file_text_block_diffs = panel->diff_cache.file_text_block_diffs[0][diff_idx];
        result.B.file_text_block_diffs = panel->diff_cache.file_text_block_diffs[1][diff_idx];
        return result;
    }

    uint64_t diff_dir_panel_selected_diff(DiffDirPanel* panel)
    {
        return rep(panel->selected_file);
    }

    // Building.
    DiffDirPanelResponse build_diff_dir_panel(DiffDirPanel* panel,
                                                CmdBuffer::CmdList* cmd_lst,
                                                CmdBuffer::DrawList* core_lst,
                                                UI::UIState* state,
                                                Feed::MessageFeed* feed)
    {
        PROF_SCOPE();

        DiffDirPanelResponse resp = {};

        CmdBuffer::ClipRect clip = CmdBuffer::current_clip(*core_lst);
        const auto& colors = Config::widget_colors();

        // Start the frame for the enclosing editor frame.
        CmdBuffer::new_frame(panel->frame_lst, core_lst->screen, { .dt = core_lst->delta_time, .app_time = core_lst->app_time });
        // Default clip rect for the screen.
        CmdBuffer::push_clip(panel->frame_lst, clip);
        // Default texture (atlas by default).
        CmdBuffer::push_texture(panel->frame_lst, panel->atlas->atlas_texture());
        // Default palette.
        CmdBuffer::push_color_palette(panel->frame_lst, *CmdBuffer::current_palette(*core_lst));

        // Build the window first.
        {
            auto window_resp = panel->window->build(panel->frame_lst, panel->atlas, state);
            resp.close = window_resp.close;
        }
        // Now we can constrain the clip.
        clip = UI::convert(panel->window->content_viewport(panel->window->window_viewport()));
        CmdBuffer::push_clip(panel->frame_lst, clip);
        Glyph::FontSize font_size = Glyph::FontSize{ Config::diff_state().diff_font_size };

        // Build panel decoration UI.
        {
            CmdBuffer::ClipRect header_clip = clip;
            auto font_ctx = panel->atlas->render_font_context(font_size);
            header_clip.height = Height(UI::standard_font_padding(font_size));
            Vec2f base_pos;
            base_pos.y = static_cast<float>(rep(clip.height));
            // Create titles for each panel and center them.
            for (PartitionDirPanel* child = &panel->A;
                not null_dir_panel(child);
                child = child->sib_next)
            {
                CmdBuffer::ClipRect child_clip = clip_from_parent(clip, &panel->A, child);
                String8 name = diff_dir_list_view_base_dir(child->view);
                CmdBuffer::start_glyph_run(panel->frame_lst, Render::VertShader::OneOneTransform);
                Vec2f pos = base_pos;
                pos.y -= font_ctx.current_font_line_height();
                pos.x = rep(child_clip.offset_x) - rep(clip.offset_x) + (rep(child_clip.width) - font_ctx.measure_text(name).x) / 2.f;
                font_ctx.render_text(panel->frame_lst, name, pos, colors.window_title_font_color);
            }
            // Replace the clip.
            clip.height = retract(clip.height, rep(header_clip.height));
            CmdBuffer::pop_clip(panel->frame_lst);
            CmdBuffer::push_clip(panel->frame_lst, clip);
        }

        // Build non-leaf UI.
        {
            CmdBuffer::start_shapes(panel->frame_lst, Render::VertShader::OneOneTransform);
            Vec4f region_color = colors.outline_selection;
            const float boundary_width_bias = rep(font_size) / 3.f;
            for (PartitionDirPanel* child = &panel->A;
                // Non-leaf UI does only involves inner-panels (e.g. the fence post problem).
                not null_dir_panel(child) and not null_dir_panel(child->sib_next);
                child = child->sib_next)
            {
                PartitionDirPanel* sib = child->sib_next;
                CmdBuffer::ClipRect child_clip = clip_from_parent(clip, &panel->A, child);
                CmdBuffer::ClipRect sib_clip = clip_from_parent(clip, &panel->A, sib);
                CmdBuffer::ClipRect boundary_clip = {};

                Vec4f panelv_clip = clip_as_vec(clip);
                {
                    Vec4f childv_clip = clip_as_vec(child_clip);
                    Vec4f sibv_clip = clip_as_vec(sib_clip);
                    Vec4f boundaryv_clip{};
                    boundaryv_clip.p0[0] = childv_clip.p1[0] - PartitionDirPanel::padding;
                    boundaryv_clip.p1[0] = sibv_clip.p0[0] + PartitionDirPanel::padding;
                    boundaryv_clip.p0[1] = panelv_clip.p0[1];
                    boundaryv_clip.p1[1] = panelv_clip.p1[1];
                    boundary_clip = vec_as_clip(boundaryv_clip);
                }

                Widgets::ID boundary_id = Widgets::ID::Zero;
                {
                    Widgets::ID ids[] = { panel->id, child->id, sib->id };
                    Widgets::MultiSeed multi_seed_in{
                        .first = ids,
                        .last = ids + std::size(ids)
                    };
                    boundary_id = Widgets::make_multi_seed(multi_seed_in, "bndry");
                }

                if (mouse_in_clip(state->mouse.ui_mouse, pad_clip(boundary_clip, Vec2i(static_cast<int>(-boundary_width_bias)))))
                {
                    try_set_hot_widget(state, boundary_id);
                    if (down(*state, MouseButton::L))
                    {
                        bool first_focus = state->focus_widget != boundary_id;
                        try_set_focus_widget(state, boundary_id);
                        if (state->focus_widget == boundary_id
                            and first_focus)
                        {
                            // Stash some drag data.
                            Vec2f start_pct{ child->pct_of_parent, sib->pct_of_parent };
                            start_drag(state, boundary_id, state->mouse.ui_mouse, start_pct);
                        }
                    }
                }

                // Process movement.
                if (dragging(*state, boundary_id))
                {
                    const Vec2f* drag_data = drag_payload<Vec2f>(state);
                    constexpr float min_pixel_value = 50.f;
                    Vec2i mouse_delta = state->mouse.ui_mouse - state->drag.payload.start_point;
                    float total_size = panelv_clip.p1[0] - panelv_clip.p0[0];
                    float child_pct_before = drag_data->x; // Child %.
                    float child_pixels_before = child_pct_before * total_size;
                    float child_pixels_after = std::max(child_pixels_before + mouse_delta.xy[0], min_pixel_value);
                    float child_pct_after = child_pixels_after / total_size;

                    float pct_delta = child_pct_after - child_pct_before;
                    float sib_pct_before = drag_data->y; // Sib %.
                    float sib_pct_after = sib_pct_before - pct_delta;
                    float sib_pixels_after = sib_pct_after * total_size;
                    if (sib_pixels_after < 50.f)
                    {
                        sib_pixels_after = 50.f;
                        sib_pct_after = sib_pixels_after / total_size;
                        pct_delta = -(sib_pct_after - sib_pct_before);
                        child_pct_after = child_pct_before + pct_delta;
                    }
                    child->pct_of_parent = child_pct_after;
                    sib->pct_of_parent = sib_pct_after;
                }

                if (state->focus_widget == boundary_id
                    and not down(*state, MouseButton::L)
                    and clicked_count(*state, MouseButton::L) == 2)
                {
                    // If the boundary is double-clicked, we'll resize both boundaries to be even.
                    float pct_sum = child->pct_of_parent + sib->pct_of_parent;
                    child->pct_of_parent = 0.5f * pct_sum;
                    sib->pct_of_parent = 0.5f * pct_sum;
                }

                if ((state->hot_widget == boundary_id
                        and self_or_empty_focus_widget(*state, boundary_id))
                    or dragging(*state, boundary_id))
                {
                    auto [pos, size] = pos_size_clip(boundary_clip);
                    // Remove the offsets from the enclosing window clip.
                    pos.x -= rep(clip.offset_x);
                    pos.y -= rep(clip.offset_y);
                    CmdBuffer::solid_rect(panel->frame_lst, Render::FragShader::BasicColor, pos, size, region_color);
                    change_cursor(state, UI::CursorStyle::LeftRightArrow);
                }
            }
        }

        DiffDirListView* scroll_changed[] = {
            nullptr,
            nullptr
        };
        uint64_t scroll_idx = 0;

        // Build leaf-UI.
        for (PartitionDirPanel* child = &panel->A;
            not null_dir_panel(child);
            child = child->sib_next)
        {
            CmdBuffer::ClipRect child_clip = clip_from_parent(clip, &panel->A, child);
            // Setup command buffer for panel.
            CmdBuffer::new_frame(child->draw_lst, core_lst->screen, { .dt = core_lst->delta_time, .app_time = core_lst->app_time });
            // Create the rect.
            CmdBuffer::push_clip(child->draw_lst, child_clip);
            // Default texture (atlas by default).
            CmdBuffer::push_texture(child->draw_lst, panel->atlas->atlas_texture());
            // Default palette.
            CmdBuffer::push_color_palette(child->draw_lst, *CmdBuffer::current_palette(*core_lst));

            // Build core widget.
            auto r = build_diff_dir_list_view(child->view, child->draw_lst, state, panel->selected_file);
            scroll_changed[scroll_idx++] = r.scroll_changed ? child->view : nullptr;
            if (r.pop_to_diff)
            {
                // Validate that the diff is actually computed.
                if (diff_dir_list_valid_diff(panel->A.view, resp.diff_idx))
                {
                    resp.pop_to_diff = true;
                    resp.diff_idx = r.file_idx;
                }
                else
                {
                    feed->queue_info("Diff still computing...");
                }
                panel->selected_file = SelectedDiffFile{ r.file_idx };
            }

            CmdBuffer::pop_clip(child->draw_lst);
            CmdBuffer::pop_texture(child->draw_lst);
            CmdBuffer::pop_color_palette(child->draw_lst);

            CmdBuffer::push_draw_list(cmd_lst, CmdBuffer::DrawListLayer::_2, child->draw_lst);
        }

        for EachIndex(i, std::size(scroll_changed))
        {
            DiffDirListView* view = scroll_changed[i];
            if (view != nullptr)
            {
                DiffDirListView* share_to = panel->A.view;
                if (view == panel->A.view)
                {
                    share_to = panel->B.view;
                }
                diff_dir_list_view_share_scroll_pos(share_to, view);
            }
        }

        panel->window->end(state);

        CmdBuffer::pop_clip(panel->frame_lst); // Window.
        CmdBuffer::pop_clip(panel->frame_lst); // Core.
        CmdBuffer::pop_texture(panel->frame_lst);
        CmdBuffer::pop_color_palette(panel->frame_lst);

        CmdBuffer::push_draw_list(cmd_lst, CmdBuffer::DrawListLayer::_1, panel->frame_lst);
        return resp;
    }
} // namespace Diff