# Releasing the VortiDeck OBS Companion

`buildspec.json` is the single version source. VortiDeck does not require a copied version or checksum in repository variables.

## One-time setup

Signing is optional during the current development phase. Without a certificate, the workflow publishes an explicitly unsigned artifact whose checksum and source provenance are still enforced. When production signing is enabled, configure:

- `WINDOWS_CODESIGN_CERTIFICATE_BASE64`
- `WINDOWS_CODESIGN_CERTIFICATE_PASSWORD`

New publicly trusted code-signing keys are normally hardware/HSM protected. If the certificate is not exportable as a PFX, replace the signing step with the certificate provider's cloud or hardware-token integration; do not export or commit a private key. Set VortiDeck repository variable `REQUIRE_OBS_COMPANION_AUTHENTICODE=true` only after that provider is configured.

Optionally configure and enable the documented Apple and Linux signing variables when those artifacts are ready. Windows x64 remains the required release platform.

## Normal developer flow

1. Develop and test the plugin.
   Run `cmake --preset windows-ci-x64`, build the preset, and run `ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure`. The overlay capability policy and structured mDNS end-to-end tests are mandatory.
2. Commit and push the complete intended source, workflow, compatibility template, and submodule pointers.
3. Run `./scripts/release.ps1 patch`, `minor`, or `major`. The helper requires clean release metadata and an up-to-date `main`, updates only `buildspec.json`, creates the matching commit and tag, and pushes both.

For local manager testing without a release, run `./scripts/stage-local-companion.ps1 -DllPath <built-dll> -VortiDeckRoot <checkout>`, then rebuild VortiDeck. This regenerates the embedded manifest and hash but does not publish or sign anything.

The workflow automatically:

- derives tag `v<buildspec version>`;
- creates a draft release at the exact source commit;
- builds the Windows x64 DLL and signs it when a signing identity is configured;
- names the artifact `vorti-obs-plugin-windows-x64.dll`;
- produces its SHA-256 sidecar;
- generates `release-compatibility.json` with the version, source commit, filename, architecture, and hash;
- uploads optional enabled macOS/Linux packages;
- publishes the release only if every required/enabled job succeeds.

A failed build leaves a draft rather than advertising a partial release. Re-running the workflow may repair that draft. Once a version is published, increment `buildspec.json`; published releases are immutable.

## How the desktop consumes it

Publish VortiDeck normally with a `desktop-v*` tag. Its workflow automatically resolves the latest stable companion release, verifies all recorded checksums and compatibility metadata, and verifies Authenticode when production enforcement is enabled. It stages that exact DLL as an application build resource; Rust embeds it inside `VortiDeck.exe`, while WiX remains responsible only for installing VortiDeck. The workflow uploads `obs-companion-provenance.json` beside the desktop release.

At runtime, VortiDeck's OBS Companion manager installs the embedded recovery copy transactionally. It detects standard, custom, and portable OBS roots, checks PE architecture, refuses foreign files, stages and verifies replacements, writes an ownership receipt, and rolls back failed operations. The manager does not require GitHub access during installation.

No DLL copying and no version/hash repository-variable synchronization are part of the normal flow. A manually dispatched desktop build can supply `obs_companion_tag` only when an intentional rollback is needed.

## Discovery behavior

The companion continuously scans for `_vortideck._tcp.local.` and automatically uses a discovered endpoint; users do not need to open the service dialog or press Refresh during normal startup. Refresh is a recovery and diagnostic action: it clears the displayed discovery snapshot, wakes the existing worker, waits for a real bounded DNS-SD scan, and updates the dialog only after that scan completes.

Discovery consumes structured PTR, SRV, A, and AAAA records directly from mDNS packets. Diagnostic log text is never treated as protocol data. Discovery failures are routed into the OBS log. No loopback or hard-coded endpoint is introduced when discovery fails.

The `mdns-discovery-e2e` test starts a real test advertiser and verifies endpoint assembly plus the streaming callback. It can also probe a running VortiDeck instance:

`build_x64/RelWithDebInfo/mdns-discovery-e2e.exe --probe-existing 9001`

## Future distributions

- Windows ARM64: add a `windows-ci-arm64` build and signed `vorti-obs-plugin-windows-arm64.dll`; never load the x64 DLL into native ARM64 OBS.
- Portable Windows: the VortiDeck manager already installs the embedded x64 DLL into a user-confirmed portable instance and records ownership. A separately downloadable ZIP can be added later as a manual recovery channel.
- Custom OBS paths: the manager already uses registry/common/running-process discovery as hints, accepts explicit selection, confirms the target, inspects architecture, and manages repair/removal. MSI must never scan disks or silently modify detected installations.
