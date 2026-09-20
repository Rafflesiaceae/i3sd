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

local function selected_profile(output, profiles)
    -- Rofi terminates its selected row with newlines; embedded line breaks
    -- indicate output that cannot name one advertised profile.
    local selected = output:gsub("[\r\n]+$", "")
    if selected == "" or selected:find("[\r\n]") then
        return nil
    end
    for _, profile in ipairs(profiles) do
        if selected == profile then
            return profile
        end
    end
    return nil
end

return function(options)
    options = options or {}
    check_options(options)
    local handle
    local menu
    local profiles = {}
    local formatter = options.format or function(value)
        return "power: " .. value.active_profile
    end

    block {
        name = "power_profiles",
        key = options.key,
        order = options.order or 0,
        init = function(ctx)
            handle = ctx:_watch_power_profiles(function(event_ctx, value)
                profiles = value.profiles
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
            if button ~= 1 or #profiles == 0 then
                return
            end
            if menu ~= nil then
                return
            end
            -- Rofi policy stays in Lua while the core owns only the generic
            -- bounded child process and its event-loop integration.
            local input = table.concat(profiles, "\n") .. "\n"
            menu = ctx:spawn({
                argv = { "rofi", "-dmenu", "-p", "Power profile" },
                stdin = input,
                stdout_limit = 1024,
            }, function(menu_ctx, result)
                menu = nil
                local selected
                if result.success then
                    selected = selected_profile(result.stdout, profiles)
                end
                if selected ~= nil then
                    menu_ctx:_set_power_profile(selected)
                end
            end)
        end,
    }
end
