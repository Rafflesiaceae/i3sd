local allowed = {
    interval = true,
    format = true,
    order = true,
    key = true,
}

local function check_options(options)
    for key in pairs(options) do
        assert(allowed[key], "unknown cpu option '" .. tostring(key) .. "'")
    end
    assert(options.format == nil or type(options.format) == "function", "cpu format must be a function")
end
return function(options)
    options = options or {}
    check_options(options)
    local previous
    local formatter = options.format or function(value)
        return ("CPU %.0f%%"):format(value.percent)
    end

    block {
        name = "cpu",
        key = options.key,
        order = options.order or 0,
        interval = options.interval or 2,
        update = function(ctx)
            local snapshot = assert(ctx:sample("cpu"))
            local current = snapshot.aggregate
            if previous == nil or snapshot.source_id ~= previous.source_id or snapshot.continuity ~= previous.continuity then
                previous = {
                    source_id = snapshot.source_id,
                    continuity = snapshot.continuity,
                    counters = current,
                }
                ctx:set { full_text = "" }
                return
            end

            local old = previous.counters
            local total = 0
            for _, key in ipairs { "user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal" } do
                if current[key] < old[key] then
                    previous.counters = current
                    ctx:set { full_text = "" }
                    return
                end
                total = total + current[key] - old[key]
            end
            local idle = current.idle - old.idle
            local iowait = current.iowait - old.iowait
            previous.counters = current
            if total == 0 then
                ctx:set { full_text = "" }
                return
            end
            local value = {
                percent = 100 * (total - idle - iowait) / total,
                iowait_percent = 100 * iowait / total,
            }
            local rendered = formatter(value)
            if rendered == nil or rendered == false then
                ctx:set { full_text = "" }
            elseif type(rendered) == "string" then
                ctx:set { full_text = rendered }
            else
                ctx:set(rendered)
            end
        end,
    }
end
