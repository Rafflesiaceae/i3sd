local allowed = {
    scope = true,
    format = true,
    order = true,
    key = true,
}

local function check_options(options)
    for key in pairs(options) do
        assert(allowed[key], "unknown systemd option '" .. tostring(key) .. "'")
    end
    assert(
        options.scope == nil or options.scope == "system" or options.scope == "user" or options.scope == "both",
        "systemd scope must be system, user, or both"
    )
    assert(options.format == nil or type(options.format) == "function", "systemd format must be a function")
end

return function(options)
    options = options or {}
    check_options(options)
    local scope = options.scope or "system"
    local handle
    local formatter = options.format or function(value)
        return {
            full_text = ("systemd: %d failed"):format(value.count),
            color = "#ff0000",
            urgent = true,
        }
    end

    block {
        name = "systemd",
        key = options.key or scope,
        order = options.order or 0,
        init = function(ctx)
            handle = ctx:_watch_systemd_failed(scope, function(event_ctx, value)
                if value.count == 0 then
                    event_ctx:set { full_text = "" }
                    return
                end
                local rendered = formatter(value)
                if rendered == nil or rendered == false then
                    event_ctx:set { full_text = "" }
                elseif type(rendered) == "string" then
                    event_ctx:set { full_text = rendered }
                else
                    event_ctx:set(rendered)
                end
            end)
        end,
    }
end
