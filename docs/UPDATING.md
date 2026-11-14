# Updating to a new DuckDB release

erpl-odata builds **one source tree** against several DuckDB lines at once: currently
v2.0.0 (cyanoptera) and v1.5.6 (variegata); 1.4.x LTS support was dropped in #274. API
differences between the lines are absorbed by the header-only shims in the
`duckdb-compat` submodule (`duckdb_compat::...`), not by version checks at call sites.

## Moving a line to a new DuckDB patch or minor release

1. **Submodules**
   - `duckdb`: check out the new tag (`git -C duckdb checkout v1.5.x`). This is the
     version local builds (`make dev`, `make release`) compile against.
   - `extension-ci-tools`: check out the branch matching that release line
     (`v2.0-cyanoptera`, `v1.5-variegata`).
2. **CI** (`.github/workflows/MainDistributionPipeline.yml`)
   - set `duckdb_version` and `ci_tools_version` of the affected build job, and the
     `@<branch>` of its reusable `_extension_distribution.yml`;
   - keep the smoke-test and deploy jobs of that line on the same `duckdb_version`;
   - the `docs-check` job downloads a stock CLI and the matching artifact; keep both on the
     same release. From 2.0 on the CLI asset is `duckdb-cli-<os>-<arch>.tar.gz`, not
     `duckdb_cli-<os>-<arch>.zip`.
3. **Build and test locally**: `make dev && make test_debug`, then `make release &&
   make test_cpp`.
4. **Docs and skills**: `make check_docs`. Update the DuckDB versions named in `README.md`
   (badge), `CLAUDE.md` and the header line of each skill in
   `plugins/erpl-odata/skills/*/SKILL.md`.

## A new DuckDB major version

Follow the `duckdb-2-port` procedure (DataZoo skill): survey compile errors against the
exact DuckDB commit community-extensions builds, add the missing helpers to
`duckdb-compat` first, then call them from here. Watch for behaviour changes the compiler
does not catch (error-message text, fallible scalar functions, what the shell links
statically).

## Releasing

1. Bump the extension version where it is reported (`src/erpl_odata_extension.cpp`: the
   banner and the telemetry `SetProduct` / `CaptureExtensionLoad` calls).
2. Bump `version` in `plugins/erpl-odata/.claude-plugin/plugin.json` and in each skill's
   frontmatter and header line to the same date version, so users see a plugin update.
3. Tag `vYYYY.MM.DD`. Versioned deploy paths come only from the tag run.
4. Update the `ref` of the community-extensions descriptor.
