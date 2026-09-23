local allowed = {
    index = true,
    interval = true,
    format = true,
    order = true,
    key = true,
}

local function check_options(options)
    for key in pairs(options) do
        assert(allowed[key], "unknown nvidia option '" .. tostring(key) .. "'")
    end
    assert(options.index == nil or type(options.index) == "number", "nvidia index must be a number")
    assert(options.interval == nil or type(options.interval) == "number", "nvidia interval must be a number")
    assert(options.format == nil or type(options.format) == "function", "nvidia format must be a function")
end

return function(options)
    options = options or {}
    check_options(options)
    local index = options.index or 0
    local interval = options.interval or 2
    assert(index >= 0 and index % 1 == 0, "nvidia index must be a non-negative integer")
    assert(interval > 0, "nvidia interval must be positive")

    if not i3sd.has_feature("nvidia") then
        block {
            name = "nvidia",
            key = options.key or tostring(index),
            order = options.order or 0,
        }
        return
    end

    local formatter = options.format or function(value)
        local metrics = {}
        if value.load_percent ~= nil then
            metrics[#metrics + 1] = ("%.0f%%"):format(value.load_percent)
        end
        if value.power_watts ~= nil then
            metrics[#metrics + 1] = ("%.0fW"):format(value.power_watts)
        end
        if value.temperature_celsius ~= nil then
            metrics[#metrics + 1] = ("%.0fC"):format(value.temperature_celsius)
        end
        if #metrics == 0 then
            return nil
        end
        return "GPU " .. table.concat(metrics, " ")
    end

    block {
        name = "nvidia",
        key = options.key or tostring(index),
        order = options.order or 0,
        interval = interval,
        update = function(ctx)
            local snapshot = ctx:sample("nvidia", { index = index })
            if snapshot == nil then
                ctx:set { full_text = "" }
                return
            end

            -- Convert NVML's milliwatt unit for convenient status formatting.
            local value = {
                index = snapshot.index,
                uuid = snapshot.uuid,
                name = snapshot.name,
                load_percent = snapshot.gpu_utilization_percent,
                memory_load_percent = snapshot.memory_utilization_percent,
                memory_used_bytes = snapshot.memory_used_bytes,
                memory_total_bytes = snapshot.memory_total_bytes,
                memory_free_bytes = snapshot.memory_free_bytes,
                power_watts = snapshot.power_mw == nil and nil or snapshot.power_mw / 1000,
                temperature_celsius = snapshot.temperature_celsius,
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
