local allowed = {
    order = true,
    key = true,
}

local levels = { "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█" }

local function check_options(options)
    for key in pairs(options) do
        assert(allowed[key], "unknown pipewire_volume option '" .. tostring(key) .. "'")
    end
end

local function volume_glyph(volume)
    local bounded = math.max(0, math.min(1, volume))
    local index = math.max(1, math.ceil(bounded * #levels))
    return levels[index]
end

return function(options)
    options = options or {}
    check_options(options)
    if not i3sd.has_feature("pipewire") then
        block {
            name = "pipewire_volume",
            key = options.key,
            order = options.order or 0,
        }
        return
    end
    local handle

    block {
        name = "pipewire_volume",
        key = options.key,
        order = options.order or 0,
        init = function(ctx)
            handle = ctx:_watch_pipewire_volume(function(event_ctx, value)
                if not value.available then
                    event_ctx:set { full_text = "" }
                elseif value.muted then
                    event_ctx:set { full_text = "🔇" }
                else
                    event_ctx:set { full_text = volume_glyph(value.volume) }
                end
            end)
        end,
    }
end
