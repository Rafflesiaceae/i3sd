local allowed = {
    command = true,
    interval = true,
    format = true,
    order = true,
    key = true,
}

local STARTUP_RETRY_SECONDS = 0.1

local function check_options(options)
    for key in pairs(options) do
        assert(allowed[key],
            "unknown json_command option '" .. tostring(key) .. "'")
    end

    assert(type(options.command) == "table" and #options.command > 0,
        "json_command command must be a non-empty argv table")
    assert(type(options.interval) == "number" and options.interval > 0,
        "json_command interval must be a positive number")
    assert(type(options.format) == "function",
        "json_command format must be a function")
end

local function command_error(result)
    if result.overflow then
        return "output limit exceeded"
    elseif result.io_error then
        return "I/O error"
    elseif result.signal ~= nil then
        return ("terminated by signal %d"):format(result.signal)
    elseif result.exit_status ~= nil then
        return ("exited with status %d"):format(result.exit_status)
    end

    return "command failed"
end

local function render(ctx, value)
    if value == nil or value == false then
        ctx:set { full_text = "" }
    elseif type(value) == "string" then
        ctx:set { full_text = value }
    elseif type(value) == "table" then
        ctx:set(value)
    else
        error("json_command format must return a string, table, nil, or false")
    end
end

return function(options)
    options = options or {}
    check_options(options)

    local label = options.key or "json_command"
    local startup_retry

    local function show_error(ctx, message)
        ctx:set {
            full_text = ("⚠ %s: %s"):format(label, message),
            color = "#ff0000",
            urgent = true,
        }
    end

    local function complete(ctx, result)
        if not result.success then
            show_error(ctx, command_error(result))
            return
        end

        local value, err = i3sd.json_decode(result.stdout)
        if value == nil then
            show_error(ctx, err or "invalid JSON")
            return
        end

        render(ctx, options.format(value))
    end

    local function poll(ctx)
        local handle = ctx:spawn({
            argv = options.command,
        }, complete)

        if handle ~= nil and startup_retry ~= nil then
            startup_retry:cancel()
            startup_retry = nil
        end
    end

    block {
        name = "json_command",
        key = options.key,
        order = options.order or 0,
        interval = options.interval,

        init = function(ctx)
            startup_retry = ctx:every(STARTUP_RETRY_SECONDS, poll)
        end,

        update = poll,
    }
end
