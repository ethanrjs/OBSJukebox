# Releasing OBS Jukebox

This is for the two maintainers. Players never need it: their mod checks GitHub once per launch and installs a signed release when they close Geometry Dash and OBS.

The updater trusts exactly one key. That key lives in a GitHub Actions secret inside a protected environment, and every release is signed by a workflow that stops until one of you approves it.

## How a release flows

1. You merge the version bump and the release notes to `main`.
2. You push a tag `vX.Y.Z` for that exact commit.
3. `Release` builds the Windows, macOS and Linux files and writes `update.json`, which holds the size and SHA-256 of the two setup files.
4. The workflow stops at the `sign` job. One of you reads the summary and approves.
5. `sign` reads the secret, signs `update.json`, and deletes the key file.
6. `draft` checks the signature against `scripts/update-signing-public.pem` and creates a draft release.
7. You publish the draft. Players see the update on their next launch.

A release published by hand has no `update.json`. If it becomes the latest release, every player's check gets a 404 and offers stop until the next workflow release. Publish hand-made releases as prereleases, which `releases/latest` skips.

## One-time setup

Do this once, in this order. Either maintainer can do it, but both accounts need admin on the repository.

### 1. Create the signing environment

Settings → Environments → New environment → `release-signing`.

- Required reviewers: ethanrjs and navinor.
- Prevent self-review: off. Either of you can approve the run that they pushed the tag for.
- Allow administrators to bypass configured protection rules: off. GitHub turns this on by default.
- Deployment branches and tags: "Selected branches and tags", one tag rule `v*.*.*`, no branch rules. A branch or pull request can then never reach the environment.

### 2. Make the key and store it

On your own computer, in an empty folder, with OpenSSL (Git for Windows ships it):

```sh
openssl ecparam -name prime256v1 -genkey -noout -out update-signing-key.pem
openssl ec -in update-signing-key.pem -pubout -out update-signing-public.pem
gh secret set UPDATE_SIGNING_KEY --env release-signing --repo ethanrjs/OBSJukebox < update-signing-key.pem
```

`gh` reads the secret from the file, so it never passes through the clipboard. Save the contents of `update-signing-key.pem` as a secure note in a password manager, then delete the file. GitHub never shows a secret again. Without the backup, losing the key means every player updates by hand once.

Send `update-signing-public.pem` to whoever prepares the code. It is committed as `scripts/update-signing-public.pem`, and `src/UpdateKey.hpp` is generated from it:

```sh
python scripts/update-key-header.py scripts/update-signing-public.pem src/UpdateKey.hpp
python tools/qa/test_update_manifest.py
```

The public key is safe to publish. Only the private key signs releases.

### 3. Turn on immutable releases

Settings → General → Releases → enable release immutability. Once a release is published, its assets and tag lock, so nobody can replace a file under players' hash checks.

### 4. Restrict release tags

Settings → Rules → Rulesets → New tag ruleset `release tags`, target `v*.*.*`, enforcement Active. Turn on "Restrict creations", "Restrict updates", "Restrict deletions" and "Block force pushes". Bypass list: Repository admin, since both maintainers have that role. Nobody else can create a release tag.

### 5. Leave `main` unprotected, by choice

Both of you push to `main` directly. If a third collaborator appears, add a branch ruleset with "Require a pull request before merging". Nothing in the updater depends on it.

### 6. Secure both accounts

Add a passkey or hardware security key as a two-factor method and remove SMS. Both accounts have admin, so this is the protection that matters. The approval step catches an accidental tag, not a compromised maintainer account.

### Check the setup

```sh
gh api repos/ethanrjs/OBSJukebox/environments --jq '.environments[].name'
gh secret list --env release-signing --repo ethanrjs/OBSJukebox
gh api repos/ethanrjs/OBSJukebox/rulesets --jq '.[].name'
```

Expected: `release-signing`; `UPDATE_SIGNING_KEY` with a recent timestamp; `release tags`.

## Every release

### 1. Prepare

- Set `version` in `mod.json` to `vX.Y.Z`. The workflow refuses a tag that disagrees with it.
- Add `release-notes/vX.Y.Z.txt`. The workflow refuses a missing file.
- Push to `main` and let the normal CI pass.

The version in `mod.json` is the one players see. `update.json` is written from the tag, the file names and `mod.json`, and all three must agree.

### 2. Tag the merge commit

Tag by SHA, never by branch name, so the tag cannot drift:

```sh
git fetch origin
git tag vX.Y.Z <sha>
git push origin vX.Y.Z
```

The tag ruleset lets only admins create it.

### 3. Approve signing

The workflow builds and tests everything (about 45 minutes), then waits at `sign` and emails both reviewers.

Open the run → `manifest` job summary → check:

- **Commit** is the one you just merged, and **On main** says yes. "NO" means the tag points at a branch commit; reject.
- **update.json** holds the intended version and two file entries with plausible sizes.
- **.github changes** since the previous release are empty, or only what you both reviewed. This is the one place a workflow change could slip in next to the key.
- **SHA-256 of the release files** is shown for the record.

Then approve `sign`. If anything looks wrong, reject: the job ends, the key is never read, and nothing is published. Delete the tag, fix, and push a new one.

`sign` runs with `permissions: {}` and only its signing step gets the secret. It writes the key to `$RUNNER_TEMP` with `umask 077`, signs, and deletes the file in an `always()` step.

### 4. Publish the draft

`draft` verifies the signature against the committed public key, then creates a draft release named `OBS Jukebox vX.Y.Z` with five files:

- `OBS-Jukebox-X.Y.Z-Windows-Setup.exe`
- `OBS-Jukebox-X.Y.Z-macOS-arm64.zip`
- `OBS-Jukebox-Linux-x86_64.tar.gz`
- `update.json`
- `update.json.sig`

Read the notes, then publish. With immutable releases on, the assets and tag lock at that moment.

### 5. Check it reached players

- Open the release's `update.json` URL: `https://github.com/ethanrjs/OBSJukebox/releases/latest/download/update.json`. It must be the new version.
- On a test machine with the previous release, launch Geometry Dash. The panel offers the new version; "Update now" exits GD, installs, and reopens it.
- Check the release page for a missing `update.json.sig` or a wrong file name. A missing file means the release was not made by the workflow.

The first release with the updater needs a manual install by every player, because the mod that looks for updates is inside it. Say so in the notes.

## Managing the secret

### See what exists

```sh
gh secret list --env release-signing --repo ethanrjs/OBSJukebox
```

The value is never shown. The timestamp only says when it was set.

### Replace the key with the same one

If the secret was lost but you still have the saved private key:

```sh
gh secret set UPDATE_SIGNING_KEY --env release-signing --repo ethanrjs/OBSJukebox < update-signing-key.pem
```

### Rotate to a new key

Installed mods trust only the key compiled into them, so a new key needs one manual install:

1. Make a new key pair (step 2 above).
2. Replace `scripts/update-signing-public.pem`, regenerate `src/UpdateKey.hpp`, run `python tools/qa/test_update_manifest.py`, and merge.
3. Replace the `UPDATE_SIGNING_KEY` secret.
4. Ship a release through the workflow. It is signed with the new key, which the release itself trusts. Players on the old key must install this release by hand.
5. Say so in the notes and wherever players follow the project.

### Delete the secret

```sh
gh secret delete UPDATE_SIGNING_KEY --env release-signing --repo ethanrjs/OBSJukebox
```

Do this first if the key leaks or the environment looks wrong. Releases then fail at `sign` until a new key is stored.

### Test on the fork

The fork `navinor/OBSJukebox` can run the whole flow without touching players:

- Give the fork its own `release-signing` environment and secret.
- Use a throwaway key, never the production one:

```sh
bash tools/qa/make-update-fixtures.sh /tmp/obs-jukebox-key
gh secret set UPDATE_SIGNING_KEY --env release-signing --repo navinor/OBSJukebox < /tmp/obs-jukebox-key/test-key.pem
```

- Point the QA build at the fork's releases with `OBS_JUKEBOX_UPDATE_BASE_URL` and `OBS_JUKEBOX_UPDATE_PUBLIC_KEY`. Release builds ignore both variables, and the QA test key is never committed.
- Delete the fake releases and tags afterwards.

## If the key leaks or is lost

1. Delete the `UPDATE_SIGNING_KEY` secret.
2. Make a new key pair and follow "Rotate to a new key".
3. If the key leaked, anyone who can also publish a release on this repository could push an update to players until they install the new release. Check the repository's releases and tags, and both accounts' security logs, for anything unexpected.

## Rules

- Never add `environment: release-signing` to another workflow or job.
- Never run signing on `pull_request` or `pull_request_target`.
- Never print the secret, and never turn on shell tracing (`set -x`) in the `sign` job.
- Keep `sign` on GitHub-hosted runners. A self-hosted runner keeps files between jobs.
- Keep every action in `release.yml` pinned by commit SHA. A moved tag on a third-party action would run next to the secret.
- Don't approve a run you didn't expect. A run waiting for approval that nobody started means an account may be compromised.
- Never replace an asset after publishing. Immutable releases enforce it; without them, every player's hash check fails until a new release ships.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `manifest` fails with `Versions disagree` | `mod.json` or a setup file name does not match the tag. Fix `mod.json`, push, delete the tag, tag again. |
| `draft` fails with `Missing release-notes/...` | Add the notes file for the tag, push, delete the tag, tag again. |
| `On main: NO` | The tag points at a branch commit. Tag the merge commit's SHA instead. |
| `sign` never starts | The environment has no reviewer available, or the tag does not match the `v*.*.*` rule. Check the environment's deployment tag rules. |
| `Sign update.json` fails | `UPDATE_SIGNING_KEY` is missing or not a P-256 private key PEM. Replace it from the password-manager copy. |
| `draft` fails verifying the signature | `scripts/update-signing-public.pem` is not the public half of the secret. Regenerate `src/UpdateKey.hpp` from the right key and ship a release. |
| Players see no offer | The latest release is a prerelease, was published by hand, or has no `update.json` / `update.json.sig`. The mod logs "No update information this launch". |
| Players get "Update failed" every time | Check the SHA-256 in `update.json` against the published asset. A replaced asset fails the hash check; publish a new release. |
| A release needs to be withdrawn | Delete the release and the tag. `releases/latest` moves back to the previous published release, which still has a valid `update.json`. |

## Release checklist

```text
[ ] mod.json version is vX.Y.Z
[ ] release-notes/vX.Y.Z.txt exists
[ ] main CI is green
[ ] tag points at the merge commit SHA
[ ] manifest summary: On main yes, update.json version right, .github diff understood
[ ] sign approved
[ ] draft has five files and the right notes
[ ] draft published
[ ] releases/latest/download/update.json shows X.Y.Z
```
