local nvidia = require("i3sd.modules.nvidia")

nvidia {
    index = 0,
    interval = 5,
    format = function(value)
        return ("GPU %s %.0f%% %.1fW"):format(
            value.name or value.uuid,
            value.load_percent or 0,
            value.power_watts or 0
        )
    end,
}
