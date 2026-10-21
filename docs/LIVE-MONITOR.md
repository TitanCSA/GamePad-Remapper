# Live IN / OUT debugging

Available in the **2.2.4 candidate**, with Interceptor Gamepad & Controller
kernel **1.0.5.0**. Microsoft signing is pending; the older public 1.0.4.0
kernel cannot supply this telemetry. The classic Interceptor driver is unchanged.

Open the application elevated, or choose **Enable live IN / OUT** in the right
panel. Nothing needs to be loaded or applied. The monitor starts independently
of script execution and remains active after **Stop**, a script error or a
failed capture. Editing remains available without elevation or an installed
driver. Enabling observation does not apply a script or start the capture trial.

| Column | Measurement |
| --- | --- |
| **IN** | A report received on this filtered controller stack, before changes made at that layer. |
| **OUT** | A successful report completion from the filter towards Windows, after any modification. |

The columns show each available control side by side. HID controls come from
their own descriptor; multiple reports and repeated axis usages stay distinct.
Native XUSB/GIP states include partial Guide updates where available. A translated
HID view is labeled by its family: it is a Windows-derived report, not another
independent electrical USB measurement. Values
remain `--` until measured, and `*` marks a difference. A quiet device keeps its
last measured value and timestamp; an error or lost report never becomes a
fabricated neutral state.

**Copy diagnostics** includes device identity, raw bytes, IN/OUT sequence numbers,
QPC timestamps and age, Windows/driver failures and lost-report counts. A slow
monitor can lose records from its bounded ring without blocking input delivery.
Observation uses a read-only control handle and per-device HID readers, not an
interception context. It does not suppress, synthesize or remap controller input.

Capture and observation run separately from UI painting. Changed snapshots wake
the UI without a fixed FPS timer. Actual report rates still depend on the device,
Windows scheduling and display refresh. OUT is the report submitted at a successful
driver completion, not a guarantee that a particular game, browser or API read it.
Several Windows HID readers can produce several OUT completions for one IN;
their queues may also have different ages. These counts are not controller FPS.

To debug a script, compare IN with OUT before applying it, apply the script,
then press **Stop** and compare again. Continue moving the controller after Stop:
the monitor remains live and normally shows matching passthrough values.
