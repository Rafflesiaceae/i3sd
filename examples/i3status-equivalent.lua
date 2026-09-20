local clock = require("i3sd.modules.clock")
local filesystem = require("i3sd.modules.filesystem")
local memory = require("i3sd.modules.memory")
local systemd = require("i3sd.modules.systemd")

local color_bad = "#FF0000"
local color_degraded = "#FDD102"

-- Failed system and user units are event-driven and hidden while healthy.
systemd {
    scope = "both",
    order = 50,
    format = function(value)
        return {
            full_text = ("systemd: %d failed"):format(value.count),
            color = color_bad,
            urgent = true,
        }
    end,
}

-- This reproduces the active path_exists VPN block through the generic local
-- file metadata collector. It does not add a network-status backend to i3sd.
block {
    name = "VPN",
    order = 50,
    interval = 2,
    update = function(ctx)
        local snapshot = assert(ctx:sample("file_stat", {
            path = "/proc/sys/net/ipv4/conf/tun0",
        }))
        if not snapshot.exists then
            ctx:set { full_text = "" }
            return
        end
        ctx:set {
            full_text = " <span foreground='#B3B3B3'>VPN</span> ",
            markup = "pango",
        }
    end,
}

-- The original root disk block is warning-only below ten percent available.
filesystem {
    path = "/",
    interval = 2,
    warn_below = 10,
    critical_below = 10,
    hysteresis = 1,
    order = 40,
    format = function(value)
        return {
            full_text = ("🧱 Disk: %.0f%%"):format(value.available_percent),
            color = color_bad,
        }
    end,
}

-- Memory stays hidden while healthy and escalates at the original 20/10
-- percent available thresholds.
memory {
    interval = 2,
    warn_below = 20,
    critical_below = 10,
    hysteresis = 2,
    order = 30,
    format = function(value)
        return {
            full_text = ("RAM: %.0f%%"):format(value.available_percent),
            color = value.severity == "critical" and color_bad or color_degraded,
            urgent = value.severity == "critical",
        }
    end,
}

-- Match the old load presentation: show load1 normally and load5 in yellow
-- once the one-minute value crosses 2.0.
block {
    name = "load",
    order = 20,
    interval = 2,
    update = function(ctx)
        local snapshot = assert(ctx:sample("load"))
        if snapshot.load1 > 2.0 then
            ctx:set {
                full_text = (" 🔲 <span foreground='%s'>%.2f</span>"):format(color_degraded, snapshot.load5),
                markup = "pango",
            }
            return
        end
        ctx:set {
            full_text = (" 🔲 %.2f"):format(snapshot.load1),
            markup = "pango",
        }
    end,
}

clock {
    format = "   📰 %Y-%m-%d  🕓 %H:%M:%S  ",
    interval = 2,
    order = 10,
}

-- Not represented here yet: read_file content, battery, cpu_temperature, and
-- volume need APIs/modules not present in this implementation. The commented
-- wireless and Ethernet blocks are outside i3sd's network-status scope.
