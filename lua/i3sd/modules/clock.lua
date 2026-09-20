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
end
return function(options)
    options = options or {}
    check_options(options)
    local format = options.format or "%H:%M"

    block {
        name = "clock",
        key = options.key,
        order = options.order or 0,
        interval = options.interval or (format:find("%%S") and 1 or 60),
        update = function(ctx)
            ctx:set { full_text = os.date(format) }
        end,
    }
end
