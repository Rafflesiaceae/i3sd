assert(i3sd.has_feature("json"))
assert(i3sd.features().json)

local decoded, decode_error = i3sd.json_decode(
    '{"name":"codex","count":0,"ok":true,"nothing":null,"items":[1,"two"]}'
)
assert(decoded ~= nil, decode_error)
assert(decoded.name == "codex")
assert(tonumber(decoded.count) == 0)
assert(decoded.ok == true)
assert(decoded.nothing == i3sd.json_null)
assert(tonumber(decoded.items[1]) == 1)
assert(decoded.items[2] == "two")

local invalid, invalid_error = i3sd.json_decode("{")
assert(invalid == nil)
assert(type(invalid_error) == "string")

block {
    name = "first",
    order = 1,
    update = function(ctx)
        -- The power-supply type filter keeps only matching supplies; machines
        -- without power supplies report an error value instead.
        local ok, err = pcall(ctx.sample, ctx, "power_supply", { type = 1 })
        assert(not ok and err:find("type must be a string", 1, true), err)
        local supplies = ctx:sample("power_supply", { type = "Battery" })
        for _, supply in ipairs(supplies and supplies.supplies or {}) do
            assert(supply.type == "Battery", supply.name)
        end

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
