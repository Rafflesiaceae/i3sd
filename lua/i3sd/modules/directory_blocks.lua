-- Render every regular file of a directory as one bar item, so external
-- processes can add, update and remove items by managing plain files. The
-- directory is watched natively; no Lua runs when files change.
local allowed = {
    path = true,
    name = true,
    key = true,
    order = true,
    max_items = true,
    max_bytes = true,
    click = true,
}

local function default_path()
    local runtime = os.getenv("XDG_RUNTIME_DIR")
    assert(runtime ~= nil and runtime ~= "",
        "directory_blocks requires a path when XDG_RUNTIME_DIR is unset")
    -- A single component below the runtime directory, because only the final
    -- directory is created automatically.
    return runtime .. "/i3sd-blocks"
end

return function(options)
    options = options or {}
    for key in pairs(options) do
        assert(allowed[key],
            "unknown directory_blocks option '" .. tostring(key) .. "'")
    end
    assert(options.click == nil or type(options.click) == "function",
        "directory_blocks click must be a function")

    local path = options.path or default_path()

    block {
        name = options.name or "directory_blocks",
        key = options.key,
        order = options.order or 0,
        max_items = options.max_items or 16,

        init = function(ctx)
            ctx:_watch_directory {
                path = path,
                max_bytes = options.max_bytes,
            }
        end,

        -- Receives the clicked file name as the fourth argument.
        click = options.click,
    }
end
