-- Render the local time. Updates fire exactly on wall-clock boundaries of the
-- displayed precision, so the minute or second flips on time, and they are
-- rescheduled after clock jumps and suspends.
local allowed = {
    format = true,
    interval = true,
    order = true,
    key = true,
}

local function check_options(options)
    for key in pairs(options) do
        assert(allowed[key], "unknown clock option '" .. tostring(key) .. "'")
    end
    assert(options.format == nil or type(options.format) == "string", "clock format must be a string")
    assert(options.interval == nil or type(options.interval) == "number", "clock interval must be a number")
    assert(options.interval == nil or options.interval > 0, "clock interval must be positive")
end

-- strftime conversions that display seconds, optionally with the E or O
-- modifier: %S, %T, %r, %X, %c and the epoch seconds %s.
local function shows_seconds(format)
    return format:find("%%[EO]?[STrXcs]") ~= nil
end

return function(options)
    options = options or {}
    check_options(options)
    local format = options.format or "%H:%M"
    -- Seconds since the epoch between two updates; boundaries are its
    -- multiples, so 60 flips on every minute and 2 on every even second.
    local unit = options.interval or (shows_seconds(format) and 1 or 60)
    -- Handles must stay referenced: collecting one cancels it.
    local pending
    local observer

    local function render(ctx, at)
        ctx:set { full_text = os.date(format, at) }
    end

    local function schedule(ctx)
        if pending ~= nil then
            pending:cancel()
        end
        local now = tonumber(assert(ctx:sample("time")).realtime_ns) / 1e9
        local boundary = (math.floor(now / unit) + 1) * unit
        pending = ctx:at_realtime(boundary, function(timer_ctx)
            pending = nil
            -- os.date reads a coarse clock that may still show the previous
            -- second right at the boundary, so render the boundary itself
            -- unless the timer fired late.
            render(timer_ctx, math.max(math.floor(boundary), os.time()))
            schedule(timer_ctx)
        end)
    end

    block {
        name = "clock",
        key = options.key,
        order = options.order or 0,
        init = function(ctx)
            schedule(ctx)
            observer = ctx:on_clock_change(function(change_ctx)
                render(change_ctx)
                schedule(change_ctx)
            end)
        end,
        update = function(ctx)
            render(ctx)
        end,
    }
end
