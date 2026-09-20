local match_handle
local call_handle
local marshal_handle
local connect_handle
local ffi = require("ffi")

assert(i3sd.has_feature("dbus"))
assert(i3sd.features().dbus)

block {
    name = "dbus_test",
    init = function(ctx)
        local bus = ctx:dbus("user")
        local connected = false
        local match_ready = false
        local call_ready = false
        local marshal_ready = false

        local function publish_ready()
            if connected and match_ready and call_ready and marshal_ready then
                ctx:set { full_text = "dbus-ready" }
            end
        end

        connect_handle = bus:on_connect(function()
            connected = true
            publish_ready()
        end)
        match_handle = bus:match({
            path = "/org/i3sd/Test",
            interface = "org.i3sd.Test",
            member = "Changed",
        }, function(message)
            assert(message:path() == "/org/i3sd/Test")
            assert(message:interface() == "org.i3sd.Test")
            assert(message:member() == "Changed")
            local entries = message:read("a{sv}")
            local values = {}
            for _, entry in ipairs(entries) do
                values[entry[1]] = entry[2]:value()
            end
            ctx:set { full_text = values.greeting .. ":" .. tostring(values.count) }
        end, function(error)
            assert(error == nil, error and error.message)
            match_ready = true
            publish_ready()
        end)
        call_handle = bus:call({
            destination = "org.freedesktop.DBus",
            path = "/org/freedesktop/DBus",
            interface = "org.freedesktop.DBus",
            member = "ListNames",
            signature = "",
            args = {},
            timeout = 2,
        }, function(reply, error)
            assert(error == nil, error and error.message)
            local names = reply:read("as")
            assert(type(names) == "table" and #names >= 1)
            call_ready = true
            publish_ready()
        end)
        marshal_handle = bus:call({
            destination = "org.freedesktop.DBus",
            path = "/org/freedesktop/DBus",
            interface = "org.freedesktop.DBus",
            member = "NoSuchMethod",
            signature = "a{sv}(su)xtay",
            args = {
                i3sd.dbus.dict {
                    { "answer", i3sd.dbus.variant("u", 42) },
                },
                { "value", 7 },
                ffi.new("int64_t", -9007199254740993LL),
                ffi.new("uint64_t", 9007199254740993ULL),
                "binary\0bytes",
            },
            timeout = 2,
        }, function(_reply, error)
            assert(error ~= nil and error.code == "dbus_error", error and (error.code .. ": " .. error.message))
            marshal_ready = true
            publish_ready()
        end)
    end,
}
