# Releasing qalam

One tag push builds, tests and publishes everything. The workflow is
`.github/workflows/release.yml`.

| What | Where it ends up |
|---|---|
| `qalam-windows-setup.exe`: installer, adds qalam to PATH | the GitHub release |
| `qalam-windows-x86_64.zip` | the GitHub release |
| `qalam-linux-x86_64.tar.gz`, `qalam-linux-aarch64.tar.gz` (static) | the GitHub release |
| `qalam-macos-universal.tar.gz` (Intel + Apple silicon) | the GitHub release |
| `SHA256SUMS` (checked by `install.sh`) | the GitHub release |
| Docker image, amd64 + arm64 | `ghcr.io/<owner>/qalam:<version>` and `:latest` |

Each one holds qalam with HarfBuzz, FriBidi and zlib compiled in, the fonts in
`fonts/`, the examples with their PDFs, and the licenses.

## First push

Make an **empty** public repository called `qalam` on GitHub (no README, no
license, no .gitignore), then in this folder:

```
git init -b main
git add .
git commit -m "qalam 0.2"
git remote add origin https://github.com/Husseinabdulameer11/qalam.git
git push -u origin main
```

The `ci` workflow runs on that push: Linux (gcc and clang, system libraries),
Linux with gram installed, Linux static (libraries built in), macOS, Windows,
and the Docker image.

## One-time setup

1. **gram must be public, under the same account** (`<owner>/gram`). Every
   workflow checks it out from there into `gram-src`. If it is named
   differently, change `GRAM_REPO` at the top of both workflow files.
   Releases build against gram's `main` branch (`GRAM_REF` in release.yml);
   once gram has version tags, set `GRAM_REF` to one so each qalam release is
   built against a known gram.
2. **Workflow permissions:** Settings → Actions → General → Workflow
   permissions → "Read and write", so the release job can create releases
   and push the image.
3. **After the first release, make the Docker image public.** GitHub creates
   container images as private: your profile → Packages → qalam → Package
   settings → Change visibility → Public. You only do this once.

## Making a release

```
git tag v0.2.0
git push origin v0.2.0
```

Then watch the Actions tab. The release appears under Releases with install
instructions and the generated changelog. The arm64 Docker image and the
aarch64 Linux build compile HarfBuzz under emulation, so they are the slow
part, around 20 minutes.

The version comes from the tag: `v0.2.0` gives `qalam --version` →
`qalam 0.2.0 (gram 0.1.0, HarfBuzz 10.1.0, FriBidi 1.0.16)`. Tags must look like
`v1.2.3` or `v1.2.3-beta.1`.

## Dry run

Actions → release → **Run workflow**. This builds and tests every platform,
including installing and uninstalling the Windows installer on a real
Windows machine. It publishes nothing; the files stay downloadable as
workflow artifacts for a few days. Do this before your first real tag.

## Updating a library

The versions and checksums of HarfBuzz, FriBidi and zlib are at the top of
`tools/deps.sh`. Change both, run `tools/deps.sh && make clean test`, and
update the table in `THIRD_PARTY.md`.

## If something fails

- **gram checkout fails**: gram's repository is missing, private, or named
  differently (see `GRAM_REPO`).
- **A download in tools/deps.sh fails or its checksum doesn't match**: the
  pinned release moved or was replaced; check the version and checksum.
- **Docker push denied**: see workflow permissions above.
- **A release for that tag already exists**: delete the release (and the tag
  if needed) on GitHub, then push the tag again.
