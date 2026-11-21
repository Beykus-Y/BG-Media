# BG Video feed preparation (experimental)

Enable `BGMEDIA_BUILD_VIDEO` and link `BGMedia::Video`. This initial component
preserves provider order and asynchronously reads a bounded prefix of the next
local file. It does not decode, play, report views, implement a disk cache, or
provide a complete provider ABI. `Snapshot::next` is a shared immutable object:
the host may retain it across navigation, and that retained memory is outside
the feed's budget. Release snapshots promptly when reducing memory use. Budget
zero disables preparation. The worker runs one provider call at a time. A new
selection, queue, budget, or close requests cancellation and discards stale
results. Destruction joins the worker; providers must make blocking operations
cancellable. `LocalProvider` is for host-owned regular local files, not pipes or
network paths. It never removes or modifies those files.

BG Tok PR #3 exposes Rust async streams with cancellation, ranges and expiring
sources, but no stable C ABI. A host adapter will be required; this experimental
local-file interface is not that contract. Network reads, retries, source
refresh, cache policy, account isolation, and playback notifications remain
unimplemented. No dependency on BG Tok is introduced.

Android should retain camera, microphone, surfaces and hardware codecs on the
platform; moving frames through JNI is not required by this component. Video
note capability selection, crop, export and diagnostics remain future work.
Windows requires a real Media Foundation or other backend and device testing.
iOS can use a future AVFoundation adapter for new feed scenarios; existing
Telegram media paths remain untouched. None of these adapters is implemented.
