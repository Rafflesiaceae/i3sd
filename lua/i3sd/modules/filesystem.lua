local allowed = {
    path = true,
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
        assert(allowed[key], "unknown filesystem option '" .. tostring(key) .. "'")
    end
    assert(type(options.path) == "string", "filesystem path is required")
    assert(options.format == nil or type(options.format) == "function", "filesystem format must be a function")
end
return function(options)
    options = options or {}
    check_options(options)
    local warn = options.warn_below or 20
    local critical = options.critical_below or 10
    local hysteresis = options.hysteresis or 2
    assert(critical >= 0 and critical <= warn and warn <= 100, "invalid filesystem thresholds")
    local severity
    local formatter = options.format or function(value)
        return ("%s %.0f%% free"):format(value.path, value.available_percent)
    end

    block {
        name = "filesystem",
        key = options.key or options.path,
        order = options.order or 0,
        interval = options.interval or 60,
        update = function(ctx)
            local snapshot = assert(ctx:sample("filesystem", { path = options.path }))
            if snapshot.total_bytes == 0 then
                ctx:set { full_text = "" }
                return
            end
            local available = 100 * snapshot.available_bytes / snapshot.total_bytes
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
                path = options.path,
                available_percent = available,
                available_bytes = snapshot.available_bytes,
                total_bytes = snapshot.total_bytes,
                readonly = snapshot.readonly,
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
