# GMCA

## What it does

Gamepad Media Center Aggregator: a controller-first media center for Plex, Jellyfin, Emby,
Stremio and local/remote files, on Switch, PS Vita, PS4, Raspberry Pi and desktop. Public, with
real users filing issues.

## Repo

- `thcolin/gamepad-media-center-aggregator`, base branch `dev`. The local folder is `gmca`.
- A GitHub fork of `dragonflylee/switchfin`. That remote is named `switchfin`, **never `upstream`**:
  Orca picks a remote named `upstream` as the issue source, and worktree cards then link to
  switchfin issues whose numbers overlap ours.
- `gh repo set-default` is `thcolin/gamepad-media-center-aggregator`. Pass `-R` anyway when a
  number matters.
- Older worktrees live under `~/.prowl/repos/switchlex/`.

## Run

```bash
cmake -B build_desktop -G Ninja -DPLATFORM_DESKTOP=ON
cmake --build build_desktop        # produces build_desktop/GMCA.app
```

Console builds (Switch, Vita, PS4) run in CI, see `.github/workflows/build.yaml`.

## Verify

- `tests/run.sh`: standalone logic tests (`tests/test_*.cpp`, no CMake wiring, `BUILD_TESTING` is
  OFF). Add a test there when the logic is pure (JSON parsing, catalog, sorting).
- `cmake --build build_desktop` must pass.
- The CI on the PR must be green: it builds every platform.

## Visual check

- Harness: `scripts/ui-audit/ctl.py` with its `scenarios/`.
- Test servers: `plenx-media-servers/docker-compose.yml` (untracked), Jellyfin on
  `localhost:8096`, Emby on `localhost:8097`, media read-only from `~/Downloads`. Start with
  `docker compose -f plenx-media-servers/docker-compose.yml up -d` and stop it with `down` when done.
- Plex, the consoles and Stremio accounts are not reachable from a session: that part of a
  check is Thomas's, or the issue reporter's.

## Where the tracking lives

- GitHub issues and PRs on `thcolin/gamepad-media-center-aggregator`.
- `.wip/music/todo.md` for the music work, `CHANGELOG.md` for what shipped.

## Autonomy

Full, decided by Thomas on 2026-09-23. This is wider than the personal-project default:

- A `/thcolin:craft` agent commits, opens the PR, gets it reviewed and **merges it into `dev`**
  once the review and the CI pass.
- `/thcolin:workshop` decides **when to release**, based on the fixes and features merged since
  the last tag, and cuts it by following `RELEASING.md` without asking first. It tells Thomas
  afterwards what shipped.
- The repo is public: PRs, review comments and releases are visible. Answering or closing an
  issue is covered by the merge of its fix (`Fixes #N` in the PR).

## Commits

Conventional Commits in English, the scope is the area (`plex`, `vita`, `player`, `config`,
`stremio`...). PRs merge with a merge commit. No AI attribution anywhere.

## Pitfalls

- Keep the `library/borealis` submodule out of unrelated commits.
- A release goes live for users as soon as the tag's `upload-release` job finishes: the in-app
  updater reads `releases/latest`. The tag must match the `CMakeLists.txt` version and
  `CHANGELOG.md` must have its `## [X.Y.Z]` section.
