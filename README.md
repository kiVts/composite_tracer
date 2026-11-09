# Link Tracer 0.4.0

Select an **on/off, number or composite input**, a **composite read/write**,
or an **on/off, number or composite output** in the microcontroller logic editor.
A native Link trace panel opens on the right with one list, channel first:

Every row leads with its channel (`ON/OFF n`, `NUMBER n`, `ON/OFF 1-8` for segment
displays) or, where no channel applies, `FROM` / `TO` / `END`. A heading names the
place the rows below share: the panel a button is on (`From ...`), the panel that
shows the signal (`Shown on ...`), or the controller input it goes into
(`Into [MC] input '...' via ...`). Scalar inputs name where their value comes from
and each step of the way ("computed by OR #151 in [MAINBOARD]"). Outputs list
every receiver, with the channel it reads once a Composite Read picks one.
Previous/Next appear only when the list doesn't fit; a page that starts inside a
group repeats its heading. X dismisses the panel until another block is selected.
Stock block settings remain available.

The trace follows vehicle wiring through other microcontrollers, composite writes,
reads, switchboxes and numerical junctions. Every receiver of a split output is
listed. Reports show custom part labels alongside the part's display name, such
as **ENGINE RPM (Dial)**, controller port labels, and named Instrument Panel slots.
Unlabelled receivers include their positions. Instrument Panel buttons appear with
their configured names and channels. Number and on/off channels are separate. Supported game outputs include
radar data, seat data, monitor touches and layouts described by part definitions.

Instrument Panels show what matters for the logic: the buttons that write channels
(name, type, slot) and the instruments that read them. Segment mode reads several
on/off channels, including wrapping past 32. Panel composite inputs pass through to
their outputs with button channels overwritten; tracing continues through chained
panels, and a channel a button overwrites is not reported as the original signal.
When controls share a channel, all slots remain listed and the report identifies
the final writer, matching the game's slot order.

Only connected switchbox branches are followed. A single connected side remains
traceable; two connected sides list their possible sources with on/off conditions.
An unlinked switch control is off: A is marked as needing switch-on control, B as
the off branch. A switchbox with no data inputs produces a short explanation naming
its microcontroller. Loops, missing blocks, radio/connector boundaries and channels
chosen at run time also have explicit stop reasons. An unknown lead is never
presented as a known empty channel.

The panel uses the live, unsaved microcontroller definition and refreshes changes
once per second. The surrounding vehicle wiring is copied when entering a trace
session, since it cannot be edited while inside the controller. Leave the controller
to change external wiring, then reopen it. Newly added external nodes acquire
vehicle connections after leaving the controller editor. A standalone controller
can trace its internal logic, but has no external vehicle links.

Lua scripts are inspected without execution: literal output channel calls and
simple literal loops can be listed. Comments and strings are ignored. Variable
channel expressions and unsupported script indirection stop with an explanation;
this is a map of possible sources, not simulated signal values.
Arithmetic and other computed signals identify the controller/block where they
are produced. Forward traces distinguish a switch control input from its data
path; a control value is not presented as the switchbox's outgoing signal.

## Install

Requires the SWML loader and the paired core advertising corrected native UI
context and measured text height (`SWML_V3_CAP_UI_TEXT_AUTO_HEIGHT`). Import
`link_tracer.zip` in SWML, or extract its contents into `mods/link_tracer` inside
your SWML installation. Enable Link Tracer in the mod list and restart Stormworks. It is local to your editor and
does not require other multiplayer players to install it.

## Build and validation

From the repository root:

```powershell
cmake --build swml/build --config Release --target link_tracer link_tracer_test link_tracer_extended_test link_tracer_panel_test -- /m:1 /nodeReuse:false
ctest --test-dir swml/build -C Release -R "^link_tracer" --output-on-failure
```

The tests cover named buttons, both switch branches, single/empty branches,
unlinked control, loops, missing blocks, dynamic channels, unsaved definitions,
channel overwrite/pass-through, Lua comments and output declarations, long lists,
and ambiguous controller identities. Extended cases cover scalar sources across
controllers, custom names, split receivers, write/output destinations, control
inputs, numerical routing, destination loops and unsaved reverse-link changes.
The synthetic cases run without a game
installation. Optional real-vehicle/part-definition checks require the game files.
Exceptionally large branching circuits stop at a bounded trace depth/work limit;
more than eight source alternatives per channel, or very long descriptions, are
shortened with an explicit reason naming the controller. All 64 channel slots
remain available. Pagination uses native text height and preserves UTF-8 labels.

All twelve selectable native block classes, layout patterns and signed function bindings are checked against the
archived original executable and the installed executable without launching them.
A changed signature or field contract disables the feature. XML parsing and graph
walking run on one worker with immutable snapshots; native pointers stay on the
editor thread. Stale results are discarded after selection/session changes.

`link_tracer_panel_test` drives the real panel code against the SDK's native UI mock and lints every frame;
`--vehicle FILE [--filter MC] [--text] [--scenes DIR]` opens the popup on every node of a saved vehicle
(see `tools/ui_preview/README.md` to render the scenes). Live click interaction still needs a game check.

0.4.0 replaces the Trace / Panel details tabs with one channel-first list. The
channel is each row's badge; slot numbers move to the detail line. Panel port
and slot inventories are no longer shown. Destination rows say what uses the
signal and through what, and readers of channels a panel button overwrites are
no longer listed as receivers of the original output.

0.2.3 fixes the background itself: the paired SDK core now calls the game's
native button size method, which also resizes and uploads its background geometry.
The old outer-bounds-only update left text and footer outside the visible panel.
The layout reserves space for wrapped titles, pads content on all sides, and
repaginates when the available height changes. The paired core is required.

0.2.1 fixes wrapped text measurement, doubles the side padding, positions the
footer explicitly below the current content, and gives each same-channel source
its own paragraph. Names are split across pages only when the entire paragraph
is taller than a page. The matching core update is required for the height fix.
