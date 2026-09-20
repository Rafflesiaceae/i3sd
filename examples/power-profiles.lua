local power_profiles = require("i3sd.modules.power_profiles")

-- Left-click the block to select an advertised profile through rofi.
power_profiles {
    order = 20,
    format = function(value)
        local labels = {
            ["power-saver"] = "power save",
            balanced = "balanced",
            performance = "performance",
        }
        return "⚡ " .. (labels[value.active_profile] or value.active_profile)
    end,
}
