# Contributing

Issues and pull requests are welcome.

- **Discuss larger changes first.** For new protocols, architectural changes or new
  dependencies, please open an issue to agree on the approach before writing code.
- **CI must pass.** Pull requests are merged only when the GitHub Actions workflow is green:
  GCC and Clang builds (Debug and Release, warnings as errors) with unit and integration tests,
  clang-format, clang-tidy, the simulator tests and the Compose demo smoke test.
- **Keep changes focused** and include tests for behavior changes.
- **Scope.** The maintainer decides scope and roadmap
  (see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#8-planned-evolution-not-implemented)).

Building, testing and running the demo locally are described in
[docs/RUNBOOK.md](docs/RUNBOOK.md) (sections 4–8 and 15).

By contributing, you agree that your contributions are licensed under the Apache License 2.0
([LICENSE](LICENSE)).
