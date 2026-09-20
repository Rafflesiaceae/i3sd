block {
    name = "menu",
    update = function(ctx)
        ctx:set { full_text = "ready" }
    end,
    click = function(ctx, button, _event)
        if button ~= 1 then
            return
        end
        assert(ctx:rofi({
            prompt = "Test menu",
            choices = { "first", "second" },
        }, function(menu_ctx, selected)
            menu_ctx:set { full_text = selected or "cancelled" }
        end))
    end,
}
