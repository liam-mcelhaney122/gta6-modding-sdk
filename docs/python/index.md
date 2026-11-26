# Python developer documentation

Build offline mods and tooling with the `gta6mod` Python package. This documentation targets Python 3.10 or later and the SDK's build 1.0.2.1 data.

> **Status:** The script layer is tested against mocks; the in-game runtime has not yet run in game. Native names, signatures, controls and models may be incomplete or unverified. A successful mock run does not establish in-game behavior. Use the SDK offline and in single-player only.

## Documentation

- [Get started](getting-started.md): set up the package, run a mod and simulate input.
- [Python API reference](reference.md): mod lifecycle, game helpers, scheduling and runtime.
- [Native API index](../api/index.md): generated native signatures, hashes and confidence information.
- [Native signatures](../native-signatures.md): argument and return marshalling.
- [Script layer design](../script_layer.md): implementation details, mock semantics and known gaps.
- [Examples](../../examples/python): complete sample mods.

For dump inspection and pattern verification commands, see the [SDK README](../../README.md). The [runtime loader README](../../runtime/loader/README.md) describes the separate in-process runtime and its current limitations.
