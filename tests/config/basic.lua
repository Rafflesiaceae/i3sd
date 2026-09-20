block {
    name = "first",
    order = 1,
    update = function(ctx)
        local memory = assert(ctx:sample("memory"))
        ctx:set {
            full_text = ("memory:%d"):format(memory.mem_total),
            color = "#ffffff",
        }
    end,
}

block {
    name = "second",
    key = "timer",
    order = 2,
    interval = 60,
    init = function(ctx)
        -- Staging validates spawn requests without starting external programs.
        assert(ctx:spawn({
            argv = { "true" },
            stdin = "ignored",
            stdout_limit = 16,
        }, function(_menu_ctx, _selected)
        end) == nil)
        ctx:after(10, function(inner_ctx)
            inner_ctx:set { full_text = "late" }
        end)
    end,
    update = function(ctx)
        ctx:set { full_text = "ready" }
    end,
}
