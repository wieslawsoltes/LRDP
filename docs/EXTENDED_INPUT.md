# Extended touch and pen input

The `Microsoft::Windows::RDS::Input` dynamic channel implements specification-derived MS-RDPEI framing, compact integers, version negotiation, touch/pen frame parsing, optional pressure/orientation/contact rectangles/pen attributes, and contact lifecycle validation. It is independent of legacy mouse/scancode input.

## Native capability contract

`Desktop::extended_capabilities()` decides whether the channel is opened. The portal backend requests touchscreen permission only when the compositor advertises that device type and exposes at most 32 touch contacts only after consent grants it. X11, headless and diagnostic backends do not advertise a digitizer. Pen parsing is implemented and tested, but no production backend advertises pen injection yet. Pressure, tilt, hover and IME parity are not implied by touch-only portal delivery.

The touch adapter maps sparse byte IDs to bounded native slots and converts captured-desktop coordinates to compositor logical coordinates. It validates a complete message before issuing native calls. Failed native batches attempt to release every possibly live slot, including newly allocated ones.

Session suppression, native layout changes, RDP reactivation, focus synchronization, channel closure, malformed batches and disconnect cancel live contacts. RDPEI suspend/resume notifications and stale-ID tracking prevent old motion packets from resurrecting a contact after a coordinate-space change. A fresh down event is required for recovery.

Native calls use the consented RemoteDesktop portal's NotifyTouchDown, NotifyTouchMotion and NotifyTouchUp methods with the exact granted session and stream identifiers. This path does not open an EIS connection or mix Notify methods with EIS. Portal requests and outstanding native calls remain bounded; permission revocation closes the session.

## Validation and limits

The portable tests include Microsoft compact-integer golden vectors, malformed packets, native slot exhaustion, transactional batch rollback and a full Session fixture exercising GCC/MCS activation, DVC creation, RDPEI negotiation, injection, suppression/resume, resize and teardown. The private D-Bus fixture checks native method signatures, logical coordinates and refusal after touchscreen consent is denied. The original 20,000 malformed input batches remain enabled.

The local environment verified 19 available ASan/UBSan suites after session integration. It does not contain GLib/PipeWire development dependencies, so native portal compilation and tests are validated by exact-revision GitHub Actions rather than asserted as locally executed. No actual Windows digitizer or GNOME/Plasma touchscreen run has been recorded.

## Public specifications

- MS-RDPEI: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpei/eff22485-b00b-4483-95c4-9253cfa36dd8
- Client Ready: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpei/de4f171b-f273-4742-98fd-99c602c4f200
- RemoteDesktop portal: https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.RemoteDesktop.html

No Microsoft, FreeRDP or xrdp implementation source is used for this wire implementation. Platform APIs are dependencies, not copied protocol implementations.
