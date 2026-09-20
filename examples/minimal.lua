-- A polling block needs only a stable identity, interval, and update callback.
block {
    name = "clock",
    order = 10,
    interval = 1,
    update = function(ctx)
        ctx:set {
            full_text = os.date("%H:%M:%S"),
            color = "#ffffff",
        }
    end,
}

-- This warning is hidden while at least 10 percent of memory is available.
block {
    name = "memory",
    order = 20,
    interval = 10,
    update = function(ctx)
        local sample, err = ctx:sample("memory")
        if sample == nil then
            ctx:set { full_text = "memory unavailable", color = "#ff0000" }
            return
        end

        local available = 100 * sample.mem_available / sample.mem_total
        if available > 10 then
            ctx:set { full_text = "" }
            return
        end
        ctx:set {
            full_text = ("MEM %.0f%% free"):format(available),
            color = "#ff0000",
            urgent = true,
        }
    end,
}
