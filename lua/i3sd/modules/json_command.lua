local allowed = {
    command = true,
    interval = true,
    format = true,
    order = true,
    key = true,
}

local function check_options(options)
    for key in pairs(options) do
        assert(allowed[key], "unknown json_command option '" .. tostring(key) .. "'")
    end

    assert(type(options.command) == "table",
        "json_command command must be an argv table")
    assert(#options.command > 0,
        "json_command command must not be empty")
    assert(type(options.interval) == "number" and options.interval > 0,
        "json_command interval must be a positive number")
    assert(type(options.format) == "function",
        "json_command format must be a function")
end

local function debug_log(format, ...)
    if os.getenv("DEBUG") ~= nil then
        io.stderr:write(
            "i3sd json_command: ",
            format:format(...),
            "\n"
        )
    end
end

return function(options)
    options = options or {}
    check_options(options)

    local command = options.command
    local interval = options.interval
    local formatter = options.format

    block {
        name = "json_command",
        key = options.key,
        order = options.order or 0,
        interval = interval,

        update = function(ctx)
            local handle = ctx:spawn({
                argv = command,
            }, function(result_ctx, result)
                if not result.success then
                    debug_log(
                        "%s failed: exit=%s signal=%s overflow=%s io_error=%s",
                        tostring(command[1]),
                        tostring(result.exit_status),
                        tostring(result.signal),
                        tostring(result.overflow),
                        tostring(result.io_error)
                    )

                    result_ctx:set { full_text = "" }
                    return
                end

                local value, err = i3sd.json_decode(result.stdout)
                if value == nil then
                    debug_log(
                        "%s returned invalid JSON: %s",
                        tostring(command[1]),
                        tostring(err)
                    )

                    result_ctx:set { full_text = "" }
                    return
                end

                local rendered = formatter(value)

                if rendered == nil or rendered == false then
                    result_ctx:set { full_text = "" }
                elseif type(rendered) == "string" then
                    result_ctx:set { full_text = rendered }
                else
                    result_ctx:set(rendered)
                end
            end)

            -- This is normal during config staging or while another asynchronous
            -- command owns the process slot. The next interval will retry.
            if handle == nil then
                debug_log("%s spawn deferred", tostring(command[1]))
            end
        end,
    }
end
