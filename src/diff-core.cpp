#include "diff-core.h"

#include "os.h"

namespace Diff
{
    namespace
    {
        struct SnakePoint
        {
            OffT x;
            OffT y;
        };

        struct DiffBox
        {
            OffT left;
            OffT top;
            OffT right;
            OffT bottom;
        };

        SUPPRESS_NONSTANDARD_EXTENSION_WARNING();

        struct DiffInput
        {
            union
            {
                struct
                {
                    const TextFile* file_a;
                    const TextFile* file_b;
                };
                struct
                {
                    const DiffBlockInput* block_a;
                    const DiffBlockInput* block_b;
                };
                struct
                {
                    const DiffWordInput* words_a;
                    const DiffWordInput* words_b;
                };
            };
            bool (*cmp)(const DiffInput*, OffT a_idx, OffT b_idx);
        };

        ENABLE_NONSTANDARD_EXTENSION_WARNING();

        void text_file_populate_line_starts(Arena::Arena* arena, TextFile* result)
        {
            // Let's start with a simple linked list of these line starts that we can then
            // flatten into an array.
            auto scratch = Arena::scratch_begin({ &arena, 1 });
            struct StartNode
            {
                StartNode* next;
                CharOffset start;
            };
            StartNode* first = nullptr;
            StartNode* last = nullptr;
            uint64_t count = 0;

            for EachIndex(i, result->content.size)
            {
                if (result->content.str[i] == '\n')
                {
                    StartNode* n = Arena::push_array<StartNode>(scratch.arena, 1);
                    n->start = CharOffset{ i + 1 };
                    SLLQueuePush(first, last, n);
                    ++count;
                }
            }
            // Flatten.
            result->line_starts.size = count + 1;
            result->line_starts.array = Arena::push_array_no_zero<CharOffset>(arena, result->line_starts.size);
            // First line.
            result->line_starts.array[0] = CharOffset{};
            uint64_t idx = 1;
            for EachNode(n, first)
            {
                result->line_starts.array[idx++] = n->start;
            }
            Arena::scratch_end(scratch);
        }

        Edit make_edit(EditType type, OffT idx_a, OffT idx_b)
        {
            Edit edit = {
                .idx_a = idx_a,
                .idx_b = idx_b,
                .type = type,
            };
            return edit;
        }

        DiffBox make_box(OffT left, OffT top, OffT right, OffT bottom)
        {
            DiffBox box = {
                .left = left,
                .top = top,
                .right = right,
                .bottom = bottom
            };
            return box;
        }

        bool same_line(const DiffInput* input, OffT line_a, OffT line_b)
        {
            CursorLine l_a = CursorLine(line_a);
            CursorLine l_b = CursorLine(line_b);
            String8 ltxt_a = text_file_line_text(*input->file_a, l_a);
            String8 ltxt_b = text_file_line_text(*input->file_b, l_b);
            return str8_match_exact(ltxt_a, ltxt_b);
        }

        bool same_text(const DiffInput* input, OffT index_a, OffT index_b)
        {
            const TextFile* file_a = input->block_a->file;
            const TextFile* file_b = input->block_b->file;
            CharOffset off_a = input->block_a->block.underlying_off[index_a];
            CharOffset off_b = input->block_b->block.underlying_off[index_b];
            assert(rep(off_a) < file_a->content.size);
            assert(rep(off_b) < file_b->content.size);
            return file_a->content.str[rep(off_a)] == file_b->content.str[rep(off_b)];
        }

        bool same_word(const DiffInput* input, OffT index_a, OffT index_b)
        {
            const TextFile* file_a = input->words_a->file;
            const TextFile* file_b = input->words_b->file;
            DiffWord word_off_a = input->words_a->words.words[index_a];
            DiffWord word_off_b = input->words_b->words.words[index_b];
            String8 word_a = str8_substr(file_a->content, { .off = rep(word_off_a.first), .len = rep(distance(word_off_a.first, word_off_a.last)) });
            String8 word_b = str8_substr(file_b->content, { .off = rep(word_off_b.first), .len = rep(distance(word_off_b.first, word_off_b.last)) });
            return str8_match_exact(word_a, word_b);
        }

        void push_edit(Arena::Arena* arena, EditList* lst, Edit edit)
        {
            EditNode* node = Arena::push_array<EditNode>(arena, 1);
            node->edit = edit;
            SLLQueuePush(lst->first, lst->last, node);
            ++lst->count;
        }

        void push_equal(Arena::Arena* arena, EditList* lst, OffT idx_a, OffT idx_b)
        {
            Edit e = {
                .idx_a = idx_a,
                .idx_b = idx_b,
                .type = EditType::Eq,
            };
            push_edit(arena, lst, e);
        }

        void push_delete(Arena::Arena* arena, EditList* lst, OffT idx_a)
        {
            Edit e = {
                .idx_a = idx_a,
                .idx_b = diff_idx_sentinel,
                .type = EditType::Del,
            };
            push_edit(arena, lst, e);
        }

        void push_insert(Arena::Arena* arena, EditList* lst, OffT idx_b)
        {
            Edit e = {
                .idx_a = diff_idx_sentinel,
                .idx_b = idx_b,
                .type = EditType::Ins,
            };
            push_edit(arena, lst, e);
        }

        struct DiagArea
        {
            OffT* buf;
            uint64_t size;
        };

        DiagArea make_diagonal_area(Arena::Arena* arena, uint64_t size)
        {
            DiagArea result = {};
            result.size = size;
            result.buf = Arena::push_array_no_zero<OffT>(arena, result.size);
            return result;
        }

        struct FindMiddleSnakeResult
        {
            SnakePoint start;
            SnakePoint end;
            bool found;
        };

        FindMiddleSnakeResult find_middle_snake(const DiffInput* input, DiffBox box)
        {
            FindMiddleSnakeResult result = {};
            OffT width = box.right - box.left;
            OffT height = box.bottom - box.top;
            OffT box_size = width + height;

            // An early out if our box size is not computable.
            if (box_size == 0)
                return result;

            auto scratch = Arena::scratch_begin(Arena::no_conflicts);
            OffT max_depth = (box_size + 1) / 2;
            OffT delta = width - height;
            bool odd_delta = (delta & 1) != 0;

            OffT diagonal_limit = max_depth + 1;
            OffT area_size = diagonal_limit * 2 + 1;
            OffT diagonal_offset = diagonal_limit;

            DiagArea forward = make_diagonal_area(scratch.arena, area_size);
            DiagArea backward = make_diagonal_area(scratch.arena, area_size);

            // Note: due to the way the algorithm below walks the diagonals, we don't actually
            // need to initialize the actual buffers of forward and backward above.  An uninitialized
            // access never happens because we fill each box from the inside out so each successive
            // box as distance grows will always have a valid inner box to index into.

            SUPPRESS_MULTI_LINE_COMMENT_WARNING();
            // Here's a helpful graphic of the boxes being built for the sample code:
            // // a.c
            //
            // size_t Chunk_copy(Chunk *src, size_t src_start, Chunk *dst, size_t dst_start, size_t n)
            // {
            //     if (!Chunk_bounds_check(src, src_start, n)) return 0;
            //     if (!Chunk_bounds_check(dst, dst_start, n)) return 0;
            // 
            //     memcpy(dst->data + dst_start, src->data + src_start, n);
            // 
            //     return n;
            // }
            //
            // int Chunk_bounds_check(Chunk *chunk, size_t start, size_t n)
            // {
            //     if (chunk == NULL) return 0;
            //
            //     size_t length = chunk->length;
            //
            //     return start <= length && n <= length - start;
            // }
            // -----
            // // b.c
            // 
            // int Chunk_bounds_check(Chunk *chunk, size_t start, size_t n)
            // {
            //     if (chunk == NULL) return 0;
            // 
            //     size_t length = chunk->length;
            // 
            //     return start <= length && n <= length - start;
            // }
            //
            // size_t Chunk_copy(Chunk *src, size_t src_start, Chunk *dst, size_t dst_start, size_t n)
            // {
            //     if (!Chunk_bounds_check(src, src_start, n)) return 0;
            //     if (!Chunk_bounds_check(dst, dst_start, n)) return 0;
            // 
            //     memcpy(dst->data + dst_start, src->data + src_start, n);
            // 
            //     return n;
            // }
            // -----
            //     0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16  17  18
            //  0  o---o---o---o---o
            //     |   |   |   |   |
            //  1  o---o---o---o---o
            //     |   | \ |   |   |
            //  2  o---o---o---o---o
            //                     |
            //  3                  @
            //                       \
            //  4                      o---o---o---o
            //                         |   |   |   |
            //  5                      o---o---o---o
            //                         |   | \ |   |
            //  6                      o---o---o---o
            //                         |   |   |   |
            //  7                      o---o---o---o
            //                                       \
            //  8                                      @
            //                                           \
            //  9                                          @---o---o---o
            //                                                 |   |   |
            // 10                                              o---o---o
            //                                                 | \ |   |
            // 11                                              o---o---o
            //                                                 |   |   |
            // 12                                              o---o---o
            //                                                 |   |   |
            // 13                                              o---o---o
            //                                                           \
            // 14                                                          @---o---o---o---o
            //                                                                 |   |   |   |
            // 15                                                              o---o---o---o
            //                                                                 | \ |   |   |
            // 16                                                              o---o---o---o
            //                                                                 |   |   |   |
            // 17                                                              o---o---o---o
            //                                                                 |   |   | \ |
            // 18                                                              o---o---o---o
            ENABLE_MULTI_LINE_COMMENT_WARNING();

            // Because we're searching an area over 'k' and 'c' and these will include negative numbers, we
            // split our buffer in half and start indexing into that so we can go backwards.  Since the area
            // buffer is 'size * 2' this gives us all the space we need for the search.
            OffT* forward_buf = forward.buf + diagonal_offset;
            OffT* backward_buf = backward.buf + diagonal_offset;

            // Set the intial search conditions.
            forward_buf[1] = box.left;
            backward_buf[1] = box.bottom;

            for (OffT distance = 0; distance <= max_depth; ++distance)
            {
                // Forward search.
                for (OffT k = distance; k >= -distance; k -= 2)
                {
                    OffT previous_x;
                    OffT x;
                    if (k == -distance or (k != distance and forward_buf[k - 1] < forward_buf[k + 1]))
                    {
                        previous_x = forward_buf[k + 1];
                        x = previous_x;
                    }
                    else
                    {
                        previous_x = forward_buf[k - 1];
                        x = previous_x + 1;
                    }

                    OffT y = box.top + (x - box.left) - k;
                    OffT previous_y = (distance == 0 or x != previous_x) ? y : (y - 1);

                    // Eat common prefix.
                    while (x < box.right and y < box.bottom and input->cmp(input, x, y))
                    {
                        ++x;
                        ++y;
                    }

                    forward_buf[k] = x;

                    OffT reverse_diag = k - delta;
                    if (odd_delta and reverse_diag >= -(distance - 1) and reverse_diag <= (distance - 1) and y >= backward_buf[reverse_diag])
                    {
                        result.start.x = previous_x;
                        result.start.y = previous_y;
                        result.end.x = x;
                        result.end.y = y;
                        result.found = true;
                        break;
                    }
                }

                // If the forward search found a result, we are done.
                if (result.found)
                    break;

                // Backward search.
                for (OffT reverse_diag = distance; reverse_diag >= -distance; reverse_diag -= 2)
                {
                    OffT k = reverse_diag + delta;
                    OffT previous_y;
                    OffT y;
                    if (reverse_diag == -distance or (reverse_diag != distance and backward_buf[reverse_diag - 1] > backward_buf[reverse_diag + 1]))
                    {
                        previous_y = backward_buf[reverse_diag + 1];
                        y = previous_y;
                    }
                    else
                    {
                        previous_y = backward_buf[reverse_diag - 1];
                        y = previous_y - 1;
                    }

                    OffT x = box.left + (y - box.top) + k;
                    OffT previous_x = (distance == 0 or y != previous_y) ? x : (x + 1);

                    // Eat common suffix.
                    while (x > box.left and y > box.top and input->cmp(input, x - 1, y - 1))
                    {
                        --x;
                        --y;
                    }

                    backward_buf[reverse_diag] = y;

                    if (not odd_delta and k >= -distance and k <= distance and x <= forward_buf[k])
                    {
                        result.start.x = x;
                        result.start.y = y;
                        result.end.x = previous_x;
                        result.end.y = previous_y;
                        result.found = true;
                        break;
                    }
                }

                // If the backward search found a result, we are done.
                if (result.found)
                    break;
            }

            Arena::scratch_end(scratch);
            return result;
        }

        void record_middle_snakes(Arena::Arena* arena,
                                    const DiffInput* input,
                                    FindMiddleSnakeResult find_r,
                                    EditList* lst)
        {
            OffT x = find_r.start.x;
            OffT y = find_r.start.y;

            // Add common prefixes.
            while (x < find_r.end.x
                    and y < find_r.end.y
                    and input->cmp(input, x, y))
            {
                push_equal(arena, lst, x, y);
                ++x;
                ++y;
            }

            OffT remaining_x = find_r.end.x - x;
            OffT remaining_y = find_r.end.y - y;
            // x > y implies delete from A.
            if (remaining_x > remaining_y)
            {
                push_delete(arena, lst, x);
                ++x;
            }
            // y > x implies insertion from B.
            else if (remaining_y > remaining_x)
            {
                push_insert(arena, lst, y);
                ++y;
            }

            // Clean up any remaining common suffix.
            while (x < find_r.end.x
                    and y < find_r.end.y
                    and input->cmp(input, x, y))
            {
                push_equal(arena, lst, x, y);
                ++x;
                ++y;
            }

            // The entire sequence should have been consumed.
            assert(x == find_r.end.x and y == find_r.end.y);
        }

        // Inspiration for this implementation: https://gist.github.com/jcoglan/4e364e98ced2189641c372eb2d92caac
        // Article at: https://blog.jcoglan.com/2017/04/25/myers-diff-in-linear-space-implementation/
        void unified_diff_box(Arena::Arena* arena, const DiffInput* input, DiffBox box, EditList* lst)
        {
            // Add common prefixes up front.
            while (box.left < box.right and box.top < box.bottom and input->cmp(input, box.left, box.top))
            {
                push_equal(arena, lst, box.left, box.top);
                ++box.left;
                ++box.top;
            }

            OffT suffix_length = 0;
            // Rewind the box to the nearest uncommon suffix, but we can't record them yet otherwise
            // the final edit list will be in the wrong order.
            while (box.left < box.right and box.top < box.bottom and input->cmp(input, box.right - 1, box.bottom - 1))
            {
                --box.right;
                --box.bottom;
                ++suffix_length;
            }

            OffT width = box.right - box.left;
            OffT height = box.bottom - box.top;

            FindMiddleSnakeResult middle_snake = find_middle_snake(input, box);
            if (not middle_snake.found)
            {
                assert((width + height) == 0);
                GAP_UNUSED(width);
                GAP_UNUSED(height);
                return;
            }

            // If the middle snake encloses the entire sub-box we're looking at, we can emit the snake
            // without any further recursive calls.
            if (middle_snake.start.x     == box.left
                and middle_snake.start.y == box.top
                and middle_snake.end.x   == box.right
                and middle_snake.end.y   == box.bottom)
            {
                record_middle_snakes(arena, input, middle_snake, lst);
            }
            else
            {
                unified_diff_box(arena, input, make_box(box.left, box.top, middle_snake.start.x, middle_snake.start.y), lst);
                record_middle_snakes(arena, input, middle_snake, lst);
                unified_diff_box(arena, input, make_box(middle_snake.end.x, middle_snake.end.y, box.right, box.bottom), lst);
            }

            for (OffT i = 0; i < suffix_length; ++i)
            {
                push_equal(arena, lst, box.right + i, box.bottom + i);
            }
        }
    } // namespace [anon]

    TextFile text_file_read(Arena::Arena* arena, String8 path)
    {
        TextFile result = {};
        if (read_entire_file(arena, &result.content, path))
        {
            text_file_populate_line_starts(arena, &result);
            // Note: We _probably_ shouldn't ignore the error code here but... w/e.
            OS::canonical_file_path(arena, &result.path, path);
        }
        else
        {
            fprintf(stderr, "ERROR: Failed to load file '%.*s'\n", int(path.size), path.str);
        }
        return result;
    }

    String8 text_file_line_text(const TextFile& file, CursorLine line)
    {
        // This is outside the file.  Return empty string.
        // Note: Lines are 1-indexed.
        if (rep(line) - 1 >= file.line_starts.size)
            return str8_empty;
        CharOffset start = file.line_starts.array[rep(line) - 1];
        CharOffset end = start;
        CursorLine next_l = extend(line);
        if (rep(next_l) - 1 >= file.line_starts.size)
        {
            end = CharOffset{ file.content.size };
        }
        else
        {
            end = file.line_starts.array[rep(next_l) - 1];
            // Note: We don't actually want to include the '\n' of the previous line,
            // so we remove it here.
            end = retract(end);
        }
        String8 substr = str8_substr(file.content, { .off = rep(start), .len = rep(distance(start, end)) });
        return substr;
    }

    LineRange text_file_line_range(const TextFile& file, CursorLine line)
    {
        LineRange result = {};
        // This is outside the file.  Return empty range.
        // Note: Lines are 1-indexed.
        if (rep(line) - 1 >= file.line_starts.size)
            return result;
        result.first = file.line_starts.array[rep(line) - 1];
        result.last = result.first;
        CursorLine next_l = extend(line);
        if (rep(next_l) - 1 >= file.line_starts.size)
        {
            result.last = CharOffset{ file.content.size };
        }
        else
        {
            result.last = file.line_starts.array[rep(next_l) - 1];
            // Note: We don't actually want to include the '\n' of the previous line,
            // so we remove it here.
            result.last = retract(result.last);
        }
        return result;
    }

    CursorLine text_file_line_for_offset(const TextFile& file, CharOffset off)
    {
        if (file.line_starts.size <= 1)
            return CursorLine::Beginning;
        // We can binary search for the line.
        uint64_t low = 0;
        uint64_t high = file.line_starts.size - 1;
        uint64_t mid = 0;
        while (low <= high)
        {
            mid = low + ((high - low) / 2);
            if (mid == high)
                break;
            uint64_t mid_start = rep(file.line_starts.array[mid]);
            uint64_t mid_stop = rep(file.line_starts.array[mid + 1]);
            if (rep(off) < mid_start)
            {
                high = mid - 1;
            }
            else if (rep(off) >= mid_stop)
            {
                low = mid + 1;
            }
            else
            {
                break;
            }
        }
        return CursorLine{ mid + 1 };
    }

    TextFile text_file_copy_to(Arena::Arena* arena, const TextFile& file)
    {
        TextFile result = {};
        result.path = str8_copy(arena, file.path);
        result.content = str8_copy(arena, file.content);
        result.line_starts.size = file.line_starts.size;
        result.line_starts.array = Arena::push_array_no_zero<CharOffset>(arena, result.line_starts.size);
        memcpy(result.line_starts.array, file.line_starts.array, result.line_starts.size * sizeof(CharOffset));
        return result;
    }

    // Diffing.
    // Note: This is the linear space variant of the Myers diff algorithm.
    EditList diff_file_lines(Arena::Arena* arena, const TextFile& a, const TextFile& b)
    {
        EditList result = {};
        DiffInput input = { .file_a = &a, .file_b = &b, .cmp = same_line };
        // Note: The box starts at 1, 1 because our lines start as 1-indexed.
        //       The box also ends at lines + 1 as line_starts.size == last line.
        unified_diff_box(arena, &input, make_box(1, 1, OffT(a.line_starts.size) + 1, OffT(b.line_starts.size) + 1), &result);
        return result;
    }

    EditList diff_file_block(Arena::Arena* arena, const DiffBlockInput& a, const DiffBlockInput& b)
    {
        EditList result = {};
        DiffInput input = { .block_a = &a, .block_b = &b, .cmp = same_text };
        unified_diff_box(arena, &input, make_box(0, 0, OffT(a.block.size), OffT(b.block.size)), &result);
        return result;
    }

    EditList diff_file_words(Arena::Arena* arena, const DiffWordInput& a, const DiffWordInput& b)
    {
        EditList result = {};
        DiffInput input = { .words_a = &a, .words_b = &b, .cmp = same_word };
        unified_diff_box(arena, &input, make_box(0, 0, OffT(a.words.size), OffT(b.words.size)), &result);
        return result;
    }
} // namespace Diff