local menu

block {
    name = "menu",
    update = function(ctx)
        ctx:set { full_text = "ready" }
    end,
    click = function(ctx, button, _event)
        if button ~= 1 then
            return
        end
        assert(menu == nil)
        local cancelled = assert(ctx:spawn({
            argv = { "rofi", "--cancelled" },
            stdout_limit = 16,
        }, function()
            error("cancelled spawn invoked its callback")
        end))
        cancelled:cancel()
        menu = assert(ctx:spawn({
            argv = { "rofi", "-dmenu", "-p", "Test menu", "" },
            stdin = "first\nsecond\n",
            stdout_limit = 1024,
        }, function(menu_ctx, result)
            menu = nil
            assert(result.success)
            assert(result.exit_status == 0)
            assert(result.signal == nil)
            assert(not result.overflow)
            assert(not result.io_error)
            menu_ctx:set { full_text = result.stdout:gsub("[\r\n]+$", "") }
        end))
    end,
}
