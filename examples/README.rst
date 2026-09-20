Examples
========

``i3sd.lua`` is a practical starting configuration using the bundled modules. Copy it to ``${XDG_CONFIG_HOME:-$HOME/.config}/i3sd.lua`` and adjust the filesystem paths and thresholds for the machine.

``minimal.lua`` demonstrates the low-level block API without loading modules. ``interactive.lua`` demonstrates per-consumer CPU deltas, click handling, and resettable one-shot timers without spawning external commands.

Validate a configuration before using it:

.. code:: console

   i3sd --check -c examples/i3sd.lua

When running an uninstalled binary directly from the source tree, make the bundled Lua modules discoverable:

.. code:: console

   LUA_PATH="$PWD/lua/?.lua;;" build/i3sd --check -c examples/i3sd.lua
