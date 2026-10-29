-- Realtime timers fire at absolute CLOCK_REALTIME instants, whether they
-- were created while staging or by a live callback.
local function now(ctx)
    return tonumber(assert(ctx:sample("time")).realtime_ns) / 1e9
end

local outcomes = {}
local updates = 0
-- Handles must stay referenced: collecting one cancels it.
local handles = {}

local function record(ctx, name)
    outcomes[#outcomes + 1] = name
    if #outcomes == 3 then
        table.sort(outcomes)
        ctx:set { full_text = "realtime:" .. table.concat(outcomes, ",") }
    end
end

block {
    name = "realtime",
    init = function(ctx)
        for _, invalid in ipairs { -1, 0 / 0, math.huge } do
            local ok, err = pcall(ctx.at_realtime, ctx, invalid, function() end)
            assert(not ok and err:find("invalid realtime timestamp", 1, true),
                err)
        end
        -- A staged deadline that already passed fires after activation.
        handles.staged = ctx:at_realtime(0, function(timer_ctx)
            record(timer_ctx, "staged")
        end)
        handles.observer = ctx:on_clock_change(function() end)
    end,
    update = function(ctx)
        -- The first update runs while staging; create these timers from the
        -- live refresh that follows activation.
        updates = updates + 1
        if updates ~= 2 then
            return
        end
        handles.live = ctx:at_realtime(now(ctx) + 0.2, function(timer_ctx)
            record(timer_ctx, "future")
        end)
        handles.past = ctx:at_realtime(now(ctx) - 5, function(timer_ctx)
            record(timer_ctx, "past")
        end)
        local cancelled = ctx:at_realtime(now(ctx) + 0.1, function()
            error("cancelled realtime timer fired")
        end)
        cancelled:cancel()
    end,
}
