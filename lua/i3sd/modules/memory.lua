local allowed = {
    interval = true,
    warn_below = true,
    critical_below = true,
    hysteresis = true,
    format = true,
    order = true,
    key = true,
}

local function check_options(options)
    for key in pairs(options) do
        assert(allowed[key], "unknown memory option '" .. tostring(key) .. "'")
    end
    assert(options.format == nil or type(options.format) == "function", "memory format must be a function")
end
return function(options)
    options = options or {}
    check_options(options)
    local warn = options.warn_below or 20
    local critical = options.critical_below or 10
    local hysteresis = options.hysteresis or 2
    assert(critical >= 0 and critical <= warn and warn <= 100, "invalid memory thresholds")
    local severity
    local formatter = options.format or function(value)
        return ("MEM %.0f%% free"):format(value.available_percent)
    end

    block {
        name = "memory",
        key = options.key,
        order = options.order or 0,
        interval = options.interval or 10,
        update = function(ctx)
            local snapshot = assert(ctx:sample("memory"))
            local available = 100 * snapshot.mem_available / snapshot.mem_total
            if severity == "critical" and available < critical + hysteresis then
                severity = "critical"
            elseif available <= critical then
                severity = "critical"
            elseif severity == "warning" and available < warn + hysteresis then
                severity = "warning"
            elseif available <= warn then
                severity = "warning"
            else
                severity = nil
            end
            if severity == nil then
                ctx:set { full_text = "" }
                return
            end
            local value = {
                available_percent = available,
                available_bytes = snapshot.mem_available,
                total_bytes = snapshot.mem_total,
                severity = severity,
            }
            local rendered = formatter(value)
            if rendered == nil or rendered == false then
                ctx:set { full_text = "" }
            elseif type(rendered) == "string" then
                ctx:set { full_text = rendered, color = severity == "critical" and "#ff0000" or "#ffaa00", urgent = severity == "critical" }
            else
                ctx:set(rendered)
            end
        end,
    }
end
