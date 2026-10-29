-- A block that faults while one of its realtime timers is pending must cancel
-- that timer on the realtime heap. Cancelling it on the monotonic heap would
-- drop the survivor's poll timer, the only monotonic timer left by then.
local faulted = false
local ticks_after_fault = 0
-- Handles must stay referenced: collecting one cancels it.
local handles = {}

block {
    name = "faulty",
    init = function(ctx)
        -- 2100-01-01, far enough to still be pending at the fault.
        handles.pending = ctx:at_realtime(4102444800, function() end)
        handles.fault = ctx:after(0.1, function()
            faulted = true
            error("intentional timer fault")
        end)
    end,
}

block {
    name = "survivor",
    interval = 0.02,
    update = function(ctx)
        if faulted then
            ticks_after_fault = ticks_after_fault + 1
        end
        ctx:set {
            full_text = ticks_after_fault >= 5 and "survivor:ok"
                or "survivor:waiting",
        }
    end,
}
