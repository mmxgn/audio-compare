# AppImage packaging tools

The release workflow builds the AppImage with linuxdeploy and two plugin
scripts. They run inside the release job, so they are pinned twice:

- `tools.env` names an immutable reference for each one: a versioned linuxdeploy
  release tag (never `continuous`) and a full commit SHA for each plugin (never
  a branch).
- `tools.sha256` holds the SHA-256 of each file. The workflow runs
  `sha256sum --check --strict` on the downloads before anything is made
  executable, so a changed byte fails the job before downloaded code runs.

## Updating

1. Pick the new linuxdeploy release tag and the new plugin commits:
   ```sh
   gh api repos/linuxdeploy/linuxdeploy/releases --jq '.[].tag_name' | head
   gh api repos/linuxdeploy/linuxdeploy-plugin-gtk/commits/master --jq .sha
   gh api repos/linuxdeploy/linuxdeploy-plugin-gstreamer/commits/master --jq .sha
   ```
2. Download the three files from those references on a machine you trust.
3. Diff the two plugin scripts against the previously pinned versions and read
   the diff. For the AppImage, compare its SHA-256 with the `digest` GitHub
   shows for the release asset
   (`gh api repos/linuxdeploy/linuxdeploy/releases/tags/<tag> --jq '.assets[].digest'`).
4. Regenerate `tools.sha256` with `sha256sum`, update `tools.env`, and open one
   pull request with both changes.
