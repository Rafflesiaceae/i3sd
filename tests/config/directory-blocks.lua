local directory_blocks = require("i3sd.modules.directory_blocks")
local path = assert(os.getenv("I3SD_TEST_BLOCKS"))

directory_blocks {
    path = path,
    max_items = 3,
    click = function(_ctx, button, _event, name)
        -- Middle click dismisses an item by removing its file.
        if button == 2 then
            assert(os.remove(path .. "/" .. name))
        end
    end,
}

-- A directory feed owns its block's items exclusively.
block {
    name = "guard",
    init = function(ctx)
        local ok, err = pcall(ctx._watch_directory, ctx, { path = "relative" })
        assert(not ok and err:find("absolute", 1, true), err)
        ctx:_watch_directory { path = path .. "-guard" }
        ok, err = pcall(ctx.set, ctx, { full_text = "x" })
        assert(not ok and err:find("come from a directory watch", 1, true), err)
        ok, err = pcall(ctx._watch_directory, ctx, { path = path })
        assert(not ok and err:find("already watches", 1, true), err)
    end,
}
