local allowed = {
    show = true,
    low_below = true,
    critical_below = true,
    interval = true,
    format = true,
    order = true,
    key = true,
}

local function check_options(options)
    for key in pairs(options) do
        assert(allowed[key], "unknown battery option '" .. tostring(key) .. "'")
    end
    assert(options.show == nil or options.show == "auto" or options.show == "always", "battery show must be auto or always")
    assert(options.format == nil or type(options.format) == "function", "battery format must be a function")
    assert(options.interval == nil or type(options.interval) == "number", "battery interval must be a number")
    assert(options.low_below == nil or type(options.low_below) == "number", "battery low_below must be a number")
    assert(options.critical_below == nil or type(options.critical_below) == "number", "battery critical_below must be a number")
end

local function numeric(value)
    if value == nil then
        return nil
    end
    return tonumber(value)
end

local function percentage(now, full)
    now = numeric(now)
    full = numeric(full)
    if now == nil or full == nil or now < 0 or full <= 0 then
        return nil
    end
    return math.max(0, math.min(100, 100 * now / full))
end

local function remaining_minutes(battery)
    local status = battery.status
    local now, full, rate
    if status == "Discharging" then
        now = numeric(battery.energy_now_uwh)
        rate = numeric(battery.power_now_uw)
        if now == nil or rate == nil then
            now = numeric(battery.charge_now_uah)
            rate = numeric(battery.current_now_ua)
        end
    elseif status == "Charging" then
        now = numeric(battery.energy_now_uwh)
        full = numeric(battery.energy_full_uwh)
        rate = numeric(battery.power_now_uw)
        if now == nil or full == nil or rate == nil then
            now = numeric(battery.charge_now_uah)
            full = numeric(battery.charge_full_uah)
            rate = numeric(battery.current_now_ua)
        end
        if now ~= nil and full ~= nil then
            now = math.max(0, full - now)
        end
    end
    if now == nil or rate == nil or rate <= 0 then
        return nil
    end
    return 60 * now / rate
end

local function format_remaining(minutes)
    if minutes == nil then
        return ""
    end
    local rounded = math.floor(minutes + 0.5)
    if rounded >= 60 then
        return (", %dh %02dm"):format(math.floor(rounded / 60), rounded % 60)
    end
    return (", %dm"):format(rounded)
end

return function(options)
    options = options or {}
    check_options(options)
    local show = options.show or "auto"
    local low_below = options.low_below or 40
    local critical_below = options.critical_below or 15
    local interval = options.interval or 30
    assert(interval > 0, "battery interval must be positive")
    assert(critical_below >= 0 and critical_below <= low_below and low_below <= 100, "invalid battery thresholds")

    local formatter = options.format or function(value)
        local charge = value.percent == nil and "" or (" %.0f%%"):format(value.percent)
        local status = value.status == nil and "" or (" %s"):format(value.status:lower())
        return "🔋" .. charge .. status .. format_remaining(value.remaining_minutes)
    end

    block {
        name = "battery",
        key = options.key,
        order = options.order or 0,
        interval = interval,
        update = function(ctx)
            local snapshot = ctx:sample("power_supply")
            if snapshot == nil then
                ctx:set { full_text = "" }
                return
            end
            local battery
            for _, supply in ipairs(snapshot.supplies) do
                if supply.type == "Battery" then
                    battery = supply
                    break
                end
            end
            if battery == nil then
                ctx:set { full_text = "" }
                return
            end

            -- Prefer energy values, then use charge values when the driver lacks energy data.
            local percent = percentage(battery.energy_now_uwh, battery.energy_full_uwh)
                or percentage(battery.charge_now_uah, battery.charge_full_uah)
            local severity
            if percent ~= nil then
                if percent <= critical_below then
                    severity = "critical"
                elseif percent <= low_below then
                    severity = "warning"
                end
            end
            local active_status = battery.status == "Charging" or battery.status == "Discharging"
            -- Auto mode keeps useful charge activity visible and hides a healthy full battery.
            if show == "auto" and not active_status and severity == nil then
                ctx:set { full_text = "" }
                return
            end

            local value = {
                name = battery.name,
                status = battery.status,
                percent = percent,
                remaining_minutes = remaining_minutes(battery),
                severity = severity,
            }
            local rendered = formatter(value)
            if rendered == nil or rendered == false then
                ctx:set { full_text = "" }
            elseif type(rendered) == "string" then
                ctx:set {
                    full_text = rendered,
                    color = severity == "critical" and "#ff0000" or severity == "warning" and "#ffaa00" or nil,
                    urgent = severity == "critical",
                }
            else
                ctx:set(rendered)
            end
        end,
    }
end
