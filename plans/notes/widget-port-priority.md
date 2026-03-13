# Widget Port Priority Queue (n00b-slop -> n00b-athens)

## Queue Goal

Publish one implementation-ready backlog for the 38 widgets missing from Athens
after parity accounting (`list` in slop treated as `list_widget` in Athens).

## Evidence Basis

- Prototype widget inventory and coupling/test signals from:
  - `n00b-slop/include/ctui/widgets/*.h`
  - `n00b-slop/src/ctui/widgets/*.c`
  - `n00b-slop/test/ctui/*.c`
- Athens current widget runtime and implemented surface from:
  - `n00b-athens/include/display/widget.h`
  - `n00b-athens/include/display/widgets/*.h`
  - `n00b-athens/src/display/widgets/*.c`

## Prioritized Waves

### Wave 1 - Foundation And Layout Unlock

Widgets: `stack`, `grid`, `split`, `scroll`, `tabs`, `text`

Rationale: These unlock container composition and scrollable/tabbed page structure that many higher-level widgets depend on. They have enough prototype depth (`grid`, `split`, `scroll`, `tabs`, `text` with test references) to guide Athens-native interfaces while still being central enough to de-risk later waves.

### Wave 2 - Form And Shell Primitives

Widgets: `header`, `footer`, `statusbar`, `select`, `maskedinput`, `slider`, `calendar`, `timepicker`

Rationale: This wave completes common application shell and form controls needed for realistic demos/apps immediately after layout primitives. `select` is explicitly added because it is present in slop and missing from Athens roadmap coverage.

### Wave 3 - Overlay And Command Interaction

Widgets: `modal`, `tooltip`, `toast`, `collapsible`, `accordion`, `menu`, `commandpalette`

Rationale: These interaction layers benefit from having stable focus, layout, and form primitives first. They are user-visible but can safely follow the foundational waves to avoid rework in event/focus contracts.

### Wave 4 - Data Visualization And Decorative Status

Widgets: `sparkline`, `chart`, `plot`, `digits`, `gradient`, `spinner`, `log`

Rationale: These provide observability and dashboard richness with moderate coupling and fewer cross-widget dependencies than overlays or backend-heavy controls. They are good value once primary interaction flows are in place.

### Wave 5 - Backend-Heavy And Complex Integrations

Widgets: `tree`, `filebrowser`, `editor`, `terminal`, `image`, `canvas`, `asciiart`, `asciify`, `fontify`, `colorpicker`

Rationale: These have the largest migration risk due to notcurses and/or PTY/rendering assumptions in prototype code and should wait for a mature Athens multi-backend substrate. `asciify` is explicitly retained in this deferred wave so roadmap parity stays complete.

## Completeness Check

- Missing-widget target count: 38
- Queue entries across Waves 1-5: 38
- Duplicates: none
- Unassigned missing widgets: none

## Ordered Backlog (Flattened)

1. `stack`
2. `grid`
3. `split`
4. `scroll`
5. `tabs`
6. `text`
7. `header`
8. `footer`
9. `statusbar`
10. `select`
11. `maskedinput`
12. `slider`
13. `calendar`
14. `timepicker`
15. `modal`
16. `tooltip`
17. `toast`
18. `collapsible`
19. `accordion`
20. `menu`
21. `commandpalette`
22. `sparkline`
23. `chart`
24. `plot`
25. `digits`
26. `gradient`
27. `spinner`
28. `log`
29. `tree`
30. `filebrowser`
31. `editor`
32. `terminal`
33. `image`
34. `canvas`
35. `asciiart`
36. `asciify`
37. `fontify`
38. `colorpicker`
