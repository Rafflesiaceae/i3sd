i3sd.configure {
    click_events = false,
    debug = true,
}

block {
    name = "settings",
    update = function(ctx)
        ctx:set { full_text = "settings" }
    end,
}
