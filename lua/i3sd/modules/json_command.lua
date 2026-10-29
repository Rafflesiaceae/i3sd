local allowed = {
    command = true,
    interval = true,
    timeout = true,
    format = true,
    order = true,
    key = true,
}

local RETRY_SECONDS = 0.1
local MAX_NAME_BYTES = 256

-- Each configuration generation loads a fresh module, so names reset on reload.
local used_names = {}

local function command_name(label)
    local base = ("json_command_" .. label):sub(1, MAX_NAME_BYTES)
    local name = base
    local number = 1

    -- Reserve room for the suffix and check it against all previously used names.
    while used_names[name] do
        number = number + 1
        local suffix = "_" .. number
        name = base:sub(1, MAX_NAME_BYTES - #suffix) .. suffix
    end

    used_names[name] = true
    return name
end

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
    assert(options.timeout == nil
        or (type(options.timeout) == "number" and options.timeout > 0),
        "json_command timeout must be a positive number")
end

local function command_error(result)
    if result.timed_out then
        return "timed out"
    elseif result.overflow then
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

    local label = table.concat(options.command, "_"):gsub("[^%w]", "_"):lower()
        :gsub("_+", "_")
    -- A run that outlives its interval is killed instead of piling up.
    local timeout = options.timeout or options.interval
    -- The handle of the run in progress, so polls never overlap.
    local running
    -- A pending retry while no child slot is free, with its backoff delay.
    local retry
    local retry_delay = RETRY_SECONDS

    local function show_error(ctx, message)
        ctx:set {
            full_text = ("⚠ %s: %s"):format(label, message),
            color = "#ff0000",
            urgent = true,
        }
    end

    local function complete(ctx, result)
        running = nil
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

    local poll

    local function retry_poll(ctx)
        retry = nil
        poll(ctx)
    end

    function poll(ctx)
        if running ~= nil then
            return
        end
        running = ctx:spawn({
            argv = options.command,
            timeout = timeout,
        }, complete)
        if running ~= nil then
            retry_delay = RETRY_SECONDS
            return
        end

        -- Staging and busy child slots refuse to spawn; retry soon, but
        -- back off so a lasting shortage never turns into busy polling.
        if retry == nil then
            retry = ctx:after(retry_delay, retry_poll)
            retry_delay = math.min(retry_delay * 2, options.interval)
        end
    end

    block {
        name = command_name(label),
        key = options.key,
        order = options.order or 0,
        interval = options.interval,

        update = poll,
    }
end
