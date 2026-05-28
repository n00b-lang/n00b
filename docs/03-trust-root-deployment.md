# WP-015 — Trust-root deployment

This note describes how the trust root for the `naudit` verifier is
deployed on production machines, and how the in-repo fingerprint
binding binds a repository to a particular trust root. See the
signed-exemption-records white paper § 9, § 10, and § 12 for the
threat model and § 6.3 for the rule-file signature mechanism.

## Architecture: two binaries, one source

Per locked decision D-X1, `naudit` ships in two configurations from
one source tree:

  - `/usr/local/bin/naudit` — the production verifier. Installed
    via `meson install`; owned by root, not writable by the
    developer without elevated privileges.
  - `./build_debug/naudit` — the in-tree development build.
    Agent-modifiable; used for local self-test. Findings from the
    in-tree build do NOT count as audit-passes for any
    agent-resistant workflow.

The naudit executable already carries `install: true` in
`meson.build`, so `meson install` drops it at
`<prefix>/bin/naudit` (default prefix `/usr/local`). Override via
`meson configure -Dprefix=/opt/...` for non-standard layouts.

## Trust roster lookup-order chain

The verifier resolves the `allowed_signers` roster via a 3-slot
chain. First slot whose path exists wins:

  1. **ENV slot** — `NAUDIT_ROSTER` env var. Top-level
     user / CI override. Wins over everything else when set;
     points at an absolute path.
  2. **SYSTEM slot** — `/etc/naudit/allowed_signers` by default.
     Tests use `NAUDIT_SYSTEM_ROSTER` to override the path (this
     env var is **test-injection only**; it substitutes the
     SYSTEM-slot path without affecting the ENV-slot semantics).
  3. **REPO slot** — `<project_root>/audit/allowed_signers`.
     Falls back here for projects that haven't deployed a system
     roster. The verifier emits a warning when this slot is the
     source: a repo-resident roster is only safe if commits
     modifying it are commit-signed by an already-trusted signer
     (which `naudit` does NOT enforce — see "Out of scope"
     below). Pass `--repo-protected` to downgrade the warning to
     informational when the running environment (CI / pre-commit
     hook) makes that assumption safe.

The two env vars (`NAUDIT_ROSTER` and `NAUDIT_SYSTEM_ROSTER`) are
NOT the same: the first is the user/CI override; the second is
the test-injection override for the SYSTEM-slot path.

## Trust-root fingerprint binding (§ 9.1)

When the SYSTEM slot is the active roster, the verifier can
additionally bind the repository to a specific trust root via
the `@expected_roster_sha256` directive in `audit-rules.bnf`:

```
@expected_roster_sha256 6c3a...e8f1   # 64-char lowercase-hex SHA-256
```

At audit time, the verifier hashes the on-disk SYSTEM-slot
roster and compares it to the directive value. Mismatch refuses
the audit with a clear error pointing the operator at both the
expected and the actual digest.

This binds the repo to a known trust root: an attacker who
substitutes a different roster on a different machine (e.g.,
their own laptop) fails the binding check. Update the directive
after auditing the trust roster.

The directive is OPTIONAL — when absent, the binding step is
skipped (the operator chose not to bind the repo to a specific
trust root). ENV-source rosters skip binding by design (the
explicit override is treated as an intentional bypass).

To compute the digest for your roster:

```
shasum -a 256 /etc/naudit/allowed_signers   # or `sha256sum` on Linux
```

## Optional rule-file signatures (§ 6.3)

`audit-rules.bnf` may itself be signed via the same
ssh-keygen-based mechanism as exemption records. To sign:

```
naudit --sign-rules <path-to-audit-rules.bnf> \
       --key /path/to/ssh-private-key \
       --signer your-principal-id
```

This produces `<audit-rules.bnf>.sig` next to the rule file.

Verifier behaviour:

  - **Signed rule with roster signer** → silent accept.
  - **Unsigned rule** → warn. Prominent without
    `--repo-protected`; informational with.
  - **Signed rule with non-roster signer** → refuse.

Agents (or attackers) modifying `audit-rules.bnf` without
re-signing will trip the unsigned-rule warning; this is the
operator's signal that the rules running against the audit do
not match what the maintainer last approved.

## Out of scope (deferred)

These properties are NOT enforced by WP-015 and are flagged for
follow-up work:

  1. **Commit-signed in-repo roster (§ 9.2).** When the REPO
     slot is the active roster, the verifier emits a warning
     but does NOT verify that the commit that touched
     `audit/allowed_signers` was itself signed by a previously-
     trusted signer. CI must enforce commit-signing on roster
     mutations for that property to hold.
  2. **Packaging.** WP-015 ships only the `install: true`
     directive on the meson target; Homebrew formulas, deb /
     rpm packages, and signed installer bundles are downstream
     packaging work.
  3. **Bootstrap.** First roster install on a fresh machine is
     outside the threat model per white paper § 9.3.

## Test injection

For unit / integration tests that need a custom SYSTEM-slot
roster path without writing to `/etc/`, set:

```
NAUDIT_SYSTEM_ROSTER=/tmp/test-roster/allowed_signers
```

`NAUDIT_SYSTEM_ROSTER` is documented as test-injection-only;
production deployments should NOT rely on it.

## Operator playbook

  1. **Initial deploy.** Install `naudit` system-wide:
     `meson install` from the `build_debug` tree (or use a
     downstream package once those land).
  2. **Roster placement.** Drop the maintainer's OpenSSH
     `allowed_signers` at `/etc/naudit/allowed_signers`. Set
     mode 0644, owner root.
  3. **Bind the repo (optional but recommended).** Compute
     `shasum -a 256 /etc/naudit/allowed_signers` and add
     `@expected_roster_sha256 <hex>` to `audit-rules.bnf`.
  4. **Sign the rules.** `naudit --sign-rules audit-rules.bnf
     --key ~/.ssh/id_ed25519 --signer maintainer@example.com`.
     Commit `audit-rules.bnf.sig` alongside `audit-rules.bnf`.
  5. **CI assertion.** Add `--repo-protected` to the CI naudit
     invocation so the rule-file + REPO-slot warnings stay
     informational in green-build output.
