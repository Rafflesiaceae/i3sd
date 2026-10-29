local function rejects(ctx, value, pattern)
    local ok, err = pcall(ctx.set, ctx, value)
    assert(not ok, "ctx:set unexpectedly accepted an invalid value")
    assert(tostring(err):find(pattern, 1, true), tostring(err))
end

local function render(ctx, clicked)
    ctx:set {
        { id = "vpn", full_text = "vpn" },
        { id = "mail", full_text = clicked or "mail" },
        { full_text = "anonymous" },
        { id = "hidden", full_text = "" },
    }
end

block {
    name = "list",
    max_items = 4,
    init = function(ctx)
        rejects(ctx, {
            { id = "same", full_text = "a" },
            { id = "same", full_text = "b" },
        }, "item 2: duplicate id 'same'")
        rejects(ctx, {
            { full_text = "1" }, { full_text = "2" }, { full_text = "3" },
            { full_text = "4" }, { full_text = "5" },
        }, "renders at most 4 items")
        rejects(ctx, { [1] = { full_text = "a" }, [3] = { full_text = "c" } },
            "sequences of state tables")
        rejects(ctx, { { full_text = "a" }, full_text = "b" },
            "sequences of state tables")
        rejects(ctx, { { full_text = "a" }, "b" }, "item 2: must be a state")
        rejects(ctx, { { full_text = "a", color = "red" } },
            "item 1: color fields")
        rejects(ctx, { { full_text = "a", id = 7 } }, "id must be a string")
        rejects(ctx, { { full_text = "a", id = "" } }, "id must be non-empty")
        rejects(ctx, { { full_text = "a", unknown = true } },
            "unknown option 'unknown'")
        rejects(ctx, { full_text = "a", id = "single" },
            "unknown option 'id'")
    end,
    update = function(ctx)
        render(ctx)
    end,
    click = function(ctx, button, _event, id)
        assert(button == 1)
        -- A failing update must leave the rendered items untouched.
        rejects(ctx, {
            { id = "vpn", full_text = "broken" },
            { id = "vpn", full_text = "duplicate" },
        }, "duplicate id")
        render(ctx, "clicked:" .. tostring(id))
    end,
}

block {
    name = "single",
    update = function(ctx)
        ctx:set { full_text = 42 }
    end,
    click = function(ctx, _button, _event, id)
        assert(id == nil)
        ctx:set { full_text = "single-clicked" }
    end,
}
