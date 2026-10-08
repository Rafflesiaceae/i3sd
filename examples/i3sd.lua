local clock = require("i3sd.modules.clock")
local filesystem = require("i3sd.modules.filesystem")
local memory = require("i3sd.modules.memory")
local pipewire_volume = require("i3sd.modules.pipewire_volume")
local power_profiles = require("i3sd.modules.power_profiles")
local systemd = require("i3sd.modules.systemd")
local nvidia = require("i3sd.modules.nvidia")
local json_command = require("i3sd.modules.json_command")

--- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---

local color_bad = "#FF4535"
local color_bad_very = "#FF0000"
local color_degraded = "#FDD102"
local color_dim = "#99947B"
local color_normal = "#F8F8F2"

local function color_gradient(value, min, max, color_min, color_max)
	local function rgb(color)
		return tonumber(color:sub(2, 3), 16), tonumber(color:sub(4, 5), 16), tonumber(color:sub(6, 7), 16)
	end

	local t = math.max(0, math.min(1, (value - min) / (max - min)))
	local r1, g1, b1 = rgb(color_min)
	local r2, g2, b2 = rgb(color_max)

	return ("#%02X%02X%02X"):format(r1 + (r2 - r1) * t, g1 + (g2 - g1) * t, b1 + (b2 - b1) * t)
end

local function color_gradient3(value, min, middle, max, color_left, color_middle, color_right)
	if value <= middle then
		return color_gradient(value, min, middle, color_left, color_middle)
	else
		return color_gradient(value, middle, max, color_middle, color_right)
	end
end

local function color_gradient4(
	value,
	min,
	middle_left,
	middle_right,
	max,
	color_left,
	color_middle_left,
	color_middle_right,
	color_right
)
	if value <= middle_left then
		return color_gradient(value, min, middle_left, color_left, color_middle_left)
	elseif value <= middle_right then
		return color_gradient(value, middle_left, middle_right, color_middle_left, color_middle_right)
	else
		return color_gradient(value, middle_right, max, color_middle_right, color_right)
	end
end

--- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---

i3sd.configure {
    click_events = false,
    debug = false,
}

-- datetime
clock({
	format = "📰 %Y-%m-%d  🕓 %H:%M:%S ·",
	interval = 2,
})

block({
	name = "cpu-load",
	interval = 2,
	update = function(ctx)
		local postfix = "  "
		local snapshot = assert(ctx:sample("load"))
		local color =
			color_gradient4(snapshot.load1, 1.0, 2.0, 6.0, 12.0, color_dim, color_normal, color_degraded, color_bad)
		ctx:set({
			full_text = ("🔲 <span foreground='%s'>%05.2f</span>" .. postfix):format(color, snapshot.load1),
			markup = "pango",
		})
	end,
})

-- RAM
memory({
	interval = 2,
	warn_below = 20,
	critical_below = 10,
	hysteresis = 2,
	format = function(value)
		local pct = tonumber(value.available_percent)
		local color = color_gradient3(pct, 12, 20, 50, color_bad, color_degraded, color_normal)
		return {
			full_text = ("📛 RAM: %.0f%% "):format(pct),
			color = color,
			urgent = value.severity == "critical",
		}
	end,
})

-- disk
filesystem({
	path = "/",
	interval = 2,
	warn_below = 20,
	critical_below = 10,
	hysteresis = 1,
	format = function(value)
		return {
			full_text = ("💾 %.0f%% "):format(tonumber(value.available_percent)),
			color = color_bad,
			urgent = value.severity == "critical",
		}
	end,
})

-- gpu
nvidia({
	index = 0,
	interval = 5,
	format = function(value)
		local load = math.min(tonumber(value.load_percent) or 0, 99)

		local color = color_gradient3(load, 20, 60, 99, color_dim, color_degraded, color_bad)

		return {
			full_text = ("  🔘 <span foreground='%s'>%02.0f%%</span> "):format(color, load),
			markup = "pango",
		}
	end,
})

power_profiles({
	format = function(value)
		local labels = {
			["power-saver"] = { "   😴   ", "" },
			balanced = { "   ➗   ", "" },
			performance = { "   🎯   ", "" },
		}
		return {
			full_text = ("<span foreground='%s'>%s</span>"):format(color_dim, labels[value.active_profile][1]),
			markup = "pango",
			background = "#272822",
		}
	end,
})

-- llm/codex
json_command({
	command = { "codex-usage", "--json" },
	interval = 30,
	format = function(value)
		local result = ("🍚 %.0f%% %s/%.0f%% %s"):format(
			tonumber(value["5h"]) or 0,
			value["5h_reset_local"],
			tonumber(value.week) or 0,
			value["week_reset_local"]
		)

		if value.credits > 0 then
			result = result .. (" · credits %g"):format(tonumber(value.credits) or 0)
		end

		return {
			full_text = result .. "  ",
			markup = "pango",
		}
	end,
})

systemd({
	scope = "both",
	format = function(value)
		return {
			full_text = ("systemd: %d failed"):format(value.count),
			color = color_bad_very,
			urgent = true,
		}
	end,
})
