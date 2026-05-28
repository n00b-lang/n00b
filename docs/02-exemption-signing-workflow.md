# Exemption signing workflow (WP-014)

This note describes the human-side workflow for the signed-exemption-
records discipline. The data plane (signatures, blame anchor, schema)
ships in WP-011 / WP-012 / WP-013; WP-014 adds the interactive
ceremony that turns an agent-authored proposal into a signed exemption.

The principle (per the white paper § 11): the rationale field is a
*human* judgment artifact. Agents propose; humans approve. The
ceremony makes the human approval moment visible and tamper-resistant.

## How a proposal is written

Per **D-X4** (proposals separate from signing), an agent may write a
draft `.bnf` exemption file without a `.sig`. The agent populates:

- `@rule_id` — content hash of the rule being exempted.
- `@locator_*` + `@region_fingerprint` — where the exempted span is.
- `@file_path` — the audited file (repo-relative).
- `@rationale` — best-effort draft text (will be BLANKED before the
  developer's prompt).
- `@version 1`.

The draft lands at `audit/exemptions/<id>.bnf`. No `.sig` sibling
means "pending" / "proposal."

## Running the signing ceremony

```
naudit --sign-pending --key <ssh-private-key> --signer <principal>
```

Walks every pending proposal (a `.bnf` under `audit/exemptions/`
lacking a `.sig` sibling) one at a time. For each proposal:

1. Prints the rule's id + summary text.
2. Prints the audited file path, locator, and a region preview
   (three lines of context on each side; exempted lines marked `>`).
3. **Blanks the rationale field** so the developer's input isn't
   pre-filled by agent text (§ 11.2).
4. Prompts for a new rationale. Multi-line input; terminator is a
   `.` on its own line (mutt-style). Blank lines are kept verbatim
   so multi-paragraph rationales are preserved.
5. Prompts for an expiration date. Accepts:
   - `YYYY-MM-DD` (ISO-8601 calendar form), or
   - `Nd` / `Nm` / `Ny` shorthand (days / months ≈ 30d / years ≈
     365d from today), or
   - a blank line for the default of today + 365 days.
6. Prompts approve / decline. Anything matching `y` / `Y` / `yes` /
   `YES` (trimmed) approves; everything else declines.
7. On approve: rewrites the `.bnf` (atomically: write to `.tmp` then
   rename) with the developer's rationale + expiration, then invokes
   the WP-012 signing primitive to drop a `.sig` sibling.
8. On decline: leaves the `.bnf` unchanged. The proposal remains
   pending and will surface again on the next ceremony invocation.

No bulk-sign in the standard ceremony. Per-item review is the
defining property.

## Initial-adoption bulk-sign

```
naudit --initial-adoption --key <ssh-private-key> --signer <principal>
```

A team adopting naudit on a non-greenfield codebase will typically
face a backlog of pre-existing findings that need exemptions. Walking
hundreds of those records one at a time on day one is impractical.
Per the white paper § 11.3, the `--initial-adoption` flag bulk-signs
every pending proposal with:

- Rationale: `preexisting; scheduled for review by <today + 90 days, ISO-8601>`.
- Expiration: today + 90 days.

The friction of per-item review isn't skipped — it's moved to the
90-day expiration boundary, when the engine will refuse the exemption
unless re-reviewed. Use the flag **once**, at adoption.

## Expiration enforcement

WP-014 wires expiration enforcement into
`n00b_audit_exemption_match`: an exemption whose `expires_at` is in
the past does NOT suppress its finding, even with a valid signature
and clean blame trace (§ 11.4). The check runs at the top of the
matcher (before blame / fingerprint work) so expired exemptions
short-circuit cheaply. Comparison is lexicographic on the ISO-8601
strings, which is correct for both `YYYY-MM-DD` and the full
`YYYY-MM-DDTHH:MM:SSZ` instant form.

An exemption with no `expires_at` field never expires; that case is
preserved for forward-compat with WP-011 records but the ceremony
always writes the field.

## Why a `.`-on-its-own-line terminator

The multi-line rationale prompt could end on a blank line (the
intuitive "press Enter twice"). We picked the mutt-style `.`-on-its-
own-line terminator instead because the blank-line option silently
truncates multi-paragraph rationales — a developer writing:

```
This exemption covers the legacy DSP loop.

The vendor's SDK is not updateable until Q3; see JIRA-12345.
```

would have the rationale chopped at the first blank line. The
`.`-terminator is rare in human text, the prompt tells the developer
to use it, and the cost of a wrong choice is bounded (the developer
can edit the resulting `.bnf` and re-sign).

## Testing the ceremony without stdin

The signing flow takes an `n00b_naudit_input_source_t *`. Production
wraps `STDIN_FILENO` (`n00b_naudit_input_from_fd(0)`); tests wrap a
pre-loaded `n00b_buffer_t *`
(`n00b_naudit_input_from_buffer(scripted_input)`). The two source
kinds present an identical line-reader contract, so the signing flow
doesn't branch on kind. Unit tests stay in-process and deterministic
— no subprocess spawning, no pipe wiring.

This pattern (DF-BA resolution) keeps the testing story simple and
makes the ceremony amenable to future automation harnesses.
