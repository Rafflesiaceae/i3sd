local allowed = {
    format = true,
    order = true,
    key = true,
}

local function check_options(options)
    for key in pairs(options) do
        assert(allowed[key], "unknown power_profiles option '" .. tostring(key) .. "'")
    end
    assert(options.format == nil or type(options.format) == "function", "power_profiles format must be a function")
end

return function(options)
    options = options or {}
    check_options(options)
    local handle
    local formatter = options.format or function(value)
        return "power: " .. value.active_profile
    end

    block {
        name = "power_profiles",
        key = options.key,
        order = options.order or 0,
        init = function(ctx)
            handle = ctx:_watch_power_profiles(function(event_ctx, value)
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
        click = function(ctx, button, _event)
            if button == 1 then
                ctx:_show_power_profiles_menu()
            end
        end,
    }
end
