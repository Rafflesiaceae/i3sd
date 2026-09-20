local clock = require("i3sd.modules.clock")
local cpu = require("i3sd.modules.cpu")
local filesystem = require("i3sd.modules.filesystem")
local memory = require("i3sd.modules.memory")

-- Persistent compact activity and time sit at the right side of the bar.
clock {
    format = "%Y-%m-%d %H:%M",
    order = 10,
}

cpu {
    interval = 2,
    order = 20,
    format = function(value)
        return ("CPU %.0f%%"):format(value.percent)
    end,
}

-- Health blocks remain hidden until action may be useful.
memory {
    interval = 10,
    warn_below = 20,
    critical_below = 10,
    hysteresis = 2,
    order = 80,
}

filesystem {
    path = "/",
    interval = 60,
    warn_below = 20,
    critical_below = 10,
    hysteresis = 2,
    order = 90,
}
