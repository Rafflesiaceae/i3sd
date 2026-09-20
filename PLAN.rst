SPEC
====

Goal
----

A small-footprint, event-driven Linux status generator for ``i3bar``: native C23 core, LuaJIT configuration/modules,
negligible idle CPU and wakeups.

Constraints
-----------

- Linux only.
- Emit/read the i3bar JSON protocol on stdout/stdin.
- Configuration: ``${XDG_CONFIG_HOME:-$HOME/.config}/i3sd.lua``, overridable with ``-c FILE``. The root configuration and
  ordinary tracked Lua dependencies must resolve to regular files and are expected to live on local filesystems; remote/FUSE/autofs
  backing is outside the supported synchronous-local-I/O model.
- No shelling out in built-in modules.
- Prefer events over polling.
- Prefer exception-driven presentation: healthy/stable conditions may remain hidden and appear only when action is useful.
- Blocks may be polling, event-driven, or both.
- Single process and one core ``epoll`` event loop. The core creates no worker threads. Optional in-process integrations must
  be externally drivable/nonblocking; integrations that fundamentally require an uncontrolled thread or blocking loop live in an
  external helper/service.
- Configuration and custom modules are Lua.
- Keep C generic; policy, thresholds, hysteresis, transient-display behavior and formatting belong in Lua where practical.
- No IP/network status integration exists in the core or built-ins: no address, link, route, throughput or packet-counter modules;
  do not open rtnetlink or read ``/proc/net``/``/sys/class/net`` for status. Local IPC such as D-Bus, i3 IPC and UNIX-domain helper
  sockets is not network-status integration.
- Built-ins must not wake sleeping storage devices merely to collect optional health/status data unless the user explicitly opts
  into that behavior.
- Hardware-specific metrics must be capability-driven. Unsupported values are unavailable, never fabricated as zero.
- External IPC and integrations that can block unpredictably must be asynchronous/nonblocking in the core and built-ins.
- Small synchronous kernel/libc/filesystem queries are allowed only where explicitly scoped. The core provides deterministic
  *computational-work/resource* bounds around such calls, not a hard wall-clock upper bound on kernel, filesystem, firmware or device
  latency. Any backend requiring a hard response-time guarantee must use a genuinely nonblocking interface or an external helper.
- Configuration is trusted, cooperative code and is not sandboxed; custom Lua/FFI can violate latency, memory and safety
  guarantees if it blocks, spins, allocates without bound or mutates native resources behind the core.

Normative Core Invariants
-------------------------

The following requirement IDs are authoritative. Later sections refine their mechanisms and tests but must not redefine them; if
descriptive prose appears to conflict, these invariants win.

- ``EVT-001``: the reactor never sleeps while immediate core work remains, continuously ready sources are fairness-budgeted, and
  kernel readiness is harvested at least once per event-loop turn. Continuous userspace continuations may prevent a blocking sleep
  but may never prevent a zero-timeout/readiness harvest that discovers newly ready fds.
- ``FD-001``: an epoll registration is identified by a core registration epoch/cookie in addition to its integer fd. Replacing a
  native connection/source creates a new registration even if Linux reuses the same fd number. Retired cookies are invalidated before
  close/teardown; stale harvested events whose cookie is no longer live are ignored without dereferencing retired state.
- ``GEN-001``: only the current generation may enter ordinary Lua callbacks; native objects may outlive a generation only through
  generation-safe shared ownership.
- ``GEN-002``: an event source made physically observable before its subscribing generation becomes current, or while a newly current
  generation is still behind its live-refresh/publication callback barrier, must either retain a bounded candidate event set, mark the source
  dirty for authoritative post-barrier resnapshot, or delay logical attachment. Required state changes may never be consumed and
  silently discarded merely because Lua dispatch is not yet enabled.
- ``RLD-001``: the old generation remains current throughout staging, local activation and pre-commit validation.
- ``RLD-002``: a candidate that is *known* before commit to differ from any root/dependency bytes or referent chain from which it was
  built is obsolete and must not commit; it is discarded and one coalesced reload from the newest state is scheduled.
- ``RLD-003``: **no rendered state produced during staging**, whether from ``init`` or staging ``update``, is publishable. After
  pointer commit, staged render state is cleared to hidden; every block defining ``update`` receives one mandatory live-refresh
  ``update`` while ordinary candidate event dispatch/rendering is gated. Candidate-local activation resnapshot callbacks required by
  ``GEN-002`` then run as publication prerequisites. Only after those bounded local prerequisites complete may the generation render
  or begin ordinary callbacks/external async initialization. A live-refresh callback failure is a post-commit block fault, not grounds
  to resurrect the old generation; an asynchronous source that cannot become authoritative inside the barrier remains hidden and does
  not hold publication of unrelated blocks.
- ``SHR-001``: remote state whose lifetime is intrinsically connection-scoped but logically shared across generations (for example
  systemd ``Manager.Subscribe``) is owned by a process-shared native lease coordinator. Generation-owned logical leases are staged
  without remote side effects, activated for the candidate before old-generation leases are released at commit, and never retain Lua
  references inside the process-shared coordinator.
- ``OUT-001``: once any status-frame byte is written, that frame is immutable until completion.
- ``OUT-002``: status-frame storage uses at most two full frame buffers: the last completed payload and one pending/in-flight payload;
  newer desired state is represented only by ``render_dirty`` while a frame is partial.
- ``SRC-001``: bounded authoritative sources never publish truncated state as authoritative; overflow produces unavailable/uncertain
  state and bounded resynchronization.
- ``MEM-001``: core-owned variable-size retained memory is charged to one process-wide steady-state budget; transactional reload has a
  separate bounded transition reserve. Per-subsystem/per-generation limits are sublimits, not simultaneously guaranteed allocations.
- ``LAT-001``: work counts, bytes, queue growth and core-owned allocations are deterministically bounded per turn/operation where
  specified. Approved synchronous local kernel/libc/procfs/sysfs/filesystem operations may still sleep inside the kernel or device
  stack and therefore carry no hard wall-clock event-loop latency guarantee.
- ``EXT-001``: a foreign loop that exposes only an indivisible nonblocking iteration is budgeted in *foreign dispatch quanta*.
  i3sd may preempt only between quanta and never claims to cap callbacks performed internally by one library iteration.
- ``ABI-001``: the v1 Lua callback signatures, collector names/options and returned field schemas documented under ``Lua ABI v1`` are
  normative. Adding/changing an incompatible schema requires an ``api_version`` change.
- ``JRN-001``: ``SD_JOURNAL_INVALIDATE`` invalidates journal cursor/cache completeness assumptions and triggers a bounded
  authoritative reseek/rebuild of the configured window before incremental following is authoritative again.
- ``SIG-001``: the signalfd-managed signal mask is installed in the initial thread before LuaJIT or any optional library capable of
  creating threads is initialized, so later incidental threads inherit the blocked mask.

Architecture
------------

.. code:: text

   i3bar <-> stdin/stdout <-> C23 core <-> LuaJIT
                            |
                            +-- epoll
                            +-- monotonic deadline heap
                            +-- optional realtime timerfd
                            +-- signalfd
                            +-- inotify
                            +-- optional PSI trigger fds
                            +-- optional kobject uevent
                            +-- optional sd-bus
                            +-- optional native event sources
                            +-- /proc, /sys

The C core owns:

- process lifecycle and signals;
- i3bar protocol framing/parsing via a bounded stream framer plus ``yyjson``;
- the ``epoll`` event loop, registration cookies/epochs and readiness harvesting;
- monotonic timer scheduling;
- wall-clock timer scheduling;
- fd/file watches;
- nonblocking stdin/stdout;
- bounded output buffering/backpressure;
- configuration loading/reload;
- LuaJIT state and native API bindings;
- generic asynchronous D-Bus transport and process-shared remote-lease coordinators when enabled;
- native collectors and stateful native-source resynchronization;
- typed block state;
- deterministic rendering and serialization;
- output deduplication;
- callback/resource lifetime management;
- process-wide accounting for bounded core-owned variable-size retained memory.

Lua owns:

- configuration;
- block definitions;
- built-in module policy;
- warning thresholds, hysteresis, transient visibility and escalation policy;
- formatting;
- click handlers;
- custom modules;
- D-Bus-specific logic such as systemd monitoring.

Configuration
-------------

Built-ins should expose concise high-level configuration with sensible defaults:

.. code:: lua

   clock {
       format = "%H:%M",
   }

   cpu {
       interval = 2,
   }

   systemd {
       scope = "system",
   }

Each built-in is implemented as a Lua module over native collectors/event primitives where appropriate. High-level built-in
configuration tables are strict schemas: unknown option names, invalid enum values and invalid option types reject the candidate
rather than being silently ignored.

Built-ins should make exception-driven status easy without requiring a custom ``block``. For example:

.. code:: lua

   filesystem {
       path = "/",
       interval = 60,
       warn_below = 20,
       critical_below = 10,
       hysteresis = 2,
   }

   pressure {
       resource = "io",
       mode = "full",
       stall = 0.10,
       window = 2.0,
       hold = 5,
   }

   battery {
       show = "auto", -- module-specific automatic policy
   }

Where a built-in accepts a ``format`` callback, its return contract is:

- ``nil`` or ``false``: hide the block;
- a string: use it as ``full_text``;
- a table: use the recognized mutable i3bar block-state fields.

Threshold-driven modules should support hysteresis when a threshold can flap around a boundary. Event/transient modules should
use resettable one-shot timers for hold/decay behavior rather than introducing a periodic tick.

Asynchronous built-ins start hidden until they have an authoritative initial snapshot unless their module explicitly defines a
loading/unavailable representation. They must not display fabricated zero values while initializing or disconnected.

The low-level ``block`` API remains available for custom modules.

Configuration is trusted, cooperative code. It is not a security boundary or preemptive scheduler.

Blocks
------

.. code:: lua

   block {
       name = "example",
       key = "optional",
       order = 10,
       interval = 5,

       init = function(ctx)
           ...
       end,

       update = function(ctx)
           ctx:set {
               full_text = "...",
           }
       end,

       click = function(ctx, button, event)
           ...
       end,
   }

``name`` is required.

``key`` is an optional configuration-only identity component. It is not the i3bar protocol ``instance`` field.

The configured ``(name, key)`` pair must be unique.

``order`` defaults to ``0``.

Higher ``order`` values are emitted first. Equal values preserve declaration order.

Order and declaration order are immutable after configuration commit, so blocks are sorted exactly once per configuration
generation.

``interval`` is optional.

A block may be:

- polling-only: ``update`` plus ``interval``;
- event-only: registers event sources in ``init`` and has no ``update``; it starts hidden until an authoritative live callback sets
  state;
- event-driven with initial snapshot: registers event sources and defines ``update`` without ``interval``; staging validates it and
  the mandatory live refresh supplies the initial visible snapshot, but no periodic deadline is created;
- hybrid: combines event sources with a periodic ``update``/``interval``.

Lifecycle:

#. construct all blocks for the candidate generation;
#. run every block's ``init(ctx)`` once in declaration order while staging;
#. after all ``init`` callbacks succeed, run every defined staging ``update(ctx)`` once in declaration order to validate the update
   path, collector options and resulting typed state;
#. activate and pre-commit-validate the candidate;
#. commit the candidate pointer, activate its process-shared logical leases/timer epoch and retire old-generation dispatch;
#. while ordinary candidate callbacks/rendering remain gated, run one mandatory **live-refresh** ``update(ctx)`` for every block that
   defines ``update``; this queue is resumed across event-loop turns under the normal callback/global work budgets;
#. after the live-refresh queue, run bounded candidate-local activation-resnapshot callbacks required by ``GEN-002`` as publication
   prerequisites; sources needing later asynchronous initialization remain hidden instead of blocking unrelated publication;
#. when those local prerequisites complete, enable ordinary candidate callbacks/external asynchronous initialization and render the
   committed generation.

All ``ctx:set`` state created during staging is validation-only and is cleared to hidden at pointer commit before any live refresh.
The staging ``update`` therefore validates the update path without creating an old sample that can leak into the first frame. This
removes the otherwise unbounded freshness gap between staged observation and a commit that may follow a multi-turn validation
barrier. Built-in ``update`` callbacks must tolerate immediate re-execution and should be sampling/state functions rather than
irreversible external operations. Custom trusted Lua already receives the same warning through ``--check`` semantics: staging
callbacks can execute more than once and are not a side-effect boundary.

Failure of any staging ``init`` or staging ``update`` rejects the candidate configuration. Once pointer commit has occurred, failure
of a mandatory live-refresh ``update`` follows the normal runtime block-fault policy; the previous generation is never resurrected.
An event-only block need not define ``update`` and does not participate in the live-refresh callback queue, but it starts hidden until
an authoritative post-commit event/source snapshot calls ``ctx:set``. A block that wants one initial synchronous visible snapshot but
no periodic polling should define ``update`` without ``interval``.

Block State
-----------

``ctx:set { ... }`` replaces the block's complete rendered state. A newly constructed block has hidden/empty state until its
first successful ``ctx:set``. During staging, ``ctx:set`` validates and stores candidate-local state only: it never requests process
output/rendering, and ``RLD-003`` clears that staged state to hidden at pointer commit before live refresh/publication.

Recognized i3bar fields are validated and copied immediately into a native C block-state structure.

The initial mutable field set is explicit:

- ``full_text``;
- ``short_text``;
- ``color``;
- ``background``;
- ``border``;
- ``border_top``, ``border_right``, ``border_bottom`` and ``border_left``;
- ``min_width``;
- ``align``;
- ``urgent``;
- ``markup``.

``full_text`` is required by ``ctx:set``. Optional fields omitted from a replacement state return to their defaults.

Lua tables are never retained as canonical rendering state.

Immutable identity/ordering fields such as ``name``, ``key`` and ``order`` are not part of ``ctx:set``.

Core-controlled protocol fields such as ``separator`` and ``separator_block_width`` are rejected from ``ctx:set``. Custom
underscore-prefixed output keys are not part of the initial typed-state API.

Validate enums/ranges and i3bar-compatible types, including ``align`` values, ``markup`` values, non-negative border widths and
``min_width`` as either a non-negative integer or string.

Unknown or invalid fields are errors.

Text-facing fields (including ``name``, ``key``, ``full_text``, ``short_text`` and string ``min_width``) must be valid UTF-8, must
not contain embedded NUL bytes and are bounded in bytes by the implementation limits below. Color fields use their documented
i3bar color syntax; enum-like fields are validated as enums. Lua strings used as binary data by non-rendering APIs, such as D-Bus
``ay`` values, remain byte strings and are not subject to the UTF-8 rule. The core never silently repairs invalid text.

Before allocating replacement strings, the core should compare length/content against the current value where practical.

``ctx:set`` requests a render only if the effective visible state changed.

There is no general ``ctx:redraw()`` API.

An empty ``full_text`` hides the block.

Lua API
-------

The public Lua surface is explicitly versioned. ``i3sd.api_version`` is an integer compatibility level and
``i3sd.version`` is the implementation version string. Build-time feature discovery is side-effect free:

.. code:: lua

   if i3sd.api_version >= 1 and i3sd.has_feature("dbus") then
       ...
   end

   local compiled = i3sd.features()

``features()``/``has_feature()`` report compiled API availability only; they do not probe hardware, open transports or imply that a
runtime service/device exists. Runtime capability remains source/backend specific and is reported through normal initialization or
snapshot results.

Core primitives:

.. code:: lua

   ctx:set { ... }

   local h = ctx:after(seconds, fn)
   local h = ctx:every(seconds, fn)
   local h = ctx:at_realtime(unix_seconds, fn)
   local h = ctx:on_clock_change(fn)
   local h = ctx:on_timezone_change(fn)

   local h = ctx:watch_fd(fd, events, fn)
   local h = ctx:watch_file(path, fn)
   local h = ctx:defer(fn)

   local snapshot, err = ctx:sample(kind, options)

   local h = ctx:spawn({
       argv = { "example-command", "--mode", "menu" },
       stdin = "first\nsecond\n",
       stdout_limit = 1024,
   }, fn)

   local h = ctx:watch_pressure({
       resource = "memory", -- "cpu", "memory", or "io"
       mode = "some",      -- "some" or "full" where supported
       stall = 0.15,       -- seconds of cumulative stall
       window = 2.0,       -- seconds
   }, fn)

   h:cancel()

Lua ABI v1
~~~~~~~~~~

``ABI-001`` makes this subsection normative. Unless a field is explicitly optional, its presence/type is part of API version 1.
LuaJIT cdata is used for exact 64-bit integer values; ordinary Lua numbers are used for bounded floating-point values, percentages
and intervals.

Core/block callback signatures are:

.. code:: lua

   init(ctx)
   update(ctx)
   click(ctx, button, event)
   timer_fn(ctx)
   clock_change_fn(ctx)
   timezone_change_fn(ctx)
   fd_fn(ctx, revents)
   file_fn(ctx, event)
   pressure_fn(ctx, event)
   spawn_fn(ctx, result)
   defer_fn(ctx)

``button`` is the i3bar integer button value. The v1 click ``event`` table contains only recognized members that were present on the
wire: integer ``x``, ``y``, ``relative_x``, ``relative_y``, ``output_x``, ``output_y``, ``width`` and ``height`` plus
``modifiers`` as an ordered 1-based sequence of strings. Wire ``name``/``instance`` are routing inputs and are not copied through as
untrusted event fields; configured identity remains available from ``ctx``. Unknown click members are ignored.

Primitive argument contracts are:

- ``after(seconds, fn)`` accepts a finite Lua number ``seconds >= 0``; ``every(seconds, fn)`` requires finite ``seconds > 0``;
  ``at_realtime(unix_seconds, fn)`` accepts a finite absolute Unix-realtime Lua number. Timer callbacks receive only ``ctx``.
- ``on_clock_change(fn)`` and ``on_timezone_change(fn)`` callbacks receive only ``ctx``; they are invalidation hooks and callers
  explicitly recompute/resample whatever semantic state they need.
- ``watch_fd(fd, events, fn)`` requires a non-negative integer fd and a strict ``events`` table containing only optional boolean
  ``read`` and ``write`` keys, with at least one true. Error/hangup delivery is implicit and cannot be disabled.
- ``watch_file(path, fn)`` requires a valid UTF-8, NUL-free expected-local pathname string. Its callback contract is below.
- ``defer(fn)`` registers ``defer_fn(ctx)`` under the teardown restrictions already specified; its returned handle is cancelled with
  the same idempotent ``cancel()`` method as other logical handles.
- ``sample(kind, options)`` requires a documented collector ``kind`` string and either ``nil`` or a strict option table; ``nil`` is
  equivalent to ``{}``. Whole-collector/runtime unavailability returns ``nil, err``. Unsupported optional fields inside an otherwise
  valid snapshot are absent/``nil`` and are never synthesized as numeric zero.
- ``spawn(options, fn)`` accepts a strict table with a required dense ``argv`` sequence, optional binary ``stdin`` string and optional
  ``stdout_limit``. ``argv`` contains 1 to 64 NUL-free strings, each at most 4096 bytes and at most 64 KiB in aggregate. Its first
  string, the executable passed to ``execvp``, must be non-empty; subsequent arguments may be empty. ``stdin`` and ``stdout_limit``
  are each bounded to 64 KiB. Execution uses ``execvp`` directly and never invokes a shell. At most one child is active process-wide.
  The method returns ``nil`` without starting a child during staging, while another child is active or when process setup fails;
  otherwise it returns an idempotently cancellable block-owned handle. On normal completion the callback receives a result table
  containing binary ``stdout``, booleans ``success``, ``overflow`` and ``io_error``, and either integer ``exit_status`` or ``signal``.
  ``success`` requires exit status zero and complete, error-free captured output. Cancellation does not invoke the callback. A child
  and callback are cancelled before their block's generation is destroyed.

``revents`` is a table containing only boolean keys ``read``, ``write``, ``error`` and ``hangup`` that are true for the delivered
condition. ``EPOLLERR``/``EPOLLHUP`` are represented by ``error``/``hangup`` even when not requested explicitly.

``watch_file`` callbacks receive an event table with ``path`` and ``reason``. ``reason`` is one of ``"changed"``, ``"unavailable"``,
``"overflow"`` or ``"resnapshot"``. Events are invalidation hints, not file contents; consumers re-open/resample authoritative state.
Multiple native notifications may coalesce into one logical callback.

``watch_pressure`` callbacks receive ``{ resource=..., mode=..., stall=..., window=... }`` describing the trigger registration that
fired. The callback does not carry inferred current averages; call ``sample("psi", ...)`` when a fresh snapshot is needed.

``ctx:sample(kind, options)`` returns ``snapshot`` or ``nil, err``. Every successful snapshot contains:

- ``timestamp_ns``: exact ``uint64_t`` cdata in the ``CLOCK_MONOTONIC`` domain, taken at/around the authoritative read;
- ``source_id``: opaque non-empty UTF-8 string whose **equality** identifies the underlying source/inventory instance relevant to
  baseline reuse; its contents have no parsing contract and may change when topology/referent identity changes;
- ``continuity``: exact opaque ``uint64_t`` cdata token. Equality means only that the core has not observed a baseline-breaking
  discontinuity; consumers must not interpret its numeric value. A source-identity change or continuity-token change invalidates any
  cumulative baseline.

The baseline v1 collector names/options/results are:

- ``sample("cpu", { per_cpu = boolean? })`` -> ``aggregate`` plus optional ``cpus``. A CPU counter table contains exact
  ``user``, ``nice``, ``system``, ``idle``, ``iowait``, ``irq``, ``softirq``, ``steal``, ``guest`` and ``guest_nice`` counters in
  USER_HZ ticks. ``aggregate.id`` is ``"cpu"``; each ``cpus[i].id`` is the kernel ``cpuN`` name. ``per_cpu`` defaults to ``false``.
- ``sample("load", {})`` -> ``load1``, ``load5``, ``load15`` as Lua numbers and ``online_cpus`` as a positive integer.
- ``sample("memory", {})`` -> exact-byte cdata fields ``mem_total``, ``mem_available``, ``swap_total`` and ``swap_free``.
- ``sample("psi", { resource = "cpu"|"memory"|"io" })`` -> ``some`` and optional ``full`` tables. Each available table contains
  ``avg10``, ``avg60``, ``avg300`` as Lua numbers and ``total_us`` as exact cdata. Non-meaningful/unavailable ``full`` is ``nil``;
  it is never synthesized as zero.
- ``sample("filesystem", { path = string })`` -> the exact input ``path`` plus exact unsigned cdata ``total_bytes``,
  ``free_bytes`` and ``available_bytes`` and boolean ``readonly``. ``available_bytes`` is based on ``f_bavail``; ``free_bytes`` is
  based on ``f_bfree``. Filesystem/referent identity is represented only by the common opaque ``source_id``.
- ``sample("block_io", { mode = "physical"? , devices = {string,...}? })`` -> ordered ``devices`` records. Exactly one of
  ``mode`` or ``devices`` may be supplied; omitted options mean ``mode="physical"``. Each record contains ``name``, ``dev_major``,
  ``dev_minor`` as non-negative Lua integers and exact unsigned cumulative cdata counters ``read_bytes``, ``write_bytes``,
  ``read_ios``, ``write_ios``, ``io_time_ms`` and
  ``weighted_io_time_ms``. Byte counters use the kernel block-statistics 512-byte sector definition. Explicit ``devices`` preserves
  caller order; physical mode uses deterministic lexical device-name order.
- ``sample("power_supply", {})`` -> ordered ``supplies`` records with mandatory ``name`` and ``type`` strings, optional ``status``
  string, and optional exact signed ``int64_t`` cdata fields ``energy_now_uwh``, ``energy_full_uwh``, ``energy_full_design_uwh``,
  ``charge_now_uah``, ``charge_full_uah``, ``charge_full_design_uah``, ``power_now_uw``, ``current_now_ua`` and
  ``voltage_now_uv``. Missing sysfs capabilities are ``nil`` rather than zero. Ordering is lexical by supply name.
- ``sample("hwmon", {})`` -> ordered ``sensors`` records containing ``chip``, ``name``, ``kind`` and exact signed ``int64_t`` cdata
  ``value_milli``;
  optional ``label`` is present when exported by the driver. ``kind`` is initially ``"temp"`` for baseline temperature support.
  Ordering is deterministic by ``chip``, then ``name``.
- ``sample("time", {})`` -> exact unsigned cdata ``realtime_ns``, ``monotonic_ns`` and ``boottime_ns``. The reads are bracketed/
  ordered closely but are not an atomic multi-clock snapshot. Local civil-time formatting/timezone semantics use the separate core
  clock/timezone APIs rather than an undocumented timezone string in this collector.
- ``sample("file_stat", { path = string, follow = boolean? })`` -> the exact input ``path``, ``exists`` and, when ``exists=true``,
  exact unsigned ``size_bytes``, ``device`` and ``inode`` cdata, exact signed ``int64_t`` cdata ``mtime_ns``, and ``kind``
  (``"regular"``, ``"directory"``, ``"symlink"`` or ``"other"``). A timestamp that cannot be represented as signed 64-bit
  nanoseconds is a ``too_large`` runtime-unavailability error rather than wrapping. ``follow`` defaults to ``true``; when false, the
  final symlink itself is described. Expected-local-path restrictions apply.

Optional collector schemas are exposed only when their named feature exists; when first implemented they must be documented here (or
under a later ``api_version`` subsection) before being public. In particular, optional CPU-frequency, powercap, GPU and process-scan
backends must not expose ad-hoc undocumented tables from C.

Option tables are strict: unknown keys or wrong types raise a programmer/configuration error. Result tables may gain **new optional
fields** within the same API version only when old consumers necessarily ignore their absence/presence; changing an existing field's
meaning/type, removing a field, renaming a collector, or changing callback arguments requires an ``api_version`` increment.

``ctx:sample`` exposes only registered native collectors; it is not a generic file-I/O escape hatch. Collector-specific options are
validated by the core. Calls are synchronous only for collectors whose local kernel/libc access is explicitly accepted by this
specification. When equivalent samples are requested more than once in one event batch, the core may return the same raw
snapshot rather than rereading the source.

Raw cumulative values are not converted into global rates by ``ctx:sample``; rate/delta state belongs to the consuming module
instance. Cumulative snapshots expose a monotonic sample timestamp, stable source identity where applicable and an opaque continuity
token. The token changes when the core knows that a previous baseline must not be reused, including resume/resynchronization
boundaries and relevant source reinitialization.

``watch_pressure`` is the generic Linux PSI trigger primitive. It is created lazily, returns a normal generation-owned handle and
surfaces unsupported resources/modes or kernel permission/parameter failures as configuration/runtime errors rather than silently
falling back to polling. Identical pressure-trigger registrations may share one native fd and fan out callbacks.

Handles returned to Lua are block-owned subscriptions. Destroying or faulting a block cancels those subscriptions automatically.
A subscription may reference a generation-shared source or process-shared transport as defined under ``Ownership Model``; cancelling
the subscription must not tear down shared state still referenced elsewhere.

Calling ``cancel()`` repeatedly is harmless.

Callbacks may cancel themselves or other callbacks safely.

Resources removed during callback dispatch are retired and freed only after no current dispatch frame can reference them.

``watch_fd`` uses level-triggered readiness.

A raw fd may have only one logical ``watch_fd`` owner. Attempting to register the same fd twice is an error. Shared native
transports such as inotify and D-Bus fan events out through their own higher-level source objects instead.

A custom fd callback must consume the relevant condition sufficiently to avoid continuously remaining ready.

``EPOLLERR`` and ``EPOLLHUP`` are surfaced to the watcher even if it requested only read/write readiness.

FD ownership must be explicit; watched fds are not closed by default.

``ctx:defer(fn)`` registers a block-owned teardown callback for user-owned resources such as FFI objects or raw fds. Registrations
are cancellable handles and run exactly once in reverse registration order when their owning block/candidate is torn down, including
staging rollback, runtime block fault, successful reload replacement, ``--check`` candidate destruction and process shutdown. Before
running deferred cleanup, the core first disables/cancels that block's ordinary timers, watches, asynchronous operations and source
subscriptions so cleanup can safely release resources they referenced. Cleanup callbacks use protected non-yielding calls; an error
is logged and remaining cleanups still run. Teardown callbacks may release user-owned resources but may not call ``ctx:set``, create
new timers/watches/subscriptions/asynchronous operations or otherwise reactivate the block. Cancelling a cleanup handle prevents it
from running.

All C -> Lua callbacks, including teardown callbacks, use protected calls and restore the Lua stack on every success/error path.
Ordinary callbacks are run-to-completion: yielding/coroutine suspension across a native callback boundary is unsupported and is
treated as a block error.

Core-owned variable-size protocol buffers, queues, reconciliation storage and authoritative source caches are hard-bounded by the
implementation limits below. Exceeding a limit follows the explicit failure-scope rules; it must never turn into unbounded
core-owned allocation. Allocations internal to LuaJIT or optional third-party libraries, and arbitrary allocations performed by
trusted custom Lua/FFI, remain outside those core bounds.

Ownership Model
---------------

Runtime ownership has three distinct levels:

- **block-owned subscriptions/handles**: timers, logical file/fd watches, D-Bus calls/matches, source subscriptions and callbacks
  requested by one block. Cancelling/faulting the block removes only these subscriptions;
- **generation-shared sources**: authoritative caches and higher-level source objects shared by multiple blocks in one committed
  configuration generation, such as one systemd manager cache or one power-supply inventory source. They are reference-counted by logical
  subscriptions and are destroyed when their generation is retired or their final reference disappears;
- **process-shared transports/primitives**: reusable transport/kernel machinery whose lifetime may span generations while referenced,
  such as the system/user D-Bus transport, the shared inotify fd, native backend transport state and connection-scoped protocol state
  whose lifetime is intrinsically tied to one native transport connection. This level also contains remote-lease coordinators for
  connection-scoped remote state such as systemd ``Manager.Subscribe``. Process-shared objects contain no direct Lua registry
  references or pointers into a replaced Lua state. Generation-owned logical handles carry generation tokens and are suppressed once
  that generation stops being current.

Sharing is an implementation optimization only when semantics are identical. Per-consumer rate baselines, formatter state, policy,
visibility and callback ownership are never silently merged merely because an underlying raw source is shared.

Process-shared remote leases obey ``SHR-001``. A lease coordinator is keyed by transport identity plus an opaque service-defined key
whose acquire/release specifications must match exactly for every user of that key. The coordinator owns only copied native request
metadata, connection-epoch state, desired-reference count, bounded retry state and remote state
(``unsubscribed``/``acquiring``/``subscribed``/``releasing``/``unknown``); it never owns a Lua callback. A generation's lease handle
is a normal block-owned logical reference to that coordinator.

A staged lease contributes no desired remote reference and sends no message. During commit, candidate lease handles become active
*before* corresponding old-generation handles are released, so a same-transport handoff changes ``1 -> 2 -> 1`` rather than
``1 -> 0 -> 1``. Fresh acquisition may be gated by module-specific prerequisites such as confirmed D-Bus match installation; an
already-subscribed remote state is retained while any active lease exists even if the new lease's acquisition gate is not ready yet.
Connection loss increments the native connection epoch and resets remote state to unknown/unsubscribed without changing logical
lease references. Persistent release failure is rate-limited and does not force a shared transport reconnect merely to clean up one
service's remote state; later acquisition treats an ``AlreadySubscribed``-style declared success error as convergence when configured.


``GEN-002`` applies to every shared/native backend, not only to file watches. If candidate subscriptions become physically observable
during activation, or native transports continue draining after pointer commit while candidate ordinary callbacks are still gated by
``RLD-003``, the backend must explicitly choose bounded event reconciliation, a ``dirty_while_inactive`` publication-prerequisite
resnapshot, a hidden-until-authoritative asynchronous source, or delayed logical attachment. Before pointer commit a process-shared
transport may continue draining for the old
generation; after pointer commit it may continue native progress without entering Lua. In neither phase may it consume a state
transition required by the candidate and then discard it merely because candidate Lua dispatch is disabled.

A block fault removes that block's subscriptions but does not destroy a shared source/transport still used by another block. A
generation retirement drops all of that generation's source/subscription references. A shared source failure is reported once to each
dependent live subscription according to its API and recovery policy; it is not attributed to an arbitrary "owner" block.

Failure Model
-------------

Failures have explicit scope:

- candidate configuration/Lua execution, staging ``init``/``update``, identity validation, static resource-limit or synchronous
  activation failure rejects the candidate generation and leaves the previous generation active; a mandatory live-refresh ``update``
  runs only after pointer commit and therefore faults that block on failure under ``RLD-003`` rather than rolling back;
- an unhandled runtime Lua callback error or a runtime block-owned operation that cannot be completed safely faults only that block;
- failure of a generation-shared source marks that source unavailable/uncertain, notifies or hides dependent blocks as specified by
  the source, and performs bounded source-specific recovery/resynchronization; unrelated sources continue;
- failure/disconnect of a process-shared transport marks dependent sources unavailable and starts its bounded reconnect policy without
  faulting unrelated blocks;
- malformed/oversized external input is discarded/resynchronized according to the corresponding parser and does not fault a block;
- reconciliation-backlog overflow abandons the in-progress candidate snapshot and schedules a fresh authoritative snapshot;
- ``EPIPE``/closed i3bar stdout is normal clean termination;
- allocation/resource exhaustion is isolated at the smallest safe scope when invariants can still be preserved. If the core cannot
  preserve its own invariants (for example unrecoverable ``ENOMEM`` while maintaining essential event/output/lifetime state), log a
  fatal error where possible and terminate nonzero rather than continue in a partially valid state;
- an internal invariant violation is process-fatal in debug/testing builds and must never be converted into fabricated status data.

Errors exposed to Lua use a consistent convention per API: configuration/argument errors raise a Lua error; expected runtime
unavailability is returned as ``nil, err`` or delivered to the asynchronous completion callback; asynchronous source disconnects do
not synchronously unwind an unrelated callback. ``err`` is a typed table rather than a string-parsing contract. It has stable fields:

- ``code``: machine-readable string such as ``unsupported``, ``unavailable``, ``timeout``, ``disconnected``, ``too_large`` or
  ``permission``;
- ``message``: human-readable diagnostics;
- ``source``: optional subsystem/source name;
- ``errno``: optional numeric errno where meaningful;
- ``transient``: optional boolean recovery hint;
- ``dbus_name``: optional D-Bus error name for D-Bus failures.

Modules make policy decisions from stable fields such as ``code``/``dbus_name`` rather than matching human-readable text.

Implementation Limits
---------------------

The initial implementation uses hard limits so memory and parser behavior are testable. Limits are byte counts unless stated
otherwise and are checked before allocation/growth where practical:

- root configuration and each tracked Lua dependency source file: 1 MiB;
- aggregate root plus tracked Lua dependency source bytes: 16 MiB per generation;
- loaded Lua configuration dependencies: 256 files per generation;
- referent-resolution symlink hops for a tracked path: 40;
- configured blocks: 256;
- ``name``/``key``: 256 bytes each;
- each rendered text/string field: 6 KiB;
- serialized contribution of one visible block object, including core-controlled wire fields: 7 KiB;
- serialized status frame: 2 MiB;
- full status-frame storage: at most two 2 MiB buffers (4 MiB total) as required by ``OUT-002``;
- one click-event object: 64 KiB;
- click-stream nesting depth: 32;
- process-lifetime click identity/token mappings: 4096;
- child-process argv entries: 64, 4096 bytes each and 64 KiB in aggregate;
- child-process stdin and captured stdout: 64 KiB each;
- active child processes: one process-wide;
- logical timers: 4096 per generation;
- logical watches/subscriptions: 4096 per generation;
- deferred cleanup callbacks: 4096 per generation;
- pending asynchronous operations: 1024 per generation;
- one D-Bus message accepted by the Lua marshalling layer: 1 MiB;
- aggregate core-owned retained D-Bus request/reply payload storage: 32 MiB per generation;
- D-Bus container nesting depth: 32;
- decoded D-Bus values/container elements: 65536 per message;
- one source reconciliation backlog: 4096 events and 4 MiB, whichever is reached first;
- aggregate retained reconciliation-backlog storage: 16 MiB per generation;
- one authoritative native/source cache: 16384 records and 8 MiB of core-owned retained record/string storage, whichever is reached
  first;
- aggregate core-owned authoritative source-cache storage: 32 MiB process-wide across generation-shared and process-shared caches;
- process-wide steady-state core-owned variable-size retained-memory budget: 64 MiB;
- transactional reload transition reserve: an additional 24 MiB, usable only by the not-yet-live candidate and reload bookkeeping
  until the old generation has been retired.

The implementation may expose these constants through diagnostics/version output, but configuration does not raise them at runtime.
A change to a limit is an implementation/version change, not silent adaptive allocation. ``MEM-001`` is authoritative: the
per-generation/per-subsystem limits above are sublimits and do not imply that all maxima may be retained simultaneously. Every
core-owned variable-size retained allocation is charged to the steady-state process budget or, for candidate-only pre-commit state,
the reload transition reserve before allocation/growth succeeds.

The transition reserve exists to make transactional replacement predictable without doubling every steady-state allowance. Large
external authoritative caches and asynchronous payloads are therefore not constructed for a candidate during staging merely to
mirror the old generation: when duplication would exceed the transition reserve, activation records the requirement and the newly
committed source starts hidden/unavailable and builds its authoritative cache after old-generation charges have been released. A
candidate that exceeds the transition reserve using state that truly must exist before commit is rejected cleanly; the core never
borrows unbounded memory from the old generation's budget.

Before pointer commit, perform a memory-accounting preflight for the state that will survive as the candidate's steady state:
``process-shared retained charges + candidate retained steady-state charges <= 64 MiB`` after subtracting old-generation-only charges
that commit is guaranteed to retire. Candidate allocations currently charged to the transition reserve are reclassified into the
steady-state budget only after those old-generation-only charges are released. If that post-retirement accounting cannot be proven to
fit, reject the candidate **before** pointer commit; never discover steady-state budget failure after the rollback point has passed.

During staging, exceeding a configuration/static limit, including the 16 MiB aggregate executed-source budget, rejects the candidate. During normal runtime, a block-owned request exceeding
a limit raises an error in that block. ``ctx:set`` preflights only the resulting block object's exact serialized contribution; if it
would exceed the 7 KiB per-visible-block bound, the operation fails before replacing canonical state. With at most 256 configured
blocks, this contribution bound leaves sufficient headroom inside the 2 MiB frame cap for array framing, commas and fixed renderer
overhead, so aggregate-frame overflow cannot depend on callback/update order. A source backlog uses the resnapshot rule above.
Oversized click input is discarded without growing the core input buffer past its cap. A D-Bus message above the Lua-facing cap or a
retained operation that would exceed the per-generation aggregate D-Bus payload budget is rejected before unbounded core-owned
growth. Allocations internal to libsystemd before the Lua/core ownership boundary remain outside that bound. Reconciliation storage
obeys both its per-source and per-generation aggregate caps; exceeding either abandons the candidate snapshot/resynchronizes rather
than retaining more events. Authoritative native/source caches obey both record-count and retained-byte limits. A snapshot that would
exceed either cache bound is never published truncated as authoritative: mark the source unavailable with a deterministic
``too_large``/resource-limit error, retain the previous cache only as explicitly stale if useful, and do not immediately resnapshot in
a tight loop. Retry only after a later external invalidation/resynchronization request, reload or explicit module recovery policy.

Timers
------

Ordinary timers do not use one ``timerfd`` per timer or block.

Timer durations/intervals must be finite. ``after`` accepts non-negative delays; ``every`` and block ``interval`` require a strictly
positive interval. ``at_realtime`` accepts a finite absolute Unix realtime timestamp. Conversion to the core's integer time unit is
checked for overflow and must not silently turn a positive interval into zero.

The core maintains a single min-heap of ``CLOCK_MONOTONIC`` deadlines.

Ordinary timers therefore measure awake monotonic time:

- the clock advances while the process is descheduled, stopped by ``SIGSTOP`` or paused in a debugger;
- the clock does not include time while the machine is suspended;
- ordinary timers are not specified to expire immediately merely because a suspend interval exceeded their deadline.

The next deadline is supplied directly as the timeout to ``epoll_pwait2()``.

Use ``epoll_wait()`` with a rounded-up millisecond timeout as a compatibility fallback. If ``epoll_pwait2()`` was compiled
in but the running kernel returns ``ENOSYS``, permanently switch to the fallback at runtime. Clamp fallback timeouts to
``INT_MAX`` milliseconds.

Repeating timers use absolute/fixed-rate scheduling rather than:

.. code:: text

   next = now + interval

so callback execution time does not accumulate drift.

After ``SIGSTOP``, debugger stops or other long process stalls, missed monotonic intervals are skipped. A repeating callback
runs at most once before being rescheduled to its next future deadline.

System suspend is different because ``CLOCK_MONOTONIC`` pauses during suspend. The core samples the boottime/monotonic offset at
startup and after each natural event-loop wake using a bracketed read ``M1 -> B -> M2``. Samples whose ``M2-M1`` bracket exceeds
10 ms are retried later rather than treated as evidence of suspend; otherwise use the monotonic midpoint for the offset estimate. An
increase of at least 250 ms over the previously accepted offset is a suspend-gap signal and requests the same state-resynchronization
path used for other uncertainty events. Smaller changes are treated as measurement jitter. This detection adds no fd and no periodic
wakeup; without another resume source it is deliberately eventual and occurs on the first natural wake after resume. When a system
D-Bus connection is already active, the core may additionally subscribe to logind ``PrepareForSleep`` and treat the post-sleep
transition as an immediate resume-resynchronization barrier. A configuration/integration that requires prompt resume freshness may
explicitly request that subscription even if it is the only reason to open the system bus; otherwise the zero-extra-fd clock-gap
fallback remains sufficient. Duplicate resume indications are coalesced.

Ordinary timer deadlines are still not converted to boottime deadlines and do not retroactively catch up for suspended time.

Polling blocks created by the same configuration generation use the same monotonic activation epoch so compatible intervals
naturally share wakeups.

Relative timers declared while a candidate configuration is staged begin from the generation's activation epoch, not from
the wall time spent executing staged ``init`` callbacks.

Cancelled timers are physically removed from monotonic/realtime deadline heaps in bounded time (for example through indexed
O(log n) removal), or the heaps are compacted under an explicit bounded policy. Repeated creation/cancellation of far-future timers
must not accumulate unbounded tombstones. Due timer callbacks are also dispatched under a per-turn work budget; a large
phase-aligned expiry set may continue on the next loop turn without allowing timers to starve fd/stdin/stdout processing.

There is no global periodic tick.

Wall-Clock Timers
-----------------

Wall-clock-aligned events such as the clock require correct handling of realtime-clock and timezone changes.

``ctx:at_realtime(unix_seconds, fn)`` registers one absolute Unix-realtime instant. Its timestamp is not a calendar rule and is
never semantically recomputed by C. Repeating/calendar behavior is built in Lua by scheduling the next absolute deadline after
each callback; the core does not add a second periodic wall-clock tick.

Maintain a separate wall-clock deadline heap and create at most one shared ``CLOCK_REALTIME`` ``timerfd`` when at least one
absolute deadline or clock-change observer exists. Arm it to the earliest wall-clock deadline using absolute time and
``TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET``. If clock-change observers exist without a real deadline, arm a far-future sentinel solely so
discontinuous clock changes remain observable without a periodic wakeup. This timerfd is the sole sleep/wakeup mechanism for the
wall-clock heap: realtime deadlines are not independently converted into the main ``epoll_pwait2()`` timeout.

The timerfd is always drained when readable before ordinary rearm where possible. ``ECANCELED`` from ``read()`` means that
``CLOCK_REALTIME`` changed discontinuously. Linux may also report ``ECANCELED`` from ``timerfd_settime()`` after an undrained
cancellation while still applying the requested new timer; treat that case according to the timerfd API rather than assuming the rearm
failed. In either cancellation case:

- keep each registered absolute Unix timestamp unchanged;
- invoke registered ``on_clock_change`` callbacks once for the coalesced discontinuity before dispatching newly-past realtime
  deadlines, so policy callbacks may cancel/rearm them;
- mark any remaining already-past deadlines due once;
- rearm future deadlines against the new realtime.

``ctx:on_clock_change(fn)`` is generation/block-owned and exists specifically for semantic rescheduling rules that C cannot infer
from an absolute timestamp.

Timezone changes do not necessarily change ``CLOCK_REALTIME``. ``ctx:on_timezone_change(fn)`` lazily enables timezone-change
tracking. i3sd follows the process timezone chosen at startup: if ``TZ`` is set, that environment value is authoritative for the
process lifetime and automatic ``/etc/localtime`` tracking is disabled; changing ``TZ`` requires process restart/re-exec. If ``TZ``
is unset, track ``/etc/localtime`` through the shared resolution-chain watch helper so replacement of the logical entry, an
intermediate symlink component or the final zoneinfo referent is observable. After such a change, re-resolve the chain, call ``tzset()`` as required
and invoke registered timezone callbacks; Lua then recomputes any local-time-derived absolute deadlines.

The built-in clock uses absolute realtime deadlines aligned to the smallest displayed unit for known ``strftime``-style formats:
minute-aligned for formats whose finest field is minutes, second-aligned when seconds are displayed. A custom formatter or ambiguous
format must provide an explicit ``precision``/``interval`` policy rather than relying on C to infer arbitrary calendar dependencies.

Clock/timezone invalidation callbacks are coalesced within an event batch and obey normal generation/lifetime/fairness rules.

If neither realtime deadlines nor clock-change observers exist, no realtime timerfd exists. If no local-time consumer exists, no
timezone watch exists.

File Watches
------------

Use one shared nonblocking ``inotify`` instance.

``ctx:watch_file(path, fn)`` watches the containing directory and filters events for the target basename rather than relying
solely on a watch attached to the file inode.

This must survive common editor save patterns such as:

.. code:: text

   write temporary file
   fsync
   rename over original

Relevant directory watches are shared between paths. The native watch is reference-counted and uses the union of event masks
required by its logical children; adding/removing one logical consumer must not accidentally replace or narrow another consumer's
mask.

Generic ``watch_file`` follows the directory-entry pathname, not the referent of an arbitrary symlink. It reliably detects
replacement/recreation of that pathname but does not promise to notice an in-place modification of a different file reached only
through a symlink.

Core consumers requiring referent tracking use one shared *resolution-chain watch* helper. It resolves the logical path
component-by-component, records every symlink-bearing directory entry encountered (including symlinked ancestor directories), watches
the parent directory of each such entry plus the final referent's containing directory, and records the final referent identity. Any
relevant component/referent event invalidates the whole chain, which is re-resolved and re-armed from the logical path. Resolution is
bounded by the implementation symlink-hop limit. This helper is used by configuration dependency tracking and automatic timezone
tracking; it is not silently implied by generic ``ctx:watch_file``.

A logical file watch created during configuration staging is armed during activation before candidate callback dispatch becomes
possible. Staged render state is never publishable under ``RLD-003``, but a watch may still have been preceded by validation-time
sampling or an ``init``-time observation. Therefore every newly activated logical file watch is marked dirty for one authoritative
resnapshot callback in the publication-prerequisite phase after live-refresh and **before the first candidate render**. Any real
inotify event observed during activation or the live-refresh phase coalesces with that callback. This closes the snapshot-to-watch
race without polling; subsequent changes after that resnapshot are normal live events.

File-change bursts may be coalesced into one callback/reload using a short one-shot timer; there is no periodic polling
fallback.

``IN_Q_OVERFLOW`` invalidates assumptions derived from all inotify event streams. On overflow, revalidate every logical file
watch and invoke each affected logical watcher at most once to force consumers to resnapshot current state.

If a directory watch becomes invalid through ``IN_IGNORED``, deletion, replacement or unmount:

- immediately try to re-establish it if the directory exists;
- otherwise track the nearest existing ancestor needed to notice path recreation and re-arm the target watch as components
  reappear;
- report the logical file as unavailable/changed once rather than silently retaining stale state.

Recovery remains event-driven; no periodic file-watch polling loop is introduced.

Generic D-Bus Integration
-------------------------

When built with D-Bus support, use ``libsystemd``'s ``sd-bus`` API as the native transport.

``sd-bus`` is used as a general D-Bus implementation, not as a systemd-specific module.

Connections are created lazily and shared by bus scope:

.. code:: lua

   local bus = ctx:dbus("system")
   local bus = ctx:dbus("user")

No D-Bus connection exists if no configured block requires it.

Connection establishment/authentication and message processing are driven by ``sd_bus_process()`` through the main event
loop. Do not use synchronous remote operations on the event-loop path. In particular, use asynchronous match/call operations
such as ``sd_bus_add_match_async()``/``sd_bus_call_async()`` rather than ``sd_bus_add_match()``/``sd_bus_call()`` where the
operation can require broker/peer round trips.

The Lua API is asynchronous only.

Example:

.. code:: lua

   local match = bus:match({
       sender = "org.example.Service",
       path = "/org/example/Object",
       interface = "org.example.Interface",
       member = "Changed",
       arg0 = "example",
   }, function(msg)
       ...
   end, function(err)
       -- Called when the broker-side match installation completes.
       ...
   end)

   local call = bus:call({
       destination = "org.example.Service",
       path = "/org/example/Object",
       interface = "org.example.Interface",
       member = "GetState",
       signature = "",
       args = {},
       timeout = 5,
   }, function(reply, err)
       ...
   end)

Both return cancellable handles owned by the block/generation.

The core always supplies its own native installation callback to ``sd_bus_add_match_async()``. It must never rely on sd-bus's
default callback behavior, because failure of one logical match on a shared connection must fault only that registration/dependent
block rather than tear down the shared bus. The optional third Lua callback merely observes the asynchronously reported
installation result. It is invoked after the broker has accepted/failed the match installation, and again after each
reconnect/reinstallation. A module that relies on a match to close a snapshot race must not begin the susceptible
subscribe/snapshot sequence until the required matches report successful installation. Merely creating the logical match handle is
insufficient.

A connection-state callback may be registered for protocols requiring per-connection initialization:

.. code:: lua

   local h = bus:on_connect(function()
       ...
   end)

``on_connect`` also returns a cancellable block/generation-owned handle and is invoked once for each native connection epoch while
the registration is live. When a registration becomes live while the shared bus is already connected, schedule one callback for the
current native connection epoch under the normal generation/callback gating; it does not wait for a future reconnect. If that epoch
changes before the callback is dispatched, suppress the stale notification and deliver the notification for the new current epoch
instead. ``on_connect`` does not imply that asynchronously restored broker-side match rules are already installed; modules that depend
on that ordering use the match installation callbacks.

Match rules should expose standard useful broker-side filters rather than forcing Lua to wake and discard unrelated
messages. Support at least:

- ``sender``;
- ``path`` and ``path_namespace``;
- ``interface``;
- ``member``;
- ``destination`` where meaningful, respecting the underlying D-Bus requirement for a unique destination name;
- ``argN`` string equality filters for supported indexes (initially ``0..63``);
- ``argNpath`` path-style matching where supported by the underlying D-Bus match syntax;
- namespace/path variants supported by the underlying D-Bus match syntax where useful.

Messages expose signature-aware decoding rather than native ``sd_bus_message`` pointers. A message object is a callback-scoped
view: it is valid only while its delivering callback is executing and cannot be retained for later native access. ``msg:read()``
is transactional with respect to its requested signature: on success it advances past the complete requested value sequence; on
signature/type/decode failure it returns ``nil, err`` and leaves the cursor at its pre-call position. Values returned on success are
copied/owned values that remain valid independently. Retaining a Lua wrapper therefore cannot pin an arbitrary native D-Bus message
after callback return. Invalid caller-supplied signature syntax is a configuration/programmer error and raises.

For example:

.. code:: lua

   local id, path = msg:read("so")

D-Bus marshalling must:

- require explicit signatures rather than runtime introspection;
- support basic types, arrays, structs, dictionaries and variants needed by modules;
- validate signatures, object paths, interface/member/bus names and argument counts;
- represent ``x``/``t`` values as exact LuaJIT ``int64_t``/``uint64_t`` cdata rather than lossy Lua numbers;
- represent ordinary arrays as ordered 1-based Lua sequences;
- represent structs as ordered 1-based Lua sequences with arity determined by the signature;
- represent ``ay`` as a copied binary Lua string, including embedded NUL bytes, rather than one Lua number per byte;
- represent dictionaries on decode as ordered sequences of two-element ``{ key, value }`` entries. Encoding accepts the same canonical
  pair-sequence representation, optionally constructed by ``dbus.dict(entries)``; native Lua table hash iteration is never a wire-order
  contract and all D-Bus-legal *basic* dictionary-key types supported by this API are supported without forcing them into Lua table
  keys; in API v1 this excludes UNIX-fd type ``h``;
- represent variants explicitly as an object created by ``dbus.variant(signature, value)`` and preserve the contained signature;
- copy decoded values that escape ``msg:read()`` so they remain valid after the callback, while the message cursor/wrapper itself stays
  callback-scoped;
- enforce the documented message, value-count and container-depth limits before unbounded recursive allocation.

UNIX-fd type ``h`` is unsupported in the initial API unless explicit duplication, ownership and lifetime rules are added.

Cancelling an unsent call prevents transmission. Cancelling an already-sent call suppresses its Lua completion callback and
releases local resources; it does not imply that the remote method execution can be cancelled.

Normal method-call expiry is owned by ``sd_bus_call_async()``/sd-bus using the requested timeout. Its absolute monotonic deadline is
already reflected by ``sd_bus_get_timeout()`` and therefore participates in the core's nearest-deadline calculation without creating
a duplicate per-call timer. A future wrapper-level deadline stricter than the transport timeout must be named separately and must
resolve cancellation/completion races explicitly. Calls that fail because the connection was lost are completed with an error and
are **not** automatically replayed after reconnect; service-specific code may retry only when it knows the operation is safe/idempotent.

The low-level D-Bus interface is intended primarily for modules; ordinary configuration should normally use higher-level
wrappers.

Process-Shared Remote Leases
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The generic D-Bus layer exposes a narrow process-shared lease primitive for connection-scoped remote enable/disable state. It is not
systemd-specific:

.. code:: lua

   local lease = bus:shared_lease({
       key = "org.freedesktop.systemd1.Manager.Subscribe",
       acquire = { destination=..., path=..., interface=..., member="Subscribe", signature="", args={} },
       release = { destination=..., path=..., interface=..., member="Unsubscribe", signature="", args={} },
       acquire_success_errors = { "org.freedesktop.systemd1.AlreadySubscribed" },
       timeout = 5,
   }, function(state, err)
       -- state: "waiting", "ready", or "error"
   end)

   lease:set_gate(true) -- acquisition prerequisite satisfied, e.g. required matches installed
   lease:cancel()

The returned handle is block/generation-owned; the underlying coordinator follows ``SHR-001``. ``key`` is interpreted only within
one bus scope/transport. The first user fixes the copied native acquire/release specification for that key; a later user presenting a
non-identical specification is a configuration error rather than silently sharing incompatible semantics.

During staging the handle is dormant: ``set_gate`` records desired gate state but sends nothing. Commit activates candidate lease
references before old-generation lease cancellation. A transition from zero active leases to nonzero active leases sends ``acquire``
only when at least one active lease has its gate true. Once remotely ready, the coordinator remains ready while any active lease
exists even if all current gates temporarily become false; gates prevent unsafe *acquisition*, not retention of already-established
remote state. Transition to zero active leases sends ``release`` asynchronously.

This primitive is only for **convergent enable/disable methods** whose acquire/release operations may safely be retried to establish a
desired boolean remote state. It is not a generic method-call deduplicator. The coordinator's acquire/release calls are internal native
control operations: cancellation of the last logical lease never discards an already-sent acquire completion. If desired count becomes
zero while acquire is in flight, observe its completion; successful acquisition immediately drives release. If desired count rises
while release is in flight, observe release completion and reacquire when a live lease gate is ready. A timeout/disconnect makes the
remote state ``unknown``; a connection-epoch change discards old-epoch completions, while same-epoch transient failures use bounded
backoff/retry appropriate to the desired state. Declared acquire success errors (such as ``AlreadySubscribed``) converge to ready.
Persistent release failure is rate-limited/bounded and never forces unrelated users off a shared bus solely to obtain cleanup.

The coordinator's completion fan-out is generation checked. It retains no Lua callbacks itself: logical lease subscriptions hold the
Lua callback references and receive state changes only while their generation is current. Connection replacement resets the remote
state for the new native epoch; active gated leases then reacquire once prerequisites for that epoch are satisfied.

D-Bus Event-Loop Integration
----------------------------

Each active ``sd_bus`` connection contributes its current:

- fd;
- requested read/write readiness;
- absolute monotonic timeout.

The core integrates these directly into the existing event loop.

Before every sleep, and again after processing/reconnect work, query ``sd_bus_get_fd()``, ``sd_bus_get_events()`` and
``sd_bus_get_timeout()`` as adapter state ``(native_connection_epoch, fd, events, deadline)``. The native connection epoch increments
whenever the underlying ``sd_bus *`` is replaced/restarted. Reconcile the whole tuple, not merely the integer fd.

Every epoll registration receives a fresh nonzero 64-bit core registration cookie stored in ``epoll_event.data.u64`` and mapped to a
live registration object. Cookies are never reused during one process lifetime; exhaustion/wrap is a fatal internal condition rather
than permission to alias a stale event. Retirement invalidates/removes the cookie mapping before ``EPOLL_CTL_DEL``/close; an
already-harvested stale event therefore fails cookie lookup and is ignored. Any native-epoch change forces retirement of the old
registration and a fresh
``EPOLL_CTL_ADD`` even when Linux immediately reused the same fd number. ``EPOLL_CTL_MOD`` is used only when the registration epoch
and owning native object are unchanged and only the interest mask changed.

Unexpected ``EPOLL_CTL_ADD`` ``EEXIST`` or ``EPOLL_CTL_MOD`` ``ENOENT`` is never repaired by blindly assuming integer-fd identity.
First verify that the source's registration/native epoch is still current; otherwise discard the stale reconciliation attempt. For a
current source, rebuild its registration with a fresh cookie according to the source-specific ownership path, and treat irreconcilable
``epoll_ctl`` state as a source/internal error rather than risking dispatch to the wrong object. ``UINT64_MAX`` means no bus deadline
and a timeout of zero means immediately runnable.

Incorporate the returned absolute monotonic D-Bus deadline directly into the nearest event-loop deadline.

After bus readiness or timeout, call ``sd_bus_process()`` repeatedly while it reports immediate work, but enforce a finite
per-source work budget per event-loop turn so a continuously busy bus cannot starve timers, stdin/stdout or other fds.

If the budget is exhausted, mark the bus immediately runnable for the next loop turn rather than sleeping. Then recalculate
its fd interest and timeout.

Do not use ``sd-event``.

Do not use synchronous D-Bus calls from Lua callbacks.

Bus match registrations and callbacks obey the same block/generation lifetime rules as every other watcher.

On connection loss:

- pending calls fail cleanly;
- matches remain represented as logical registrations;
- destroy/close the disconnected native ``sd_bus *`` and create/start a fresh native bus object for the next transport attempt; the
  shared logical Lua-facing bus object survives, but native connection objects/slots/fds do not;
- prefer ``sd_bus_set_watch_bind()``/equivalent event-driven socket reappearance behavior where applicable;
- otherwise reconnect using bounded exponential backoff through the existing timer heap;
- invoke registered ``on_connect`` callbacks when the transport reconnects;
- restore match registrations asynchronously and deliver their installation-completion callbacks;
- let service-specific modules begin any match-dependent subscribe/snapshot sequence only after the required match installs succeed;
- let service-specific modules resnapshot state when required.

No fixed-frequency reconnect polling loop is used.

Close an unused shared bus connection when nothing references it.

Event Loop
----------

The core maintains one ``epoll`` instance, registration-cookie table and explicit queue of runnable logical sources.

``EVT-001`` requires a **kernel readiness harvest at least once per turn**. A blocking ``epoll_pwait2()``/``epoll_wait()`` return that
begins a new turn counts as that turn's harvest. If a new turn begins only because userspace work was carried forward from the previous
turn, perform a zero-timeout epoll wait first. Thus a permanently runnable userspace source can prevent sleeping but cannot prevent
new signalfd/stdin/inotify/socket/stdout readiness from entering the runnable set.

Typical loop:

.. code:: text

   have_fresh_epoll_batch = false

   repeat:
       begin event-loop turn

       refresh adapted-source native-epoch/fd/event/deadline registrations

       if not have_fresh_epoll_batch:
           epoll_pwait2(..., timeout=0)   # readiness harvest, never a sleep
           enqueue/mark returned registration cookies runnable
       have_fresh_epoll_batch = false

       service guaranteed control work (signals, stdin clicks, stdout progress, reload control)
       dispatch runnable logical sources in rotating/round-robin order within per-source/global budgets
       process due monotonic timers
       process due wall-clock timers
       process pending live-refresh/publication-prerequisite/resynchronization/scan/snapshot continuations
       detect any newly-observed suspend gap via CLOCK_BOOTTIME - CLOCK_MONOTONIC

       if render_pending and render throttle permits and no partial frame prevents replacement:
           render/replace the unwritten frame once

       flush pending stdout while writable within its byte budget
       refresh adapted-source native-epoch/fd/event/deadline registrations

       if any immediate core work remains:
           continue                       # next turn will first harvest with timeout=0

       compute nearest:
           monotonic deadline, including render-throttle deadline
           D-Bus/journal/other adapted-source monotonic deadlines

       epoll_pwait2(..., timeout)
       enqueue/mark returned registration cookies runnable
       have_fresh_epoll_batch = true      # returned readiness is next turn's harvest

The blocking wait is replaced by ``epoll_wait()`` with the documented rounded-up timeout on kernels lacking ``epoll_pwait2()``;
the zero-timeout harvest uses the same selected backend. Harvesting processes only the bounded kernel event array returned by one
wait call; if more readiness remains, level triggering/continuation causes subsequent turns to harvest it without an unbounded drain.

The fundamental reactor invariant is: **never sleep while immediate core work remains, and never execute an entire new turn without
harvesting kernel readiness**. Due timers, runnable adapted sources, live-refresh/publication-prerequisite/resynchronization continuations, pending reload
work, a render whose throttle deadline has arrived, and writable buffered stdout all count as immediate work. This guarantees both
that the initial committed frame cannot be postponed indefinitely by an unbounded sleep and that continuous userspace work cannot
starve newly ready fds that are waiting in epoll.

One **event-loop turn** begins immediately before its readiness harvest/service and ends after at most one coalesced render/flush
opportunity before the next turn or sleep. One **event batch** is the set of state changes accumulated during that turn; work deferred
because a fairness budget was exhausted belongs to a later turn/batch. A source becoming immediately runnable again therefore cannot
extend one batch forever or postpone rendering indefinitely.

Multiple block changes within one batch result in at most one render attempt.

Use level-triggered epoll.

All owned descriptors are ``O_NONBLOCK`` and ``FD_CLOEXEC``.

All native registrations obey ``FD-001``. Integer fd values are transport details, not lifetime identities. Source teardown first
makes its cookie undispatchable, then removes/closes the underlying registration/resource; callback dispatch always resolves the
cookie to a currently live object before reading source state.

No single always-ready source, nor a fixed prefix of many busy sources, may monopolize event-loop turns. Budgets are deterministic
work counts/bytes rather than wall-clock time: each native/adapted source defines an item budget and, for byte streams/dumps, a byte
budget. Initial defaults are at most 256 logical items/callbacks and 1 MiB consumed per source per turn, plus global ceilings of 4096
dispatched logical items and 4 MiB of byte-stream/dump input per turn. A source may use a smaller backend-specific bound. Timer
callbacks count as items; kobject-uevent/D-Bus/journal messages count as items and bytes; a resumable scan/snapshot counts parsed
records and bytes.

``EXT-001`` is the explicit exception to callback-level preemption: when a foreign library exposes only one indivisible nonblocking
iteration, **one iteration is one foreign dispatch quantum/item** regardless of how many library callbacks it invokes internally.
The core bounds the number of such quanta per turn and can rotate to other sources between quanta, but it does not claim the normal
256-callback ceiling inside one quantum. A backend whose single nonblocking quantum has unacceptable/unbounded real-world latency does
not qualify for in-process integration and must use an external helper.

Ready/runnable sources are serviced through a persistent rotating queue (or equivalent fair cursor). If a source exhausts its quantum
and still has immediate work, preserve explicit continuation state and append/reposition it behind other runnable sources for a later
turn. The dispatch cursor is not reset to the same source on every turn, so a continuously busy set cannot permanently starve a
source that appears later in registration/epoll order. The mandatory per-turn readiness harvest additionally ensures that sources
which become ready *after* the busy set was already queued can join that rotation.

Control-plane work required for liveness/correctness -- at minimum signal handling, stdin click framing, stdout progress and reload
control -- receives a small guaranteed service opportunity even if the normal global source budget is exhausted. Rendering/stdout
handling is still reached before beginning the next normal-source turn. Remaining runnable work resumes without sleeping.

Ordering between independently ready sources is intentionally unspecified; modules must not use cross-source dispatch order as a
correctness mechanism. Timers with exactly equal deadlines are dispatched in registration-sequence order, and logical subscribers of
one shared native source are dispatched in their stable registration order unless that source documents a stronger semantic order.

Signals
-------

Block normal process signals and receive them through one ``signalfd`` where possible. To satisfy ``SIG-001``, build and install
this blocked signal mask in the initial process thread **before** initializing LuaJIT or any optional library that may create an
incidental thread. POSIX-created threads inherit the creator's signal mask; no supported in-process backend may deliberately unblock
these process-control signals in another thread. Create/register ``signalfd`` after the mask is installed and before entering normal
runtime initialization.

Handle at least:

- ``SIGINT``: clean exit;
- ``SIGTERM``: clean exit;
- ``SIGHUP``: configuration reload;
- ``SIGCONT``: resume-resynchronization barrier;
- ``SIGPIPE``: clean handling of disconnected i3bar/stdout.

Do not interfere with i3bar's normal stop/resume mechanism. ``SIGSTOP`` itself cannot be caught or blocked; after a later
``SIGCONT`` the process resumes and can consume the pending/blocked ``SIGCONT`` through ``signalfd``.

A ``SIGCONT`` requests a bounded full resynchronization of state that may have become stale while the process was stopped.
System suspend is detected by the ``CLOCK_BOOTTIME - CLOCK_MONOTONIC`` gap check on the first natural wake after resume and, when
enabled/available, by the post-sleep logind ``PrepareForSleep`` transition. Any of these conditions requests the same
resynchronization path:

- polling measurements are sampled once promptly, while cumulative-rate consumers invalidate old baselines and use that first
  sample only to seed new baselines;
- stateful native sources are resnapshotted where their backend requires authoritative recovery;
- power-supply/sysfs inventory is revalidated where applicable;
- service-specific event-driven modules such as systemd are asked to resnapshot;
- ordinary repeating timers still skip missed process-stall intervals rather than catching up repeatedly.

Resynchronization requests from multiple causes are coalesced.

This treats resume as a correctness boundary: queued notifications may be consumed, but cached state is not assumed to be
complete merely because no explicit overflow was observed.

Native Event Sources
--------------------

Create kernel/native event sources lazily.

Examples:

- kobject uevent source only if a configured module benefits from it;
- realtime timerfd only if wall-clock timers exist;
- system/user D-Bus connection only when requested;
- optional integration libraries only when corresponding modules are active.

No unused subsystem should introduce an fd or wakeup.

Stateful event sources follow a common model:

#. obtain an authoritative initial snapshot;
#. reconcile notifications arriving while the snapshot is in flight;
#. atomically publish the completed snapshot/cache generation;
#. apply incremental events;
#. on detected loss, reconnect, ``SIGCONT``, detected suspend gap or other uncertainty, discard assumptions and obtain a
   fresh snapshot.

A large snapshot/dump must obey the event-loop work budget and may continue across turns. Do not expose a half-built replacement
cache as authoritative state; keep the old cache explicitly stale or keep the module hidden until the new snapshot is complete. The
candidate authoritative cache itself is record-count/byte-bounded by the implementation limits; hitting that bound produces the
defined ``too_large``/resource-limit source state rather than publishing a truncated snapshot or growing without bound.
Incremental events that must be retained for reconciliation are stored in a count/byte-bounded backlog. If that backlog would
overflow, abandon the candidate snapshot, mark the source uncertain and schedule a fresh authoritative snapshot instead of growing
memory without bound. Sources that do not require exact event replay should prefer a coalesced ``dirty_while_snapshot`` flag and a
single follow-up snapshot over retaining every event. Queued events, when used, are reconciled before publication.


Kobject-uevent consumers similarly treat detected receive loss or resume as a reason to re-enumerate the relevant sysfs
state rather than assuming every change notification was retained.

Pressure Stall Information
--------------------------

Linux PSI is a first-class native event source for exception-driven CPU, memory and I/O pressure warnings.

A ``watch_pressure`` registration opens the relevant ``/proc/pressure/{cpu,memory,io}`` file ``O_RDWR|O_NONBLOCK``, installs one
kernel threshold trigger and registers the resulting fd with the main ``epoll`` loop for ``EPOLLPRI`` (plus error/hangup handling).
No periodic sampling is required merely to detect threshold crossing.

Each kernel trigger fd represents one ``(resource, mode, stall, window)`` tuple. The core may share exactly identical tuples but
must not combine semantically different triggers.

A trigger callback means "pressure crossed the configured event condition"; consumers that want current averages should obtain a
fresh PSI snapshot rather than inferring them from the notification alone.

Pressure-trigger fds are closed when their last logical registration disappears. Reconnect logic is unnecessary because procfs PSI
files are local kernel interfaces; read/write/poll errors fault only dependent blocks and are retried only through reload or an
explicit module policy.

Validate trigger parameters before installation. The Linux PSI interface accepts tracking windows from 500 ms through 10 s, and
unprivileged system-wide monitors require a window that is a multiple of 2 s; kernel rejection is surfaced rather than silently
changing the requested condition. System-wide CPU ``full`` pressure is not a meaningful supported metric even on kernels that
report a compatibility zero, so the high-level module must not offer it as a usable pressure signal.

PSI snapshots expose ``some``/``full`` ``avg10``, ``avg60``, ``avg300`` and cumulative ``total`` values where the kernel provides
them. Unsupported/non-meaningful fields remain unavailable rather than becoming zero-valued capabilities.

Native Collectors
-----------------

Native collectors expose measurements to Lua without subprocesses.

Baseline collector families:

- CPU counters: ``/proc/stat``, including user/system/idle/iowait/steal fields needed for per-consumer utilization and iowait
  deltas;
- load: ``sysinfo(2)`` or ``/proc/loadavg`` where required, plus the online logical-CPU count for optional normalization;
- memory/swap: ``/proc/meminfo``, including ``MemAvailable``, ``SwapTotal`` and ``SwapFree``;
- PSI snapshots: ``/proc/pressure/cpu``, ``memory`` and ``io`` where available;
- filesystems: ``statvfs(2)`` for explicitly configured/expected-local filesystems;
- block-device I/O: ``/proc/diskstats`` plus ``/sys/block`` inventory metadata;
- power/battery values: ``/sys/class/power_supply``;
- temperatures: ``/sys/class/hwmon``;
- clock/time information: native time APIs;
- local file metadata needed by event-driven age/existence modules: ``statx(2)``/``fstatat(2)`` where appropriate.

Optional/capability-dependent collector families may include:

- CPU frequency from cpufreq sysfs when exposed by the active driver;
- package/SoC energy counters from the Linux powercap/RAPL hierarchy where exposed;
- bounded process statistics from ``/proc/<pid>`` for an opt-in process-hog module;
- hardware-specific GPU metrics through optional backends described below.

Collector snapshots distinguish at least unavailable/unsupported data from valid numeric zero. Hardware-facing snapshots should
carry enough capability/backend metadata that Lua can avoid displaying meaningless fields.

Potentially large or cumulative integer counters are preserved losslessly across the C/Lua boundary, using LuaJIT 64-bit cdata
where an ordinary Lua number cannot represent the full native range exactly. Percentages and derived rates may use Lua numbers.
Units are explicit and stable within each collector API; raw byte counters are bytes, rates are bytes/second, and percentages use
``0..100``.

Stable pseudo-files may remain open and be rewound/read on subsequent samples when safe.

Dynamic sysfs/proc objects must tolerate disappearance/recreation and reopen as necessary.

If several blocks request the same equivalent underlying source during one event-loop batch, sample it once and fan the raw
snapshot out rather than rereading it for every block.

Collectors for cumulative quantities expose raw cumulative counters. Per-consumer deltas/rates belong to each block/module
instance so blocks with different intervals do not corrupt one another's baselines. This applies to CPU counters, block-device counters, powercap energy counters and similar future sources. Such modules begin hidden/unavailable until two valid
samples establish a delta. Counter reset, wraparound, device replacement, changed source identity, changed collector continuity
token or an unexpectedly long sampling gap invalidates the consumer baseline. The next valid sample seeds a new baseline rather
than producing zero, a spike or a long-period average.

The baseline aggregate ``cpu`` module uses deltas from ``/proc/stat``. For each sample, ``total`` is the delta of the non-guest CPU
fields ``user + nice + system + idle + iowait + irq + softirq + steal``; guest/guest_nice are excluded because they are already
accounted in user/nice. Headline busy time is ``total - idle - iowait`` and percent is ``100 * busy / total``. ``iowait`` remains
separately available as a diagnostic counter and is not treated as busy CPU time; it is not claimed to be a reliable measure of
storage contention. Counter regression, CPU-topology/source-identity change, zero total delta or continuity invalidation resets the
baseline instead of emitting a percentage. Per-core formatters use the same rule per ``cpuN`` entry and tolerate hotplug.


Filesystem free/available-space warnings use user-available capacity: available bytes are ``f_bavail * f_frsize``. Percentage
policy must use an explicitly documented denominator (the baseline built-in uses total data blocks, ``f_blocks * f_frsize``), so
reserved blocks are not silently reported as ordinary-user free space. Zero/invalid filesystem sizes are unavailable rather than
division-by-zero or fabricated percentages.

Block-device I/O aggregation must define its layer explicitly. Automatic ``physical`` mode includes whole physical block devices
and excludes partitions, loop/ram devices and stacked virtual devices to avoid accidental double counting. Explicit device lists
measure exactly the requested devices/layers. Sector counters from the Linux block statistics interface are converted using the
kernel-defined 512-byte sector unit.

Power-supply collection should expose charging/discharging/full status and raw charge/energy/current/power values where available.
Battery health is derived only when comparable ``full`` and ``design`` capacity fields exist. Remaining-time estimates are derived
only when the required energy/charge and power/current data are available; otherwise the estimate is unavailable.

CPU package-power modules derive power from energy-counter deltas and account for the exported wrap range when the kernel exposes
it. CPU-frequency values are best-effort driver data and must be labeled/treated as such rather than claimed to be a universally
accurate instantaneous clock measurement.

Do not use inotify as a generic value-change mechanism for sysfs attributes.

Where reliable kernel events exist, use them to trigger rereads instead of polling. For example, power-supply
change/add/remove events may use kobject uevents.

Polling remains appropriate for measurements without reliable change notifications, including CPU utilization, throughput,
temperatures, free filesystem space, most frequency/power counters and many hardware-specific utilization metrics.

``statvfs(2)`` is synchronous and can block unpredictably on some remote/FUSE/autofs-style filesystems. The initial filesystem
built-in therefore targets explicitly configured/expected-local filesystems. Under ``LAT-001`` even supported local procfs/sysfs,
hwmon, ``statvfs`` and configuration reads receive bounded *work/byte* treatment but no hard wall-clock upper bound: local kernel or
hardware-backed sysfs operations may sleep. Local file-metadata/age helpers have the same expected-local-path constraint. Supporting
arbitrary potentially blocking filesystems or devices with strict responsiveness requires a genuinely asynchronous API or an
external helper and is outside the default single-loop design.

An opt-in process scanner is not treated like a cheap scalar collector. It must be rate-limited and bounded per event-loop turn;
large ``/proc`` directory scans are continued across turns so they cannot monopolize the loop. Per-process baselines are keyed by
PID plus process start-time identity so PID reuse cannot inherit an old process's counters. Inaccessible/disappearing PIDs are
normal scan outcomes. A process-hog module should normally activate detailed scanning only after a cheap aggregate condition
indicates sustained CPU pressure/load.

Use Cases and Module Policy
---------------------------

The intended default bar is mostly exception-driven rather than a permanently visible dashboard. Modules fall into three broad
presentation classes:

- always-useful compact state, such as clock, selected CPU activity or laptop battery;
- warning state, hidden while healthy and shown only after a threshold/event condition is met;
- transient state, shown for a bounded hold period after an event and then hidden again.

Warning modules should avoid boundary flicker with hysteresis. Multi-severity modules may expose warning/critical/emergency
thresholds. A module becoming unavailable is distinct from its measured value becoming zero.

The following use-cases drive the initial and optional module set.

Health / warnings
~~~~~~~~~~~~~~~~~

- ``systemd``: event-driven failed-unit tracking for system/user managers; normally hidden while the selected failed set is empty.
  Filtering/grouping by scope and unit type allows failed services and timers to be shown separately.
- ``filesystem``: slow polling for configured local filesystems; normally hidden until free space crosses configured thresholds.
- ``memory``: ``MemAvailable``-based usage/warnings with optional warning/critical thresholds; a sensible warning profile is
  roughly 20%/10%/5% available with hysteresis. Swap usage comes from the same snapshot and may be rendered separately; a typical
  warning policy can require both a meaningful percentage and/or absolute amount (for example >10% or >1 GiB) rather than showing
  harmless small swap use.
- ``pressure``: PSI-triggered CPU/memory/I/O contention warnings. For diagnosing actual memory or storage stalls, this is often a
  better exception signal than free-memory or CPU-iowait percentages alone.
- ``oom``: transient indication of newly observed OOM-related unit failure/result events where available. Coverage must be labeled:
  D-Bus/systemd state is not claimed to be a complete historical audit of every kernel OOM kill. An optional journal backend may
  broaden coverage when the user has permission.
- ``thermal_throttle``: optional backend-specific warning driven by throttle-counter deltas where the platform exports reliable
  counters; hidden when no new throttling is observed.
- ``battery``: event-triggered rereads from the power-supply subsystem; configurable as always visible or conditional on charge
  state/threshold. An ``auto`` profile may show charging/discharging state and low charge (for example <40%) while allowing a fully
  charged healthy battery to disappear. Remaining time is shown only when derivable.
- ``battery_health``: low-frequency/event-triggered warning derived from comparable full/design capacity fields; hidden when
  healthy or unsupported.

Machine activity
~~~~~~~~~~~~~~~~

- ``cpu``: sampled aggregate utilization; per-core details may be formatter/click-driven rather than permanently rendered.
- ``load``: load average with optional normalization by online logical CPU count. A default configuration should normally choose
  either persistent CPU utilization or persistent load rather than showing both.
- ``iowait``: derived from CPU counters and available as a diagnostic warning (for example hidden below ~5%), while PSI ``io`` is
  preferred when the intent is to detect user-visible storage contention.
- ``diskio``: current disk I/O read/write throughput from block-device counters with explicit physical-vs-logical device
  semantics; it may hide when throughput is practically zero.
- ``gpu``: optional capability-driven utilization backend; engine-specific utilization is exposed only when the backend provides
  it reliably.
- ``vram``: optional GPU-memory usage/warning; absent on architectures/backends where a dedicated VRAM quantity is not meaningful.
- ``cpu_frequency``: optional best-effort diagnostic display; normally hidden unless requested.
- ``cpu_power``: package/SoC power derived from powercap energy deltas where available.
- ``gpu_power``: optional backend-specific GPU power metric.
- ``system_power``: custom/local integration point for a UPS, smart plug or other authoritative *local* source exposed through
  D-Bus, a local fd/UNIX-domain socket or a watched state file. i3sd itself contains no remote/IP client for this use-case.
- ``process_hog``: opt-in process-hog sustained-CPU warning. Detailed process scanning activates only after a cheap aggregate
  trigger and is work-budgeted across event-loop turns.

Desktop / session state
~~~~~~~~~~~~~~~~~~~~~~~

- ``volume``: optional PipeWire integration, event-driven. A useful default is transient display for a few seconds after volume
  changes while keeping mute visible until cleared.
- ``microphone``: optional PipeWire integration that becomes prominent while a real capture stream/link is active, not merely
  because a source exists or is unmuted. Monitor/loopback sources are distinguishable/configurable.
- ``keyboard_layout``: optional X11/XKB event-driven layout/group display, useful only when more than one layout is configured.
- ``idle_inhibitor``: D-Bus/logind-oriented display of active sleep/idle inhibitors. If the platform API offers no reliable change
  signal, use a slow poll only while this module is configured rather than a global tick.

Clicks / detail
~~~~~~~~~~~~~~~

Built-ins may use click handlers to toggle compact/expanded in-bar detail, for example failed-unit names, per-core CPU detail or
per-device I/O, without spawning subprocesses. Launching arbitrary external inspection tools is deliberately not a built-in
requirement; users who want that behavior can implement an explicit custom action/module.

Arch / system maintenance
~~~~~~~~~~~~~~~~~~~~~~~~~

- ``updates``: optional libalpm-backed count against the currently available local sync databases. Refreshing those databases is
  outside i3sd; when another package-management workflow changes them, inotify triggers a recomputation.
- ``kernel_sync`` / ``reboot_required``: optional warning when the running kernel/relevant module tree and configured installed
  kernel package state are demonstrably out of sync. This is the Arch-specific "reboot required" condition the module can actually
  justify; avoid distro-independent guesses. The Arch implementation may use libalpm/package file ownership.
- ``orphans``: optional libalpm query, recomputed after local package-database changes rather than continuously polled.
- ``pacman_lock``: event-driven pacman-lock age watch of ``/var/lib/pacman/db.lck``. On every newly observed appearance, ``stat``
  the lock and define its *long-held* warning instant as the lock ``mtime`` plus the configured duration; schedule that absolute
  realtime deadline, or warn promptly if it is already past. Age alone does not prove the lock is stale. Disappearance cancels the
  deadline. Startup/reload uses exactly the same rule, so persistence semantics do not depend on whether i3sd witnessed creation.
- ``aur``: custom integration. The core/built-ins do not embed an AUR client.
- ``failed_timers``: presentation preset over the same shared systemd unit source, filtering failed ``.timer`` units separately from
  failed services; it must not create a duplicate D-Bus subscription/snapshot.
- ``backup_age``: watch an explicitly configured local success-stamp/state file and schedule the exact wall-clock deadline at which
  it becomes stale; file changes reschedule the deadline. No periodic age polling is necessary.
- ``smart``/``nvme_health``: optional health integration, normally very infrequent (for example 15–60 minutes) and warning-only
  when a safe in-process/asynchronous backend actually requires sampling. A backend must not wake a sleeping rotational disk by
  default; if non-waking health inspection cannot be guaranteed, require explicit ``allow_wakeup`` or leave that device unprobed.

Built-ins
---------

Baseline built-ins requiring no optional third-party integration library:

- CPU and load;
- memory and swap;
- PSI pressure;
- filesystems;
- block-device I/O;
- power/battery and battery health;
- temperatures;
- clock;
- local file age/existence helpers used by backup/pacman-lock style modules.

Optional built-ins/integrations:

- systemd failed units and logind inhibitor state through generic D-Bus support;
- CPU frequency and powercap package power;
- GPU/VRAM/GPU-power backends;
- thermal-throttle backends;
- PipeWire volume/capture state;
- X11/XKB keyboard layout;
- libalpm update/orphan/kernel-maintenance state;
- OOM/journal enrichment;
- process-hog scanning;
- SMART/NVMe health.

Built-in modules should provide sensible defaults while allowing a Lua formatter:

.. code:: lua

   cpu {
       interval = 2,

       format = function(v)
           return ("%d%%"):format(v.percent)
       end,
   }

The low-level native collector should not impose presentation policy. Optional modules must be absent/lazy when their dependency or
capability is not configured; merely compiling support must not add idle fds or wakeups.

Systemd Module
--------------

Systemd monitoring is implemented in Lua over the generic D-Bus API.

No systemd-specific event loop is added to C.

Supported scope:

.. code:: lua

   systemd {
       scope = "system", -- "system", "user", or "both"
       types = nil,      -- e.g. { "service", "timer" }; nil means all unit types
   }

Type filtering is presentation/state policy in Lua; the shared manager source should retain enough unit metadata that multiple
blocks can render separate service/timer counts without creating duplicate signal matches/snapshots inside one generation.

``Manager.Subscribe`` is scoped to the underlying D-Bus client connection, so the module uses the generic
``bus:shared_lease``/``SHR-001`` mechanism keyed per manager scope. Every live systemd manager source owns one generation-scoped
logical lease; the process-shared native coordinator carries the desired count and remote state across generation replacement and
native connection epochs without carrying Lua references. The systemd acquire specification calls ``Subscribe`` and treats
``AlreadySubscribed`` as convergence; the release specification calls ``Unsubscribe``. The shared state machine reconciles desired
lease count with the remote subscription state asynchronously:

- ``0 -> 1`` establishes ``Subscribe`` after the required signal matches are confirmed installed; success and
  ``AlreadySubscribed`` both mean ready;
- ``N -> N+1`` reuses the existing connection-scoped subscription;
- ``1 -> 0`` requests ``Unsubscribe`` so systemd is not left generating a subscribed signal stream solely because an unrelated D-Bus
  consumer keeps the bus connection alive;
- if a new lease appears while ``Unsubscribe`` is in flight, the state machine converges back toward subscribed after that
  completion, issuing the new ``Subscribe`` as soon as at least one active lease's match/acquisition gate is ready;
- native connection loss resets remote subscription state regardless of logical leases; reconnect installs matches and subscribes once
  for the fresh connection epoch.

A candidate creates its logical lease while staging, but that lease is dormant and has no remote effect. Pointer commit activates
candidate lease references before old-generation lease references are released, so same-transport handoff changes the coordinator's
desired count ``1 -> 2 -> 1`` and cannot transiently flap ``Unsubscribe``/``Subscribe``. Match-install completion drives that lease's
acquisition gate. Manager reload completion, ``SIGCONT`` and suspend/resume resynchronization do not alter the lease count; they
request only an authoritative state resnapshot.

For each selected manager and native connection epoch:

#. establish/reuse the D-Bus connection;
#. create the required signal matches and wait for their asynchronous broker-installation callbacks to succeed;
#. mark that connection epoch's logical systemd lease acquisition gate ready, then wait until the process-shared subscription
   coordinator reports ready (or reuse its already-ready state);
#. request an initial snapshot with ``ListUnitsFiltered({"failed"})`` where available;
#. if that method is unavailable, fall back to ``ListUnits`` and filter locally;
#. maintain failed-unit state keyed by manager scope plus unit object path;
#. update rendered state only when that set or its displayed metadata changes.

Relevant events include:

- ``UnitNew``;
- ``UnitRemoved``;
- ``org.freedesktop.DBus.Properties.PropertiesChanged`` for unit ``ActiveState`` and any additional result metadata requested by
  dependent modules;
- manager ``Reloading(bool)`` lifecycle signaling; transition to ``Reloading(false)`` after a reload is an uncertainty boundary and
  triggers an authoritative resnapshot rather than assuming the pre-reload object/cache set is still complete.

The shared unit state should retain at least scope, canonical unit id, object path, unit type, active state and substate.
Service/result-specific metadata such as ``Result=oom-kill`` is fetched/subscribed only when a dependent module needs it.

Signal matches must be confirmed installed before the connection-subscription/snapshot sequence. Once the connection is known
subscribed -- either by a successful ``Subscribe`` reply or ``AlreadySubscribed`` -- signals received while the snapshot call is in
flight are queued/reconciled.

Events arriving while the initial snapshot is in flight are queued/reconciled so changes cannot be lost between subscription
and snapshot establishment. This uses the common bounded reconciliation policy: if the backlog limit is reached, abandon that
snapshot and obtain a fresh authoritative one rather than retaining an unbounded signal queue.

Once the snapshot arrives:

#. populate current state;
#. reconcile queued events against it;
#. switch to normal live event processing.

Do not key the failed set solely by unit name: one underlying unit may have aliases, and system/user managers may contain the
same visible name. Preserve suitable display metadata separately from the stable scope/object-path identity.

For ``PropertiesChanged``, process both the changed-property dictionary and ``invalidated_properties``. If a property used by the
cache/presentation (for example ``ActiveState``, ``SubState`` or requested ``Result`` metadata) is invalidated without a replacement
value, mark that field unknown and issue the required asynchronous property read or resnapshot; absence from the changed dictionary
never means the invalidated cached value remains authoritative.

A newly announced unit whose state is not yet known may require an asynchronous property read.

On D-Bus reconnection, install the new connection's matches, establish subscription for that fresh native connection and perform a
new authoritative snapshot. Manager reload completion, ``SIGCONT``, detected suspend gaps and explicit source-resynchronization
requests keep the existing live connection subscription and perform only the authoritative resnapshot/reconciliation phase.

There is no periodic systemd polling.

Journal Integration
-------------------

Optional journal support uses ``libsystemd``'s ``sd-journal`` API as a narrow event source for modules such as recent OOM
notifications. It is lazy and permission-dependent; failure to access the requested system/user journal faults or degrades only the
dependent module.

Integrate the journal's pollable fd, requested events and timeout into the core event loop, analogous to the D-Bus adapter and with
its own native-instance epoch/epoll cookie under ``FD-001``. After readiness/timeout, call ``sd_journal_process()`` and process journal
changes with a finite work budget. ``SD_JOURNAL_NOP`` adds no work; ``SD_JOURNAL_APPEND`` continues incremental following.
``SD_JOURNAL_INVALIDATE`` obeys ``JRN-001``: mark the current cursor/window cache uncertain, discard assumptions that the previously
opened journal-file set is complete, and start a bounded authoritative reseek/rebuild using the module's configured current-boot and
recent-time/cursor window. Do not publish a half-rebuilt replacement as authoritative; keep prior data explicitly stale or hide the
dependent transient source until rebuild completes. Cursor testing may suppress duplicates where valid, but invalidation recovery
must not depend on a cursor that is no longer accepted by the changed journal set.

Do not scan an unbounded historical journal on the event-loop path. A recent-event module selects the current boot and a bounded
initial time/cursor window, then follows new entries. Rotation/vacuum/invalidation and resume uncertainty reuse the same bounded
rebuild path rather than assuming append-only history.

Install the narrowest available journal matches before following entries. Kernel OOM text without stable structured identifiers may
require conservative pattern handling and must be labeled as best-effort; structured systemd/systemd-oomd events are preferred when
available. The module must not claim complete coverage when journal permissions or event sources do not provide it.

PipeWire Integration
--------------------

PipeWire support is optional and lazy. It is used for volume/mute and active-capture state.

Do not start a PipeWire thread loop and do not run a separately blocking PipeWire main loop. Create the minimal PipeWire loop/context
state needed by the library, call the library loop ``enter`` once on the core event-loop thread for the active integration lifetime,
register the fd returned by its loop implementation with the core ``epoll`` instance, and drive zero-timeout/nonblocking loop
iteration only when that fd is ready. Do not enter/leave around every readiness callback; leave once before destroying/replacing the
loop.

PipeWire follows ``EXT-001``: one successful zero-timeout ``spa_loop_control_iterate(..., 0)`` is one indivisible foreign dispatch
quantum. The core can bound the number of PipeWire iterations per turn and rotate after each iteration, but cannot preempt or claim a
callback-count bound *inside* one PipeWire iteration. If profiling shows that one nonblocking iteration can itself monopolize the
reactor under supported PipeWire behavior, the in-process backend is disqualified and must move behind an external local helper.

Connection loss hides/marks dependent state unavailable, reconnects with bounded backoff and rebuilds an authoritative registry
snapshot before live incremental events are trusted again.

The microphone/capture module distinguishes an actually active capture stream/link from a merely present or unmuted source. Backend
metadata should allow monitor/loopback sources to be excluded by default or configured explicitly.

X11/XKB and i3 Integration
--------------------------

Optional X11 integration uses XCB only. Register the XCB connection fd with the core event loop and drain events nonblockingly with
a finite work budget. Do not add Xlib or a separate X event loop.

Keyboard-layout state uses XKB extension events where available and takes an initial authoritative group/layout snapshot before
processing incremental changes. Reconnect/restart of the X server invalidates cached state and requires a new snapshot.

Do not use synchronous XCB reply waits on the event-loop path. Initial queries use request cookies plus nonblocking reply/event
processing; outgoing data is flushed without turning a missing reply into a blocking round trip.

Optional i3 IPC similarly uses one nonblocking UNIX socket integrated into ``epoll``. Its framing/parser must be bounded and, if
implemented in native C, receives the same parser/fuzzing treatment as click input. No i3 IPC connection exists unless a configured
module needs it.

GPU Backends
------------

``gpu`` is a Lua-facing logical module over optional native backends, not one assumed universal Linux ABI. Backends may include
DRM/sysfs facilities for supported Intel/AMD hardware and an optional NVIDIA management-library backend when present.

Each backend advertises capabilities independently, for example aggregate utilization, engine utilization, dedicated-memory usage,
temperature and power. A module requests only the capabilities it needs. Unsupported capabilities remain unavailable and do not
create polling deadlines.

Device hotplug, driver reset and suspend/resume invalidate cached GPU inventory/state and trigger rediscovery. Multiple blocks using
the same GPU/backend/interval share the raw sample while keeping rate/history state per consumer where necessary.

Arch/libalpm Integration
------------------------

Arch package-maintenance support is optional and uses ``libalpm`` rather than shelling out to ``pacman``/``checkupdates``.

``updates`` compares installed packages against the currently available local sync databases. Refreshing those databases is outside
i3sd.

Watch relevant local/sync database directories/files and ``/var/lib/pacman/db.lck`` with the shared inotify infrastructure. Database
changes mark package state dirty. While the pacman transaction lock exists, defer libalpm recomputation; when the lock disappears,
coalesce the remaining burst and recompute update/orphan/kernel-maintenance state once. Database changes not accompanied by the lock
use a short one-shot debounce fallback. Do not poll package state periodically merely to discover that no transaction occurred.

libalpm/database scans are opt-in, infrequent local work and fall under ``LAT-001``: their algorithmic work is bounded/coalesced,
but synchronous local database access has no hard wall-clock latency guarantee. If real-world latency is large enough to harm
responsiveness, move refresh/computation into an external helper that publishes a local event/file rather than adding background
worker infrastructure to the default core.

Local Maintenance Sources
-------------------------

``pacman_lock`` and ``backup_age`` are examples of modules that should compose file watches and one-shot deadlines instead of
polling:

- every observed lock-file appearance is ``stat``-based: define the long-held warning instant as ``mtime + configured_duration``
  and schedule one absolute realtime deadline; if already past, warn promptly; disappearance cancels the deadline. The warning means
  old/long-held, not proven stale;
- a backup-success timestamp determines one absolute realtime stale deadline; replacement/update of the stamp reschedules it;
- realtime clock discontinuity invokes the module's clock-change hook so civil-time/age policy can recompute if needed; timezone
  changes matter only to modules whose rule is defined in local civil time.

Optional SMART/NVMe health collection must be explicitly device-scoped and low-frequency. Backends must preserve drive power state
by default; a rotational device that cannot be queried without risking spin-up is skipped unless ``allow_wakeup`` is enabled.
Potentially blocking ATA/NVMe pass-through/admin commands do not belong directly on the default event-loop path. Prefer an existing
asynchronous service (for example through D-Bus) or an explicit external helper that publishes local state; only a backend with a
well-understood bounded local query may run in-process.

Polling Semantics
-----------------

Polling is used only for quantities that inherently require sampling or lack useful kernel events.

Each block has its own interval.

There is no global tick.

Blocks without polling intervals create no polling deadlines.

Intervals should be reasonably phase-aligned when instantiated together so equal or compatible periods share wakeups.

A block callback is never invoked merely because the event loop woke for an unrelated fd.

Rendering
---------

Each block has native typed i3bar state plus a cached canonical serialized JSON object fragment for its current visible state.

At configuration commit, construct a permanently ordered block vector sorted by:

#. descending ``order``;
#. declaration order for ties.

No sorting occurs in the normal render path.

``ctx:set`` validates the candidate typed state and serializes that one block into a bounded scratch fragment using the fixed
C-defined field order. Only after validation and the exact 7 KiB contribution check succeed are the typed state and cached fragment
replaced atomically. An unchanged effective state keeps the old fragment and requests no render. Hidden state retains no visible
fragment.

A render:

#. walks the pre-sorted vector;
#. skips hidden/empty blocks;
#. concatenates the already-canonical block fragments with array framing/commas into the pending frame buffer;
#. computes/checks the exact final frame size as a defensive invariant.

With at most 256 valid 7 KiB block contributions, the renderer's 2 MiB frame capacity cannot be exhausted by otherwise-valid block
states; aggregate overflow is therefore an invariant failure, not an update-order-dependent error. ``yyjson`` remains the block-object
serializer/parser utility, but unchanged blocks are not reserialized merely because another block changed.

Native i3bar separator lines are disabled. Inter-block spacing uses i3bar's native ``separator_block_width`` rather than synthetic
space blocks. The initial core spacing policy is explicit and deterministic; configuration authors do not set
``separator``/``separator_block_width`` through ``ctx:set``.

Each real block emits its configured ``name`` and an opaque 128-bit core-generated i3bar wire ``instance`` token used only for click
routing. Maintain process-lifetime maps in both directions between configured ``(name, key)`` identity and token. The first time an
identity appears, obtain 128 random bits from ``getrandom()``, encode them as a fixed 32-character lowercase hexadecimal token, and
retry if that token already belongs to any previously seen identity. ``key = nil`` and an empty string are distinct identities. A
previously seen identity reuses its token on later reloads. The maps remain for the process lifetime because i3bar gives no
acknowledgement that old frames can no longer generate clicks; their bounded entry count is part of the implementation limits. If a candidate introduces a never-before-seen identity after that
process-lifetime map is full, reject the candidate rather than evicting an old mapping and risking stale-click aliasing.

Lua callbacks and diagnostic APIs expose configured ``name``/``key`` rather than the opaque wire token. Click routing consults only
the current generation: both emitted ``name`` and ``instance`` must map to that current block. If the identity disappeared, the click
is ignored. If the same identity survived a reload, a click from an older still-displayed frame may invoke the current handler.
Configuration/modules that intentionally change click semantics incompatibly across a reload should change ``key``.

No Lua generation is retained solely for click routing. Once a replacement generation commits, the old generation is no longer
callback-routable and is destroyed as soon as current dispatch frames release it, independent of stdout/display progress. A partially
written old status frame remains immutable and is still completed for protocol correctness; any later click from that frame follows
the stable-current-identity rule above.

JSON fields are emitted in a fixed C-defined order. No rendering depends on Lua table iteration order.

Render Deduplication
--------------------

``OUT-001``/``OUT-002`` define the frame-memory model. The core owns at most two full status-frame buffers:

- ``last_completed``: the exact payload most recently completed on stdout, or absent before the first frame;
- ``current``: one newly rendered pending/in-flight payload plus its write offset.

The buffers may swap roles after a successful write; no third full frame is retained. ``render_dirty`` means canonical visible state
is newer than the newest completed/pending representation and is a coalescing bit, not a queued render count. The small protocol
prelude buffer is separate and does not contain a status frame.

If ``current`` has not been partially written, multiple state changes in one event batch collapse to one render and a newer render may
replace that unwritten buffer. Render directly into ``current`` and compare its bytes/length exactly against ``last_completed``. If
identical, discard ``current`` and clear ``render_dirty`` without a write.

Once any bytes of ``current`` have been written, it is immutable until completion. If state changes while it is blocked/partial, set
``render_dirty`` only; do not serialize an intermediate replacement. After completion, swap/promote it to ``last_completed`` and, if
dirty, serialize the then-current canonical state exactly once.

Rendering is deferred until the current event batch is complete so several state changes produce one serialization. The core also
enforces a process-wide maximum render rate of 20 Hz by default. If visible state changes sooner than the minimum 50 ms interval, set
``render_dirty`` and arm/reuse one monotonic one-shot deadline for the earliest permitted render; do not create a periodic tick.
Finishing an already-partial frame is never rate-limited. The bound may be configurable within an implementation-defined safe range,
but disabling storm protection is not the default.

Startup and Diagnostics
-----------------------

On normal startup, run the same Stage/Activate/Commit/live-refresh machinery for the initial configuration before emitting any i3bar
protocol bytes. Any failure before pointer commit is reported to stderr and exits nonzero; there is no previous generation to
preserve. After pointer commit, a live-refresh callback failure faults that block as specified by ``RLD-003``. Only after the first
generation's mandatory live-refresh/publication-prerequisite barrier completes may the protocol prelude and first rendered frame be
emitted.

Minimum command-line surface:

- ``-c FILE`` / ``--config FILE``: override the configuration path;
- ``--check``: execute the same configuration staging/validation path, including ``init`` and staging ``update`` callbacks, but do
  not pointer-commit, run the post-commit live-refresh/publication-prerequisite barrier, activate core-owned external integrations,
  transmit asynchronous
  protocol operations or emit i3bar output; exit zero only if candidate execution, schemas, ``init``, staging ``update`` and
  synchronous local validation succeed. This is staging and
  synchronous validation, not proof of later asynchronous peer/service/broker availability. It is also not a sandbox: trusted
  configuration/LuaJIT FFI may itself perform arbitrary side effects;
- ``--version`` and ``--help``;
- optional ``--debug`` for verbose lifecycle/source diagnostics on stderr.

Normal informational/debug logging goes to stderr only. Repeated source/reconnect errors should be rate-limited or state-change
logged so a broken optional integration cannot create an unbounded log storm.

Shutdown after SIGINT/SIGTERM/EPIPE stops ordinary callback dispatch, cancels core-owned block subscriptions/operations, runs
registered deferred cleanups while the Lua state is still valid, closes remaining owned resources and exits without attempting to
close the intentionally infinite JSON outer array.

i3bar Output Protocol
---------------------

Emit a valid i3bar protocol header with click events enabled. The header JSON is followed by exactly one ``\n`` before any outer-array
byte:

.. code:: json

   {"version":1,"click_events":true}

Then begin the infinite outer array.

Do not emit a synthetic empty status frame solely to simplify comma handling. Keep one ``first_frame`` bit: the first committed
status frame is written without a leading comma and every later frame is comma-prefixed. This avoids an unnecessary blank-bar
update at startup.

.. code:: text

   {"version":1,"click_events":true}
   [
   [...]
   ,[...]
   ,[...]

The stream remains open for the process lifetime.

Only required i3bar fields and semantics need to be supported.

Because ``click_events`` is permanently enabled in the one-time header, stdin remains drained for the process lifetime even
if the current configuration has no click handlers; a later reload may add them without changing the header.

stdout and Backpressure
-----------------------

stdout is nonblocking.

Never block the event loop waiting for i3bar.

The one-time header/outer-array prelude is also written through a bounded nonblocking startup-output state machine. Status-frame
bytes are not interleaved with an incomplete prelude. Partial writes, ``EAGAIN`` and ``EPIPE`` are handled from the first protocol
byte onward.

Maintain the two-buffer ``OUT-002`` model: ``last_completed`` plus at most one serialized ``current`` pending/in-flight frame,
with ``render_dirty`` as the only representation of newer desired state while ``current`` is partial.

If the current frame has not yet been partially written, a newer rendered frame may replace it entirely.

Once any bytes from a frame have been written, its remainder must be completed before another frame begins or JSON framing
would be corrupted.

If further renders become necessary meanwhile, set ``render_dirty``; do not queue or repeatedly serialize intermediate
frames.

After the current frame completes, render the newest state once if dirty.

Thus blocked stdout cannot create an unbounded queue or repeated serialization work.

Register ``EPOLLOUT`` only while bytes remain to be written.

Remove it immediately once output is drained.

A closed pipe or ``EPIPE`` causes clean termination.

Click Input
-----------

stdin is nonblocking and remains registered for ``EPOLLIN`` until EOF because click events were enabled in the immutable
protocol header.

The i3bar click stream is an infinite JSON array, so arbitrary ``read()`` boundaries are not JSON-message boundaries.

Use a small bounded streaming framer before ``yyjson``. The framer:

- consumes the one-time outer ``[`` plus inter-object commas/whitespace;
- tracks string/escape state and nested object/array depth, rejecting/discarding beyond the configured depth limit;
- retains a syntactically incomplete object across reads;
- invokes ``yyjson`` only after one complete click object has been framed;
- never treats an ordinary partial ``read()`` as malformed input.

Use bounded event bytes and nesting depth. If one event exceeds either bound, enter discard/resynchronization mode without growing
the retained input buffer. Resume only when a structurally provable top-level boundary is found. JSON does not guarantee recovery from
every malformed prefix (for example an indefinitely unterminated string); if framing becomes irrecoverably ambiguous, keep discarding
click input until EOF/restart rather than guessing a boundary or risking unbounded storage. Status output and unrelated modules
continue normally.

A complete but syntactically invalid event is logged/discarded and the framer resynchronizes at the next valid top-level
element where possible.

EOF in the middle of an event is treated as truncated input and followed by normal stdin/i3bar shutdown handling.

The Lua click callback and copied event fields are exactly the ``ABI-001`` schema defined under ``Lua ABI v1``. Unknown wire
members are ignored rather than copied through, and routing-only wire identity fields are not exposed as arbitrary event payload.

Route clicks through the emitted stable identity token and require the event's ``name`` to match the current block as an additional
consistency check. Present the configured ``name``/``key`` to Lua through the block context/event identity rather than exposing the
wire token.

Unknown tokens, removed identities and name/token mismatches are ignored. A stale click for an identity that still exists routes to
that identity's current-generation handler; no replaced Lua state is entered for click delivery.

Reload
------

The configuration reload watch is core-owned rather than block/generation-owned. Watch the configuration's containing directory
with inotify, filter events for the configuration basename, establish the watch before the initial load where possible, and keep it
across successful/failed generations.

This must continue working if an editor replaces the file by rename.

Load the root configuration through one bounded fingerprinting read helper: ``open()`` the configured path, require the opened
object to be a regular file, ``fstat()`` it, read at most the per-source-file limit into memory while computing an ``XXH3_128`` content digest over exactly those bytes, ``fstat()`` the same fd again, and reject/retry if device/inode/size/nanosecond-mtime metadata
changed during the read. Execute exactly the buffered bytes. The stored fingerprint contains ``XXH3_128`` plus size and the relevant file/referent metadata; the digest is a fast
identity/change-detection mechanism for trusted local configuration, not a security boundary. After each load/commit attempt, re-resolve the pathname and verify that it still names the opened file metadata; if not, set
``reload_dirty``. Events observed while a candidate is staging also set ``reload_dirty``.

Lua files loaded through the normal configuration module loader during staging are configuration dependencies, subject to both the
per-file/dependency-count limits and the 16 MiB aggregate root-plus-dependency executed-source-byte limit. The core loader applies the same bounded regular-file read/digest discipline to each dependency and executes the exact bytes
whose digest is recorded. Dependency fingerprints therefore include the content of the executed bytes rather than only pathname/stat
metadata.

For the root config and every ordinary tracked dependency, record the logical pathname, complete resolution chain and final referent
identity using the shared resolution-chain helper. Activation arms the parent-directory watches for every symlink-bearing component,
the logical final entry and the final referent. Any chain-component/referent event marks candidate revalidation dirty and forces full
re-resolution from the logical path; this covers symlinked ancestor directories as well as a symlink in the final component.

After all candidate reload watches are armed, enter a **pre-commit validation barrier** while ``RLD-001`` still keeps the old
generation current. Re-open/re-resolve and fingerprint every root/dependency path against the exact bytes/referent chains from which
the candidate was executed. This pass is resumable across reactor turns and consumes at most 2 MiB of configuration-source input per
turn; digest/file continuation state is bounded. Maintain a reload-watch change epoch: if a relevant event/overflow is observed during
a validation pass, finish/discard that pass and repeat against a fresh epoch rather than treating earlier per-file checks as an atomic
snapshot.

When one complete validation pass observes a stable change epoch and every source matches, the candidate may proceed directly to
commit. If any digest, pathname resolution chain or final referent identity is known to differ, ``RLD-002`` applies: the candidate is
obsolete, is torn down without becoming current, and one coalesced reload from the newest contents is scheduled. A filesystem change
that occurs only after the final successful read but before commit is necessarily not yet known; because all watches are already live,
its event schedules the subsequent reload normally.

A change/replacement/disappearance of any dependency requests a root configuration reload; the new candidate rediscovers its own
dependency set. The root config directory is prepended to the configuration Lua module search path in a deterministic way; arbitrary
FFI/custom loaders remain trusted code and are not automatically dependency-tracked unless they opt into the core dependency-watch
API.

``IN_Q_OVERFLOW`` applies to the core-owned root/dependency reload watches as well as block file watches. Overflow increments the
reload-watch change epoch and schedules the same resumable full resolution-chain/fingerprint validation. If current identities cannot
be proven equal to the loaded generation, request one coalesced reload; never perform an unbudgeted all-dependencies reread in one
reactor turn.

Also support explicit reload through ``SIGHUP``.

Only one reload transaction may execute at a time. Further file-change/SIGHUP requests observed before that transaction completes
set one ``reload_dirty`` flag. After the active transaction succeeds or fails, perform at most one new reload from the newest file
contents if the flag is set; do not recurse or queue an unbounded number of reloads. Reload progress is independent of stdout/display
progress, so a partially written status frame never blocks configuration replacement.

Reload is transactional with respect to in-process configuration visibility and callback dispatch. It cannot make external
asynchronous protocols such as D-Bus remotely transactional.

Stage
~~~~~

#. create a fresh Lua state with deterministic root-relative module search setup and empty dependency tracking;
#. read/fingerprint the root config and load/execute those exact in-memory bytes;
#. load normal Lua module-file dependencies through the core fingerprinting loader and record the exact executed identities;
#. construct all block definitions;
#. execute every block ``init`` against a staging context in declaration order;
#. after all ``init`` callbacks succeed, execute every defined **staging** ``update`` in declaration order; its resulting render
   state is validation-only and is never publishable under ``RLD-003``;
#. create/validate logical timers, watches, D-Bus matches and native-source requirements without dispatching callbacks;
#. prepare bounded resolution-chain metadata needed for activation/pre-commit source validation;
#. validate identities, state and configuration;
#. pre-sort blocks.

Relative timers created while staging are represented logically and receive deadlines from the eventual activation epoch.

External asynchronous operations such as D-Bus method calls are not sent during staging.

Failure of configuration execution, staging ``init``/``update`` or validation:

- cancels candidate core-owned handles/operations and runs registered deferred cleanups in teardown order;
- destroys the candidate state/resources;
- retains the old generation unchanged;
- reports the error to stderr.

Activate
~~~~~~~~

Prepare candidate local/kernel resources that can be installed synchronously, such as epoll registrations and native source
references, while candidate callback dispatch remains disabled and the old generation remains current.

If synchronous activation fails:

- roll back candidate registrations/resources;
- leave the old generation active.

No candidate Lua callback may run during this phase. Every source activated here must obey ``GEN-002``: candidate-relevant events
are reconciled/marked dirty or attachment is delayed; they are never silently consumed and lost.

Pre-commit Validation
~~~~~~~~~~~~~~~~~~~~~

After local activation has armed the candidate's reload/resolution-chain watches, run the resumable validation barrier described under
``Reload``. The old generation remains current and fully callback-routable during this work. Synchronous activation failure rejects the
candidate as an error; a source mismatch discovered by the barrier rejects it as an *obsolete candidate*, not as a runtime failure of
the old generation. The final pre-commit checks also perform the ``MEM-001`` post-retirement steady-state accounting preflight. Only a
candidate that passes both the stable-epoch barrier and this memory preflight may commit.

Commit
~~~~~~

After successful local activation and pre-commit validation, pointer commit and live publication are deliberately separated by the
``RLD-003`` refresh barrier:

#. make the candidate generation ``current``; from this point the previous generation can never receive another Lua callback;
#. establish the candidate activation epoch and arm its relative timers, but keep ordinary candidate timer/event callback dispatch
   gated until live refresh completes;
#. activate candidate process-shared logical lease references before releasing corresponding old-generation references, satisfying
   ``SHR-001`` without sending any acquisition whose prerequisite gate is still false;
#. stop old-generation dispatch, cancel its ordinary timers/watches/source subscriptions/pending asynchronous callbacks, release its
   process-shared logical leases, and run its registered deferred cleanups;
#. clear every candidate block's staging-only rendered state to hidden, without emitting/rendering that transition;
#. run the candidate's mandatory live-refresh ``update`` queue under normal per-turn/global callback budgets. Native transports may
   continue draining while Lua dispatch is gated, but candidate-relevant changes obey ``GEN-002`` and are retained/marked dirty;
#. after every update-capable block has completed one live refresh (faulting only the affected block on callback failure), run the
   bounded candidate-local publication-prerequisite resnapshot callbacks accumulated under ``GEN-002``; each such callback runs at
   least once after its activation/barrier dirty state, with concurrent notifications coalesced into the live source afterward;
#. once local publication prerequisites complete, enable ordinary candidate callback dispatch and start required asynchronous external
   initialization/operations. Sources whose authority depends on those later asynchronous operations remain hidden until their normal
   authoritative snapshot completes and therefore do not block unrelated blocks;
#. request the first render of the committed generation immediately; on initial startup the protocol prelude is likewise withheld
   until this publication barrier is complete;
#. destroy the old generation as soon as no current dispatch/cleanup frame can reference it. Any already-partially-written old status
   frame remains owned by the output state machine until completion but contains no pointer/reference into the old Lua state.

The live-refresh plus publication-prerequisite queues are immediate core work and therefore prevent a blocking sleep, but they are
continued across event-loop turns so readiness harvesting/control-plane service still occurs. Timer deadlines that become due while
ordinary candidate dispatch is gated
are not replayed as a catch-up burst: after the barrier, repeating timers use their normal fixed-rate skip-ahead rule.

Failure of asynchronous external initialization after the refresh barrier is a normal runtime integration failure handled by the
affected module/source. It does not resurrect the previous configuration generation.

Every callback/resource carries its owning generation. Events referring to a generation that is not ``current`` are discarded before
entering Lua. Click routing is instead resolved against the current generation's stable identity map as defined under ``Rendering``.

A callback can never outlive its Lua state. No replaced Lua generation is retained merely for output or click-routing progress.

Block Failures
--------------

Configuration-time failure of any staging ``init`` or staging ``update`` rejects the candidate generation as described under
``Reload``. Failure of a mandatory live-refresh ``update`` occurs only after pointer commit and faults that block under ``RLD-003``.

After a generation has committed, an unhandled Lua callback error affects only its block.

On such a runtime callback error:

#. log the error once with block identity and traceback;
#. mark the block faulted;
#. cancel its block-owned timers, subscriptions and pending callbacks without tearing down still-referenced shared sources/transports;
#. run its registered deferred cleanups in reverse registration order;
#. hide its rendered state;
#. continue running all other blocks.

A faulted block is not repeatedly invoked and cannot produce repeated log/wakeup storms.

A successful configuration reload recreates it normally.

These guarantees cover ordinary Lua errors under cooperative execution. Trusted custom Lua/FFI that blocks forever, corrupts
native state or terminates the process is outside the fault-isolation contract.

Resource Lifetime
-----------------

Every Lua-visible handle belongs to one block and one configuration generation. It may additionally hold a reference to a
generation-shared source and/or process-shared transport. Destroying the block/generation invalidates the handle and releases its
reference; shared objects remain alive only while another live reference requires them.

Generation-shared source objects never keep unguarded pointers into arbitrary block Lua state. Subscriber callback references are
separate generation-tagged objects. Process-shared transports contain no Lua callback references directly; they fan events into live
source/subscription objects after generation validation.

Watcher/subscription/source objects that may still be referenced by the current dispatch turn are retired and freed only after no
current dispatch frame can reference them. This applies equally to cancellation, block faults, source teardown and generation reload.
Core-owned subscriptions/operations are disabled before block deferred cleanups run; Lua registry references held by native
subscriptions are released before the owning Lua state is destroyed. Deferred cleanup registry references remain valid until their
callbacks have run/cancelled and are then released. No queued callback may enter a non-current Lua state.

fd ownership is explicit: a transport/source closes only descriptors it owns; raw ``watch_fd`` does not close the caller's fd. User
code that owns such an fd or other FFI/native resource should register ``ctx:defer`` immediately after acquisition so rollback, block
fault, reload and shutdown have deterministic teardown.

JSON
----

Use ``yyjson`` for:

- deterministic serialization of changed i3bar block-object fragments;
- click-event parsing.

Keep serialization order explicitly controlled by the C renderer.

Reuse output/storage buffers where practical instead of introducing avoidable allocation churn.

Do not add a general JSON abstraction layer.

Delivery Scope
--------------

The specification describes the intended architecture beyond the first release, but implementation is staged deliberately so
optional backends do not delay validation of the core.

Initial/v1 scope:

- core event loop, timers, signals, rendering/backpressure, click input and transactional reload;
- LuaJIT configuration/block API and the ownership/failure/limit contracts;
- baseline ``/proc``/``sys``/libc collectors for CPU/load, memory/swap, filesystem, disk I/O, power/battery, temperature and clock;
- inotify file/config watches, kobject power-supply events and PSI triggers;
- generic asynchronous sd-bus transport plus the Lua systemd failed-unit module.

Later optional milestones include journal enrichment, PipeWire, X11/XKB, i3 IPC, GPU backends, libalpm/Arch maintenance, process-hog
scanning and SMART/NVMe integrations. Their contracts remain specified here so they fit the same core when implemented, but they are
not prerequisites for a useful v1.

Optional Native Integrations
----------------------------

Optional integrations may add source/backend adapters such as:

- PipeWire;
- X11/XCB/XKB;
- i3 IPC;
- systemd journal access;
- libalpm;
- GPU/DRM/vendor management APIs;
- SMART/NVMe health APIs;
- other Linux-native APIs.

Integrations should expose generic mechanisms to Lua when doing so remains small and reusable.

A feature belongs in native C when doing so materially improves:

- wakeup count;
- syscall count;
- dependency footprint;
- correctness;
- integration with the main event loop;
- bounded parsing/state-machine behavior;
- or implementation simplicity.

Otherwise it should remain in Lua.

A foreign library may require loop *state* internally, but the i3sd core never creates a worker thread for it. An in-process
backend is accepted only when its documented integration can be driven nonblockingly from the core loop without requiring an
uncontrolled library thread/independently blocking loop. Adapt exposed fds/deadlines/nonblocking iteration into the single core
``epoll`` loop and apply source fairness budgets. Every replaceable foreign/native instance has its own epoch and epoll registration
cookie under ``FD-001``; reconnect/recreation never treats a reused integer fd as object identity. If a backend fundamentally requires
its own uncontrolled thread/loop, keep it
outside the process and integrate through explicit local IPC instead. Incidental implementation threads created internally by a
third-party library are not claimed impossible unless that library explicitly guarantees their absence; such a backend must still
not depend on them for a blocking callback into the core event-loop thread.

Dependencies
------------

Required:

- Linux;
- C23 toolchain/libc;
- LuaJIT;
- yyjson;
- an XXH3 implementation (vendored ``xxhash.h`` or ``libxxhash``) for non-security configuration-content fingerprinting;
- Meson;
- Ninja.

Optional build-time integrations selected through Meson feature options:

- ``libsystemd`` using ``sd-bus`` for generic D-Bus support and, separately, ``sd-journal`` if journal integration is enabled;
- PipeWire development library for audio/capture integration;
- XCB plus the XKB extension library for X11 layout integration;
- ``libalpm`` for Arch package-maintenance integration;
- DRM/vendor libraries required by selected GPU backends;
- libraries required by selected SMART/NVMe or other native integrations.

Avoid mandatory:

- GLib;
- Qt;
- GTK;
- libdbus;
- general-purpose D-Bus frameworks;
- libuv;
- worker-thread frameworks.

Build-time availability and runtime activation are separate: compiling/linking an optional backend must not instantiate it, open
fds, start polling or add wakeups unless the active runtime configuration actually requests it. Meson/Ninja are build tools, not
runtime dependencies of the installed executable.

Do not introduce an independently running second event loop.

Derived Performance Properties
------------------------------

This section is a non-normative checklist derived from ``EVT-001``, ``OUT-001``/``OUT-002``, ``MEM-001``, ``LAT-001`` and
``EXT-001``. It does not redefine their guarantees; where wording differs, the named normative requirement wins. The implementation
should exhibit these properties:

- with only healthy/stable event-driven sources configured, i3sd generates no periodic wakeups of its own;
- the process normally sleeps inside ``epoll_pwait2()``/``epoll_wait()`` when idle;
- ordinary polling requires no timerfd;
- wall-clock scheduling uses at most one realtime timerfd;
- compatible polling deadlines can be serviced by one wakeup;
- event-only blocks create no polling deadlines;
- no callback runs merely because an unrelated source woke the process;
- no Lua code runs while completely idle;
- no periodic garbage-collection timer exists;
- native sources are created only when required;
- unsupported optional hardware capabilities create no polling deadline merely because their module was compiled in;
- identical PSI trigger registrations may share one fd, while distinct trigger conditions remain independent;
- common native measurements are sampled at most once per event batch;
- cumulative counter deltas remain per consumer rather than global collector state;
- large snapshots and optional process scans are work-budgeted across event-loop turns;
- snapshot reconciliation backlogs and pending asynchronous operations are count/byte-bounded both per source/operation and in
  aggregate where variable-size payload retention could otherwise multiply the individual limits;
- package-maintenance state is recomputed from package-database change events rather than a fixed status polling tick;
- file-age warnings use a change watch plus an exact one-shot stale deadline rather than periodic age polling;
- optional health probes do not wake sleeping storage by default;
- package-maintenance built-ins consume only local package-database state, and no IP/network status collector exists;
- several state changes within one batch cause at most one render attempt, and continuous change is capped by a one-shot-driven
  default 20 Hz render-rate ceiling rather than an unbounded render storm;
- unchanged block state causes no render request;
- blocked partial stdout causes at most one later serialization of the newest state;
- byte-identical output causes no stdout write;
- stdout backpressure cannot create unbounded memory usage; individual strings/frames are bounded and ``OUT-002`` caps full frame
  storage at 4 MiB;
- process stalls/``SIGSTOP`` cannot create timer catch-up storms;
- ``SIGCONT``, detected suspend gaps or enabled logind post-sleep notification cause bounded state resynchronization rather than
  periodic catch-up;
- built-in modules never invoke a shell; explicit interactive helpers use the bounded asynchronous child-process primitive;
- the core starts no worker threads; optional in-process integrations require externally drivable/nonblocking integration and no
  independently blocking event loop.

Core-owned memory remains bounded by ``MEM-001`` plus the enumerated sublimits; practical resident size should be dominated by
LuaJIT, configured modules and optional libraries rather than unbounded queues or per-timer kernel objects. ``LAT-001`` deliberately
makes no hardware-independent wall-clock latency promise for the approved synchronous-local operations.

Trusted custom Lua/FFI is explicitly outside core memory/work/latency guarantees.

Derived Reliability Checklist
-----------------------------

This checklist summarizes consequences of the normative IDs and the subsystem contracts above; it is not an independent source of
requirements. Tests should reference the relevant ID/section so future edits cannot create two competing definitions.

- malformed click input must not terminate the process;
- ordinary partial click input is retained until a complete event is framed;
- click input and event size are bounded;
- input/output buffering is bounded;
- partial/EAGAIN writes of the one-time protocol prelude cannot interleave/corrupt later status frames;
- runtime failure of one cooperative Lua block must not stop other blocks;
- configuration-time staging ``init``/``update`` errors reject the candidate reload, while post-commit live-refresh errors fault only
  the affected block;
- faulted callbacks cannot repeatedly wake/log;
- config reload failure retains the working configuration;
- concurrent/bursty reload requests collapse to at most one follow-up transaction;
- asynchronous external initialization failure after commit does not roll back into an old generation;
- file replacement by editors does not disable future config reloads;
- inotify overflow/invalidation causes resynchronization rather than silent stale state;
- an in-progress authoritative resnapshot is not exposed as a half-built cache, and reconciliation overflow restarts a bounded
  authoritative snapshot rather than growing memory indefinitely;
- ordinary callbacks/watches cannot outlive the current Lua generation; replaced Lua generations are never retained solely for
  click routing or stdout/display progress;
- registrations may safely be removed during callbacks/reload;
- partial stdout writes are handled correctly;
- broken stdout/i3bar causes clean exit;
- all owned fds are nonblocking and ``CLOEXEC``;
- raw ``watch_fd`` ownership is unambiguous;
- D-Bus disconnect affects only dependent modules and may reconnect asynchronously;
- continuously busy D-Bus/fd sources cannot monopolize the loop indefinitely, and persistent rotating dispatch prevents a busy
  prefix of sources from starving later runnable sources;
- long ``SIGSTOP``/resume periods do not generate timer catch-up callbacks;
- ``SIGCONT``, detected suspend gaps and optional logind post-sleep notification resynchronize state that may have become stale
  while notifications were not consumed;
- disappearing ``/proc``/sysfs resources fail gracefully;
- unsupported optional metrics remain distinguishable from legitimate numeric zero;
- cumulative rates are hidden until two valid samples exist; reset/wrap/source replacement, continuity changes and excessive
  sampling gaps invalidate rate baselines instead of producing bogus zeroes/spikes/long-period averages;
- GPU/desktop/package optional integrations may disconnect/disappear without stopping unrelated blocks;
- sleeping rotational storage is not spun up by default merely for a health block;
- click identity is stable for ``(name, key)`` across reloads: stale clicks for removed identities are ignored, while stale clicks
  for surviving identities route only to that identity's current handler; the unavoidable lack of an i3bar display acknowledgement is
  explicitly not modeled as a generation-lifetime guarantee;
- core/built-in external IPC paths do not perform synchronous blocking waits;
- shared-source teardown/faults cannot invalidate other live subscribers;
- configuration reload executes bounded regular-file root/dependency bytes through named content-digest fingerprinting, enforces an
  aggregate source-byte budget, tracks complete symlink resolution chains, uses a resumable stable-epoch pre-commit validation barrier,
  never commits a candidate already known obsolete, and revalidates after inotify overflow;
- D-Bus adapter fd/event/deadline changes are reconciled atomically enough that an fd replacement cannot leave epoll watching the old
  descriptor; disconnected method calls are never automatically replayed;
- systemd invalidated properties and manager reload completion cannot leave cached unit state silently authoritative;
- systemd ``Manager.Subscribe`` is native-connection-epoch state with a reference-counted desired lease count: zero consumers
  converge to ``Unsubscribe``, same-transport reload handoff does not flap the subscription, and reconnect establishes it once for the
  fresh connection;
- rendered/config identity text is valid UTF-8 without embedded NUL while binary protocol values retain byte-string semantics;
- the per-visible-block serialized-contribution bound makes aggregate frame capacity independent of callback order, with the final
  2 MiB renderer check retained as an invariant assertion;
- authoritative source caches as well as reconciliation queues are bounded and never publish truncation as authoritative state;
- deterministic deferred cleanup runs for cooperative user-owned resources on rollback/fault/reload/shutdown;
- remote/FUSE/autofs paths are outside the supported synchronous-local-I/O model, and ``LAT-001`` makes no hard wall-clock latency
  guarantee even for approved local kernel/filesystem/device-backed queries; uncooperative custom Lua/FFI is outside all such bounds.

Testing and Fuzzing
-------------------

Unit/integration tests should cover at least:

- fixed-rate timer phase alignment and skip-ahead after long process stalls;
- distinction between ``CLOCK_MONOTONIC`` process stalls and system suspend semantics, including bracketed M/B/M sampling,
  scheduler-delay rejection and the suspend-gap threshold;
- runtime ``epoll_pwait2()`` -> ``epoll_wait()`` fallback and timeout rounding/clamping;
- absolute wall-clock timer rearm after realtime changes, including already-past deadlines, ``read()``/rearm ``ECANCELED`` cases,
  plus explicit clock-change/timezone callbacks after ``/etc/localtime`` replacement or any watched resolution-chain component change;
- PSI trigger validation, identical-trigger sharing, distinct-trigger independence, event delivery and unsupported/permission errors;
- threshold hysteresis, multi-severity transitions, formatter-returned hiding and transient hold-timer behavior;
- arbitrary click-stream chunk boundaries, escaped strings, nesting-depth limits, oversized events, recoverable malformed-event
  resynchronization and irrecoverably ambiguous prefixes without unbounded buffering;
- partial/EAGAIN writes of both startup protocol prelude and status frames, replacement of unwritten frames and dirty-state collapse
  while a frame is partial;
- cancellation/destruction of timers/watches during callback dispatch, far-future timer cancellation without heap tombstone
  growth, fairness when very large aligned timer sets expire together, rotating fairness across more simultaneously busy sources than
  fit in one global budget, guaranteed control-plane service under saturation, and ``EVT-001`` readiness harvesting where a
  permanently runnable userspace continuation coexists with a newly-ready signalfd/inotify/socket that was not ready on the previous
  epoll wait;
- reload rollback, activation-epoch timer behavior, reload-dirty coalescing, stable-epoch pre-commit validation, rejection of a
  candidate known obsolete before commit, ``GEN-002`` activation event retention/dirty/resnapshot behavior, ``RLD-003`` proof that
  neither ``init`` nor staging-``update`` render state can leak into publication, that a staged sample may age across many validation
  turns yet is replaced by live refresh before rendering, that activation-dirty local watches resnapshot before the first candidate
  render, live-refresh failure
  becoming a block fault rather than rollback, immediate old-generation teardown independent of blocked/partial stdout, and stable
  current-identity click routing;
- fd-based root/dependency bounded read/XXH3 races, aggregate source-byte-limit rejection, same-size/same-mtime content replacement,
  root/dependency rename-save, final and intermediate symlink-component replacement, ordinary Lua ``require`` tracking, budgeted
  pre-commit revalidation across turns, dependency removal/recreation and reload-watch recovery after ``IN_Q_OVERFLOW``;
- startup configuration failure and ``--check`` producing no protocol output/core-owned external activation while documenting
  that arbitrary trusted Lua/FFI side effects are outside that guarantee;
- stale click behavior across reload: removed/change-key identities are ignored, surviving ``(name, key)`` identities route to the
  current handler even when an older frame is still displayed, and no old Lua generation is entered;
- inotify rename-save behavior, shared-directory mask union/refcounting, generic pathname-vs-referent semantics, complete
  resolution-chain rearming, ``IN_Q_OVERFLOW`` and watch invalidation/re-establishment;
- ``SIGCONT``-, suspend-gap- and logind-post-sleep-triggered resynchronization, including coalescing duplicate resume indications,
  atomic publication of replacement source snapshots and bounded reconciliation-overflow restart;
- raw cumulative collector snapshots with independent consumer intervals, two-sample warm-up, continuity invalidation and
  excessive-gap baseline reset;
- diskstats physical/explicit aggregation, 512-byte sector conversion, hotplug, counter reset/wrap and no partition double counting;
- power-supply partial-capability matrices, health derivation, unavailable remaining-time cases and uevent-triggered rereads;
- backup-age exact stale deadlines and pacman-lock ``mtime + duration`` absolute-realtime *long-held* warnings, including startup,
  runtime appearance, realtime clock changes and disappearance without incorrectly claiming age proves staleness;
- optional process scanning respecting per-turn budgets under very large synthetic PID populations;
- D-Bus signature validation, encode/decode round trips, exact int64 handling, ``ay`` binary strings, canonical dictionary pair
  sequences, variants, value/depth/message bounds, callback-scoped message lifetime and sd-bus-owned call timeouts;
- D-Bus call cancellation/disconnect without automatic replay, match-install failure isolation on a shared connection, recreation of
  native ``sd_bus`` objects after disconnect, fd replacement/event-mask/deadline changes across adapter refreshes, same-integer-fd
  reuse across a native connection epoch, stale epoll-cookie rejection, and ``epoll_ctl`` reconciliation errors without blind
  fd-number aliasing;
- D-Bus fairness under a continuously busy connection and fairness across many concurrently busy adapted sources;
- systemd match/subscription-before-snapshot reconciliation, reference-counted subscription leases, zero-consumer
  ``Unsubscribe``, same-transport reload handoff without subscription flapping, fresh subscribe after native D-Bus reconnect, no
  resubscribe on manager-reload/resume-only resnapshot, ``ListUnitsFiltered`` fallback, aliases, type filtering, optional result
  metadata and ``PropertiesChanged`` invalidated-property recovery;
- journal ``SD_JOURNAL_INVALIDATE`` recovery after rotation/vacuum/file-set replacement, proving that incremental
  cursor/cache assumptions are marked uncertain and a bounded current-boot/window rebuild completes before authoritative follow mode;
- PipeWire/XCB foreign-loop-fd adapters under continuously ready input, including PipeWire enter/iterate/leave lifecycle,
  reconnect and fairness, with ``EXT-001`` tests measuring fairness between indivisible foreign dispatch quanta rather than claiming
  callback-level preemption inside one PipeWire iteration;
- GPU backend capability discovery where missing VRAM/power/utilization capabilities never become zero;
- libalpm database-change coalescing, transaction-lock deferral until ``db.lck`` disappears, debounce fallback for nonstandard
  writers, and local-database-only behavior;
- SMART/NVMe backend policy that skips a sleeping rotational device unless wakeup was explicitly permitted;
- block fault isolation, protected C -> Lua calls, rejection of callback yielding, candidate-reload rejection semantics, and
  ``ctx:defer`` exactly-once/LIFO cleanup on staging rollback, block fault, reload, ``--check`` destruction and shutdown;
- shared-source ownership under block fault/reload: one subscriber cancellation cannot tear down another, while last-reference/generation
  teardown frees the source and process-shared transports retain no stale Lua references;
- allocator/fd/watch fault injection including ``ENOMEM``, ``EMFILE``/``ENFILE`` and inotify-watch exhaustion at staging and runtime
  scopes, plus stale epoll events after fd close/reuse;
- UTF-8/NUL validation, exact per-block serialized-fragment preflight/caching, and proof/assertion that configured block-count and
  contribution limits fit inside the aggregate frame bound regardless of update order;
- filesystem ``f_bavail``-based availability math including reserved blocks, zero-sized/error cases;
- rendering with native separator spacing and no synthetic spacer objects;
- hard resource-limit failures for block/string/frame/pending-operation/reconciliation bounds, including aggregate retained D-Bus
  payload and aggregate reconciliation-backlog budgets, ``MEM-001`` steady-state charging, reload transition-reserve exhaustion, and
  post-commit deferred construction of a large authoritative cache after old-generation charges are released;
- ``SIG-001`` initialization ordering with a test backend that creates an incidental thread after core signal masking and verifies the
  inherited blocked mask/signalfd-only delivery path;
- ``ABI-001`` schema conformance tests for every v1 primitive/collector, including strict option rejection, exact cdata counters,
  deterministic ordering and nil-vs-zero capability semantics.

Performance regression tests/benchmarks should additionally measure:

- idle wakeups/syscalls and CPU over a multi-minute event-only configuration;
- RSS/core-owned memory at idle and under bounded event storms, plus steady-state/transition-reserve high-water accounting against
  ``MEM-001``;
- clock-only idle behavior;
- wakeup coalescing for representative 1/2/5/60-second polling intervals;
- render/serialization cost at realistic and maximum supported block counts;
- fairness/latency under busy D-Bus/fd/timer sources, including more runnable sources than one turn's global budget can service and
  a continuously carried userspace continuation while new kernel readiness is harvested each turn;
- render-storm suppression under rapidly changing visible state;
- stdout-stall behavior with exactly the two full-frame buffers allowed by ``OUT-002``, no queue/serialization growth, and click
  routing while an old frame remains displayed.

These measurements should establish repeatable regression baselines for the project's stated negligible-idle-work goal rather than
serving as hardware-independent absolute performance promises.

In addition to example-based tests, add deterministic model/property tests for the hardest state machines: output/backpressure,
generation stage/activate/validate/commit/live-refresh/retire, timer scheduling/cancellation, epoll registration-cookie lifetimes,
snapshot/reconciliation publication, process-wide memory charging and shared subscription/remote-lease lifetimes. Generate long
randomized action sequences (partial writes, reload dirties, source events, fd close/reuse, disconnects, lease acquire/release races,
cancellations, stale clicks and faults) against an injected-clock/fake-fd harness and continuously assert the normative requirement
IDs above.

Add libFuzzer harnesses for parsers/state machines with attacker- or environment-controlled byte structure, especially:

- the i3bar click-stream framer plus ``yyjson`` handoff;
- D-Bus signature parsing and Lua/native marshalling helpers;
- i3 IPC framing if that optional integration is implemented natively;
- parsers for ``/proc``/sysfs text formats whose malformed/truncated input could otherwise violate bounds or state assumptions;
- kobject-uevent decoding/reconciliation helpers when implemented natively, including truncated/malformed lengths and
  ``MSG_TRUNC``/loss paths;
- any compact protocol/framing parser added by optional native modules.

Run normal tests under ASan and UBSan in dedicated sanitizer builds. Exercise reload/cancellation and parser fuzz corpora under
sanitizers because these paths combine native lifetimes with untrusted byte structure.

Keep deterministic tests independent of real wall-clock timing where practical by making timer/deadline logic testable with
injected clock values.

Design Rule
-----------

Optimize first for avoiding work entirely:

.. code:: text

   no wakeup
   > one cheap wakeup

   no callback
   > cheap callback

   no render
   > fast render

   no write
   > fast write

Keep the runtime footprint and idle work small. Do not sacrifice explicit state-machine correctness merely to minimize source-line
count; prefer a small number of generic native mechanisms with precisely bounded behavior over duplicated special cases.

Prefer one generic native primitive over several service-specific C integrations.

Features that can live cleanly in Lua should remain in Lua.

Native C integrations are appropriate where they substantially reduce wakeups, syscalls, dependencies, complexity or
overhead.
