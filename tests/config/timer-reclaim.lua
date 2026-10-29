-- One generation runs far more timers than I3SD_MAX_TIMERS over its lifetime.
-- Fired one-shot timers and cancelled or collected timers must free their
-- slots, otherwise every chain below fails with "timer limit reached".
local rounds = 5000
local results = {}
-- Handles must stay referenced: collecting one cancels it.
local handles = {}

local function done(ctx, name)
    results[#results + 1] = name
    if #results == 4 then
        table.sort(results)
        ctx:set { full_text = "reclaim:" .. table.concat(results, ",") }
    end
end

local function never()
    error("cancelled timer fired")
end

block {
    name = "reclaim",
    init = function(ctx)
        -- One-shot monotonic timers, each scheduling the next one.
        local after_left = rounds
        local function after_step(timer_ctx)
            after_left = after_left - 1
            if after_left == 0 then
                done(timer_ctx, "after")
                return
            end
            handles.after = timer_ctx:after(0, after_step)
        end
        handles.after = ctx:after(0, after_step)

        -- Realtime timers rescheduled from their own callback like the clock
        -- module. Cancelling the running timer is deferred to the dispatcher.
        local realtime_left = rounds
        local function realtime_step(timer_ctx)
            handles.realtime:cancel()
            realtime_left = realtime_left - 1
            if realtime_left == 0 then
                done(timer_ctx, "realtime")
                return
            end
            handles.realtime = timer_ctx:at_realtime(0, realtime_step)
        end
        handles.realtime = ctx:at_realtime(0, realtime_step)

        -- Repeating timers that cancel themselves from their callback.
        local every_left = rounds
        local function every_step(timer_ctx)
            handles.every:cancel()
            every_left = every_left - 1
            if every_left == 0 then
                done(timer_ctx, "every")
                return
            end
            handles.every = timer_ctx:every(1e-6, every_step)
        end
        handles.every = ctx:every(1e-6, every_step)

        -- Timers cancelled before they fire, explicitly or by collecting
        -- their handle, in batches that stay below the limit between
        -- collections.
        handles.cancel = ctx:after(0, function(timer_ctx)
            for _ = 1, rounds do
                timer_ctx:after(3600, never):cancel()
                timer_ctx:at_realtime(4102444800, never):cancel()
            end
            for _ = 1, rounds / 1000 do
                for _ = 1, 1000 do
                    timer_ctx:after(3600, never)
                end
                collectgarbage()
            end
            done(timer_ctx, "cancel")
        end)
    end,
}
