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

-- Process-wide child limits: timeouts kill the whole process group, missing
-- executables complete asynchronously and at most eight children run.
local limits_started = false
local outcomes = {}

local function record(ctx, name)
    outcomes[#outcomes + 1] = name
    if #outcomes == 3 then
        table.sort(outcomes)
        ctx:set { full_text = "limits:" .. table.concat(outcomes, ",") }
    end
end

block {
    name = "limits",
    update = function(ctx)
        if limits_started then
            return
        end
        -- The grandchild keeps stdout open, so completion relies on the
        -- timeout closing the pipe and killing the group.
        local slow = ctx:spawn({
            argv = { "sh", "-c", "sleep 37.25 & exec sleep 37.25" },
            timeout = 0.2,
        }, function(result_ctx, result)
            assert(result.timed_out)
            assert(not result.success)
            assert(result.signal == 9, tostring(result.signal))
            record(result_ctx, "timeout")
        end)
        if slow == nil then
            -- Staging never starts children.
            return
        end
        limits_started = true
        -- Timers created by a live callback are armed immediately.
        ctx:after(0.05, function(timer_ctx)
            record(timer_ctx, "after")
        end)
        assert(ctx:spawn({
            argv = { "i3sd-test-missing-command" },
        }, function(result_ctx, result)
            assert(result.exit_status == 127)
            assert(not result.success and not result.timed_out)
            record(result_ctx, "missing")
        end))
        local fillers = {}
        for index = 1, 6 do
            fillers[index] = assert(ctx:spawn({ argv = { "sleep", "30" } },
                function()
                    error("cancelled filler invoked its callback")
                end))
        end
        assert(ctx:spawn({ argv = { "true" } }, function() end) == nil,
            "a ninth child started")
        for _, filler in ipairs(fillers) do
            filler:cancel()
        end
        local ok, err = pcall(ctx.spawn, ctx, { argv = { "true" }, timeout = 0 },
            function() end)
        assert(not ok and err:find("positive number", 1, true), err)

        -- Argument errors must spell out their numbers.
        local function expect_error(request, message)
            local spawned, failure = pcall(ctx.spawn, ctx, request, function() end)
            assert(not spawned and failure:find(message, 1, true), failure)
        end
        local oversized = {}
        for index = 1, 65 do
            oversized[index] = "true"
        end
        expect_error({ argv = oversized }, "must contain 1 to 64 entries")
        expect_error({ argv = { "true", 5 } }, "argv entry 2 must be a string")
        expect_error({ argv = { "true", ("x"):rep(4097) } }, "at most 4096 bytes")
        expect_error({ argv = { "true" }, stdout_limit = 0 }, "from 1 to 65536")
    end,
}
