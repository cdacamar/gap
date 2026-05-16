#pragma once

#include "timers.h"
#include "types.h"

namespace Thread
{
    struct ThreadWorkData;

    using ThreadWorkFn = void(*)(ThreadWorkData*);

    struct ThreadWorkData
    {
        void* data;
        ThreadWorkFn work_fn;
        Timers::Stopwatch sw;
        uint32_t cancellation_flag;
    };
} // namespace Thread