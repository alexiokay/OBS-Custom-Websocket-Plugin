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
2. Choose the next semantic version once in `buildspec.json`.
3. Commit and push the complete intended source, workflow, compatibility template, and submodule pointers.
4. In GitHub Actions, run **Native OBS Companion Release** on that commit.

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

Publish VortiDeck normally with a `desktop-v*` tag. Its workflow automatically resolves the latest stable companion release, verifies all recorded checksums and compatibility metadata plus the DLL's Authenticode signature, and embeds that exact DLL into the MSI. It uploads `obs-companion-provenance.json` beside the desktop release.

No DLL copying and no version/hash repository-variable synchronization are part of the normal flow. A manually dispatched desktop build can supply `obs_companion_tag` only when an intentional rollback is needed.

## Future distributions

- Windows ARM64: add a `windows-ci-arm64` build and signed `vorti-obs-plugin-windows-arm64.dll`; never load the x64 DLL into native ARM64 OBS.
- Portable Windows: publish a ZIP with `obs-plugins/64bit` and `data/obs-plugins/vorti-obs-plugin`; VortiDeck's future companion manager must install it only into a user-confirmed portable instance and record ownership.
- Custom OBS paths: use registry and running-process discovery as hints, confirm the target, inspect architecture, and manage repair/removal explicitly. Do not make MSI scan disks or silently modify every detected installation.
