# Widget Port Inventory (n00b-slop -> n00b-athens)

## Scope

This inventory compares prototype widgets in `n00b-slop/include/ctui/widgets/*.h`
against current Athens widget headers in `n00b-athens/include/display/widgets/*.h`.
It is the reproducible baseline for migration prioritization.

## Repro Command Baseline

Run from `/home/baron/crash-override/n00b-tui`:

```bash
slop=$(ls n00b-slop/include/ctui/widgets/*.h | xargs -n1 basename | sed 's/\.h$//' | sort)
athens=$(ls n00b-athens/include/display/widgets/*.h | xargs -n1 basename | sed 's/\.h$//' | sort)
comm -12 <(printf '%s\n' "$slop") <(printf '%s\n' "$athens")
comm -23 <(printf '%s\n' "$slop") <(printf '%s\n' "$athens")
```

Observed on 2026-03-10:

- `n00b-slop` widget headers: 52
- `n00b-athens` widget headers: 14
- Direct overlap: 13
- Raw missing from Athens: 39
- Semantic rename: `list` in slop is represented by `list_widget` in Athens
- Effective missing set to port: 38

## Full Widget Sets

### n00b-slop (52)

- `accordion`
- `asciiart`
- `asciify`
- `box`
- `breadcrumb`
- `button`
- `calendar`
- `canvas`
- `chart`
- `checkbox`
- `collapsible`
- `colorpicker`
- `commandpalette`
- `digits`
- `divider`
- `editor`
- `filebrowser`
- `fontify`
- `footer`
- `gradient`
- `grid`
- `header`
- `image`
- `input`
- `label`
- `link`
- `list`
- `log`
- `maskedinput`
- `menu`
- `modal`
- `plot`
- `progress`
- `radio`
- `scroll`
- `select`
- `selectionlist`
- `slider`
- `spacer`
- `sparkline`
- `spinner`
- `split`
- `stack`
- `statusbar`
- `switch`
- `tabs`
- `terminal`
- `text`
- `timepicker`
- `toast`
- `tooltip`
- `tree`

### n00b-athens (14)

- `box`
- `breadcrumb`
- `button`
- `checkbox`
- `divider`
- `input`
- `label`
- `link`
- `list_widget`
- `progress`
- `radio`
- `selectionlist`
- `spacer`
- `switch`

## Direct Overlap (13)

- `box`
- `breadcrumb`
- `button`
- `checkbox`
- `divider`
- `input`
- `label`
- `link`
- `progress`
- `radio`
- `selectionlist`
- `spacer`
- `switch`

## Semantic Rename Handling

- `list` (`n00b-slop`) is treated as parity-covered by `list_widget` (`n00b-athens`), so it is excluded from the missing queue.

## Missing Widgets To Port (38)

- `accordion`
- `asciiart`
- `asciify`
- `calendar`
- `canvas`
- `chart`
- `collapsible`
- `colorpicker`
- `commandpalette`
- `digits`
- `editor`
- `filebrowser`
- `fontify`
- `footer`
- `gradient`
- `grid`
- `header`
- `image`
- `log`
- `maskedinput`
- `menu`
- `modal`
- `plot`
- `scroll`
- `select`
- `slider`
- `sparkline`
- `spinner`
- `split`
- `stack`
- `statusbar`
- `tabs`
- `terminal`
- `text`
- `timepicker`
- `toast`
- `tooltip`
- `tree`

## Migration-Risk Indicators (Prototype Signals)

Source command:

```bash
for f in n00b-slop/src/ctui/widgets/*.c; do
  w=$(basename "$f" .c)
  [ -f "n00b-athens/include/display/widgets/$w.h" ] && continue
  [ "$w" = "list" ] && continue
  loc=$(wc -l < "$f")
  nc=$( (rg -n "\b(struct nc|ncplane_|nctree|nctabbed|notcurses|ncinput|nccell_)" "$f" || true) | wc -l )
  tests=$( (rg -n "ctui_${w}|test_${w}|${w}_widget" n00b-slop/test/ctui || true) | wc -l )
  printf "%s|loc=%s|nc_refs=%s|test_refs=%s\n" "$w" "$loc" "$nc" "$tests"
done | sort
```

- `accordion|loc=272|nc_refs=0|test_refs=0`
- `asciiart|loc=446|nc_refs=7|test_refs=0`
- `asciify|loc=586|nc_refs=7|test_refs=0`
- `calendar|loc=745|nc_refs=16|test_refs=4`
- `canvas|loc=741|nc_refs=12|test_refs=0`
- `chart|loc=723|nc_refs=0|test_refs=0`
- `collapsible|loc=290|nc_refs=15|test_refs=0`
- `colorpicker|loc=1009|nc_refs=41|test_refs=0`
- `commandpalette|loc=571|nc_refs=53|test_refs=0`
- `digits|loc=514|nc_refs=10|test_refs=0`
- `editor|loc=1553|nc_refs=28|test_refs=0`
- `filebrowser|loc=517|nc_refs=0|test_refs=0`
- `fontify|loc=493|nc_refs=7|test_refs=0`
- `footer|loc=197|nc_refs=10|test_refs=0`
- `gradient|loc=275|nc_refs=15|test_refs=0`
- `grid|loc=638|nc_refs=8|test_refs=18`
- `header|loc=254|nc_refs=14|test_refs=0`
- `image|loc=565|nc_refs=23|test_refs=0`
- `log|loc=561|nc_refs=13|test_refs=0`
- `maskedinput|loc=526|nc_refs=11|test_refs=0`
- `menu|loc=314|nc_refs=11|test_refs=0`
- `modal|loc=458|nc_refs=46|test_refs=0`
- `plot|loc=486|nc_refs=11|test_refs=0`
- `scroll|loc=943|nc_refs=50|test_refs=121`
- `select|loc=496|nc_refs=34|test_refs=0`
- `slider|loc=441|nc_refs=16|test_refs=0`
- `sparkline|loc=469|nc_refs=11|test_refs=0`
- `spinner|loc=193|nc_refs=3|test_refs=0`
- `split|loc=457|nc_refs=28|test_refs=15`
- `stack|loc=181|nc_refs=0|test_refs=0`
- `statusbar|loc=329|nc_refs=24|test_refs=0`
- `tabs|loc=615|nc_refs=19|test_refs=6`
- `terminal|loc=994|nc_refs=16|test_refs=2`
- `text|loc=535|nc_refs=13|test_refs=49`
- `timepicker|loc=484|nc_refs=15|test_refs=0`
- `toast|loc=359|nc_refs=24|test_refs=0`
- `tooltip|loc=330|nc_refs=28|test_refs=0`
- `tree|loc=735|nc_refs=21|test_refs=53`

## Interface Gap Snapshot

- Prototype side: `ctui_widget_t` classes with direct notcurses plane ownership in `n00b-slop/include/ctui/widget.h`.
- Athens side: plane-attached behavior through `n00b_widget_vtable_t` in `n00b-athens/include/display/widget.h`.

This confirms that ports must be semantic migrations into Athens widget/plane APIs, not file copies.
