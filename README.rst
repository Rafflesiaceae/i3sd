i3sd
====

``i3sd`` is an event-driven Linux status generator for i3bar. Its native core owns the epoll reactor, bounded protocol buffers, timers, signals, reloads and collectors; presentation policy remains in Lua.

Build
-----

.. code:: console

   meson setup build
   meson compile -C build
   meson test -C build

The required development dependencies are LuaJIT, yyjson, xxHash and libsystemd.
PipeWire development headers are optional and enable the event-driven default
sink volume module. NVML development headers are optional and enable the
NVIDIA GPU module.

Configuration
-------------

The default configuration is ``${XDG_CONFIG_HOME:-$HOME/.config}/i3sd.lua``. A minimal low-level configuration is:

.. code:: lua

   i3sd.configure {
       click_events = false,
       debug = false,
   }

   block {
       name = "clock",
       interval = 1,
       update = function(ctx)
           ctx:set { full_text = os.date("%H:%M:%S") }
       end,
   }

Validate it without activating runtime sources or writing the i3bar protocol:

.. code:: console

   i3sd --check -c ~/.config/i3sd.lua

``i3sd.configure`` accepts the process-wide ``click_events`` and ``debug``
booleans. Click handling defaults to enabled and debug logging defaults to the
``DEBUG`` environment setting. The equivalent command-line switches are
``--click-events`` / ``--no-click-events`` and ``--debug`` / ``--no-debug``;
an explicit command-line switch takes precedence over the configuration and
environment. Because the i3bar protocol header is written only once,
``click_events`` can only change on process startup. Disabling it both advertises
``"click_events":false`` to i3bar and stops i3sd from monitoring stdin.

The high-level Lua modules can be loaded explicitly:

.. code:: lua

   local clock = require("i3sd.modules.clock")
   local battery = require("i3sd.modules.battery")
   local memory = require("i3sd.modules.memory")
   local nvidia = require("i3sd.modules.nvidia")
   local pipewire_volume = require("i3sd.modules.pipewire_volume")
   local power_profiles = require("i3sd.modules.power_profiles")
   local systemd = require("i3sd.modules.systemd")

   clock { format = "%H:%M" }
   battery { show = "auto" }
   memory { warn_below = 20, critical_below = 10 }
   nvidia {}
   pipewire_volume {}
   power_profiles {}
   systemd { scope = "both" }

Copyable configurations are available under ``examples/``. See ``PLAN.rst`` for the complete architecture, API contracts and delivery roadmap.

``pipewire_volume`` follows the current default audio sink and updates without
polling. It displays one of ``▏ ▎ ▍ ▌ ▋ ▊ ▉ █`` for the volume, ``🔇`` while
muted, and remains hidden while PipeWire or a default sink is unavailable.

Besides ``ctx:after(seconds, fn)`` and ``ctx:every(seconds, fn)``, which count
awake monotonic time, blocks can schedule work on the wall clock.
``ctx:at_realtime(unix_seconds, fn)`` runs ``fn`` once when the realtime clock
reaches an absolute timestamp, and ``ctx:on_clock_change(fn)`` runs ``fn`` after
the realtime clock jumped, for example when it was set or the machine resumed
from suspend, so wall-clock schedules can be recomputed. Both share one timerfd
that only exists while they are in use. The ``clock`` module uses them to flip
exactly on the second or minute boundary; its ``interval`` aligns updates to
multiples of that many seconds since the epoch. After a suspend, i3sd also runs
every block's ``update`` once so that values do not stay stale until the next
interval. Keep the returned handles referenced: collecting one cancels it.

Lua modules can start bounded asynchronous child processes from a block
callback, up to eight at a time. ``ctx:spawn`` returns ``nil`` during staging,
when all eight are running, or when process setup fails:

.. code:: lua

   local process = ctx:spawn({
       argv = { "example-command", "--mode", "menu" },
       stdin = "first\nsecond\n",
       stdout_limit = 1024,
       timeout = 10, -- seconds; omit it to let the child run indefinitely
   }, function(process_ctx, result)
       if result.success then
           process_ctx:set { full_text = result.stdout }
       end
   end)

A child that outlives its ``timeout`` is killed together with any processes it
started, and its result has ``timed_out = true``. ``json_command`` uses its
``interval`` as the default ``timeout`` and never overlaps two runs of the same
command. The returned handle has an idempotent ``cancel()`` method. The
``power_profiles`` module uses this primitive to implement its left-click rofi
selector entirely in Lua. Install ``rofi`` when using that module.

Item lists
----------

A block can render a variable number of i3bar objects. Declare how many it may
use with ``max_items`` and pass ``ctx:set`` a sequence of state tables instead
of a single state:

.. code:: lua

   block {
       name = "notifications",
       max_items = 16,
       update = function(ctx)
           ctx:set {
               { id = "vpn", full_text = "VPN up", color = "#00ff00" },
               { id = "mail", full_text = "3 new" },
           }
       end,
       click = function(ctx, button, event, id)
           -- id is "vpn" or "mail" here, and nil for single-state blocks.
       end,
   }

Each call replaces the complete list, which renders left to right in sequence
order, and ``{}`` hides the block. Every entry accepts the same fields as a
single ``ctx:set`` state plus an optional string ``id`` of at most 255 bytes.
Ids must be unique within a list; they keep click routing stable while items
come and go. Entries without an id are passed to ``click`` as their 1-based
position. A click on an item that has disappeared since it was rendered is
dropped.

The core reconciles each list against the previous one: unchanged entries
reuse their cached output without allocating, and only new or changed entries
are validated and serialized. A failing call raises and leaves the rendered
items untouched. All blocks of a configuration together reserve at most 256
items; ``max_items`` defaults to 1.

Directory blocks
----------------

``directory_blocks`` lets external processes add, update and remove bar items
by managing plain files. Every regular file in the directory becomes one item,
ordered by file name:

.. code:: lua

   local directory_blocks = require("i3sd.modules.directory_blocks")
   local path = os.getenv("XDG_RUNTIME_DIR") .. "/i3sd-blocks"

   directory_blocks {
       path = path,       -- this is also the default
       max_items = 16,    -- the lexically first files are shown
       max_bytes = 512,   -- per file, at most 4096
       click = function(ctx, button, event, name)
           if button == 2 then
               os.remove(path .. "/" .. name) -- middle click dismisses
           end
       end,
   }

.. code:: console

   $ touch "$XDG_RUNTIME_DIR/i3sd-blocks/10-vpn"          # shows "10-vpn"
   $ printf 'VPN up\nVPN\ncolor=#00ff00\n' >"$XDG_RUNTIME_DIR/i3sd-blocks/10-vpn"
   $ rm "$XDG_RUNTIME_DIR/i3sd-blocks/10-vpn"             # item disappears

An empty file shows its name. Otherwise the first line is ``full_text`` and
the second is optional ``short_text``; an empty first line hides the item.
The third line contains ``key=value`` options separated by spaces or tabs:

.. code:: text

   <b>VPN disconnected</b>
   <b>VPN!</b>
   color=#ff0000 pango=1 urgent=1

Supported options are ``color=#RRGGBB`` or ``color=#RRGGBBAA``, ``pango`` and
``urgent``. Both flags accept ``1``/``0`` or ``true``/``false``. Pango enables
markup in both text variants. Options can appear in any order; unknown keys
and invalid values are ignored individually, and the last valid value for a
repeated option wins. Each update starts with no explicit color, no markup
and no urgency. A bare color such as ``#00ff00`` is also accepted as shorthand
for ``color=#00ff00``. Lines after the third are ignored.

Content is cut at ``max_bytes`` and text that is not UTF-8 falls back to the
file name.
Names starting with ``.`` or ending in ``~`` are ignored, so producers can
update atomically by writing ``.name.tmp`` and renaming it to ``name``.
Symlinks, FIFOs, devices and subdirectories never render.

The directory is watched natively and no Lua runs when files change: a change
costs one ``open``/``fstat``/``read``/``close`` and identical rewrites do not
render. Changes become visible when the writer closes the file or renames it
into place. The directory is created with mode ``0700`` when the configuration
becomes active (``--check`` never creates it) and is refused unless it is owned
by the current user and not group or world writable. If it is deleted or moved
away, its items disappear and it is recreated.

The ``battery`` module refreshes immediately on Linux power-supply change
events and keeps a slower periodic sample as a resynchronization fallback.

The ``nvidia`` module polls NVML for GPU utilization and power draw. Its
``format`` callback also receives memory utilization and usage, temperature,
device name and UUID when the GPU exposes those values.

D-Bus module API
----------------

Lua modules can lazily acquire the shared system or user bus and use it without
blocking the status loop:

.. code:: lua

   local bus = ctx:dbus("user")
   local match = bus:match({
       sender = "org.example.Service",
       path = "/org/example/Object",
       interface = "org.example.Interface",
       member = "Changed",
   }, function(message)
       local name, value = message:read("su")
       ctx:set { full_text = name .. ": " .. value }
   end, function(error)
       assert(error == nil, error and error.message)
   end)

   local call = bus:call({
       destination = "org.example.Service",
       path = "/org/example/Object",
       interface = "org.example.Interface",
       member = "GetState",
       signature = "",
       args = {},
       timeout = 5,
   }, function(reply, error)
       if error == nil then
           local state = reply:read("s")
           ctx:set { full_text = state }
       end
   end)

``bus:on_connect(callback)`` reports every connection epoch so modules can redo
connection-scoped initialization after a reconnect. All three operations return
cancellable handles owned by the block generation; retain a handle while its
operation should stay active. Calls are sent only after the staged configuration
becomes live, are never synchronously waited for, and are not replayed after a
disconnect.

Signatures are explicit. Basic values, arrays, structs, ordered dictionary pair
sequences, and variants are supported. ``ay`` is represented as a binary Lua
string, and ``x``/``t`` decode to exact LuaJIT ``int64_t``/``uint64_t`` cdata.
Use ``i3sd.dbus.dict(entries)`` to validate a dictionary pair sequence and
``i3sd.dbus.variant(signature, value)`` to create a variant. Message objects are
valid only for the duration of their callback, while values returned by
``message:read(signature)`` are ordinary owned Lua values.
