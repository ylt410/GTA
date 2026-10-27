# Personal performance update: v1.4.6, build 17

Superseded by build 18. The optional frame-generation experiment regressed source
FPS (about 30 to 20) and showed car ghosting on the user's device. Keep it off in
this archived build. Build 18 removes it; the old source tests established no
device-level performance benefit.

This private build keeps the existing app identifier, game files, saves and mod
loader. Install with the same signing account as the existing app. The earlier
`latest-ipa` release is retained for rollback.

Changes:

- Reuse up to 64 leased local asset file handles, avoiding repeated open/close
  calls while keeping concurrent reads, failures and configuration changes safe.
- Avoid redundant Metal immediate-constant bindings, with encoder and shared-slot
  invalidation.
- Batch log writes and bound session/previous/stopped logs to 16 MiB each,
  preserving startup information and the recent tail. Important startup and stop
  signals remain available when normal logs flood the UI queue.
- Automatically remove abandoned installer work older than 24 hours. Delete old
  resource chunks only after their packed replacements are durable. Active mod
  resources, downloaded offline data, saves and game files remain available.
- Optional **AI frame generation (MetalFX)** in Settings > Graphics. Off by
  default; requires iOS 26 or later and supported Apple hardware. It uses actual
  scene depth and camera reprojection, with the HUD composited separately. Motion
  vectors describe the camera, so moving vehicles/people can show artifacts.
  Missing inputs, unsupported hardware, insufficient memory and GPU errors retain
  normal presentation. The FPS counter still reports real engine frames.

Frame generation improves displayed motion, not simulation speed or input
latency. Device frame rate, image quality and long-session stability require
testing on the installed app; source regression tests cannot establish them.

The build checks C/C++, Foundation logging/cleanup, Swift/iOS types, native
Metal renderer and the new Metal shader before the expensive engine build.
It reuses only the verified unchanged engine shader library/reflection from the
previous working private IPA; runtime code and the new shader are rebuilt.
