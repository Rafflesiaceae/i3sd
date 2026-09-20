local previous
local current_percent
local expanded = false

local function render_cpu(ctx)
    if current_percent == nil then
        ctx:set { full_text = "" }
        return
    end
    local format = expanded and "CPU utilization %.1f%%" or "CPU %.0f%%"
    ctx:set { full_text = format:format(current_percent) }
end

block {
    name = "cpu",
    key = "interactive",
    order = 20,
    interval = 2,
    update = function(ctx)
        local snapshot = assert(ctx:sample("cpu"))
        local current = snapshot.aggregate
        if previous == nil then
            previous = current
            render_cpu(ctx)
            return
        end

        -- Guest counters are excluded because Linux already includes them in
        -- user and nice. I/O wait is reported separately from busy time.
        local fields = { "user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal" }
        local total = 0
        for _, field in ipairs(fields) do
            if current[field] < previous[field] then
                previous = current
                current_percent = nil
                render_cpu(ctx)
                return
            end
            total = total + current[field] - previous[field]
        end
        local idle = current.idle - previous.idle
        local iowait = current.iowait - previous.iowait
        previous = current
        current_percent = total > 0 and 100 * (total - idle - iowait) / total or nil
        render_cpu(ctx)
    end,
    click = function(ctx, button, _event)
        if button == 1 then
            expanded = not expanded
            render_cpu(ctx)
        end
    end,
}

-- Event-only blocks can compose one-shot timers without a periodic deadline.
block {
    name = "startup",
    order = 100,
    init = function(ctx)
        ctx:after(2, function(timer_ctx)
            timer_ctx:set { full_text = "i3sd ready", color = "#00cc66" }
            timer_ctx:after(3, function(hide_ctx)
                hide_ctx:set { full_text = "" }
            end)
        end)
    end,
}
