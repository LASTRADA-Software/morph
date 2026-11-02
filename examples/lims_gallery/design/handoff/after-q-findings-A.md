Reviewer A, after batch Q: esignature, receipt, instrument, audit and samples, at 1000, 1215 and 390, plus receipt and
audit at 1920. Grabs are in `lg-cmp/app-q/`, and the baseline is `app-p/`. All logs are 0 bytes. Sources were read with
`git show lg-fold-q:<path>` (head `dcefd3d15`).

**Method.** A p→q image diff per grab: every grab moved, as expected with 13 px body text. PIL pixel reads for edges,
gaps and ink extents (scripts in `lgqrA/`), checked against the mockup PNG and the `.dc.html`. *Measured* means a pixel
read. *Inferred* means read from code or reasoned.

## E-signature

| Width | Finding |
|---|---|
| all | **Regressions:** none. |
| 1215/1000 | **Q2, 13 px body: closed.** The dialog is 4 px taller. The Reason now wraps "…closed as lab / error.", where the design wraps "…closed / as lab error." (font face, not counted). Cancel is 72 px and Sign and approve 134 px, against the design's 70 and 134. Signer and Meaning are still 226 px each. |
| 1215/1000 | Note, not counted: the record box's rows are 24 px apart (ink at 253/277/301), against 22 at P and 21 in the design (`.kv` gap 4). This follows the fallback face's line height at 13 px (inferred). The instrument Details rows went from 22 to 23 for the same reason. |
| 390 | Banner, Reason and fields match the design's wraps. No change beyond the size. |
| 1000 | No clipping, overlap, lost content or mid-word break. |
| — | **Remaining:** none (A10 stands). |

## Receipt

| Width | Finding |
|---|---|
| 1000 | **Regression, M, screen: the "Label legible and matches the requ…" checkbox label is elided.** P showed it whole. `dcefd3d15`'s message says 31 % "keeps the label … whole", and the grab at that head contradicts it: the session panel is 750–983 (234 px, which is 31 %), and the label still ends in "…". The label needs about 243 px (219 px of ink plus the box, measured at 1215). Its column is about 241. Both this label and the sample type are a few px short on the 785 px page, so no percentage for the session panel fits both the badges and the form at 13 px. **Fix options:** let the condition grid fall to one column below a width, or take the 8 px from the row's `gap = 2` or from the session table's `fixed(11)` Sample column. The framework alternative is a checkbox label that wraps instead of eliding. It is not a gap today, because nothing asks for it. |
| 1000 | **Known residual, L→M, framework: "Release, finished produc" is a silent clip.** The read-only `textInput` cuts the "t" with no ellipsis and no report, and the log is empty. This is the kind of loss a framework report should catch. A read-only input cannot be scrolled into view the way an edited one can, so overflow there is lost content. Yet the checkbox two rows below elides with "…", so one form treats overflow two ways. **What is missing:** a read-only input whose value is wider than its field should elide like other single-line text, or emit the overflow report that the Q6b texts get. The cause is inferred, and I did not read `MorphTextInput.qml`. |
| 1215 | **Q4/S6, legend in the `label` role: closed.** "Condition on receipt" has a cap height of 9 px, the same as "Sample type" (it was 10 at P). Its text is 245–349, 105 px, against 110 at P. |
| 1215 | **Regression, L, screen: the session table's Time column is 54 px**, where it was 62 at P and is 70 in the design. This comes from `fixed(6)`→`fixed(5)`, made for 1000. The Outcome badges now sit 80 px from the panel's edge, against 33 in the design. The cause is the fixed Sample column plus a stretch Outcome column, which was already the case at P. **Fix:** size the Time column from the 1215 look (`fixed(6)` or more), and solve 1000 as in the first row. |
| 1215 | The second condition column is at x 563, as at P, and at 916 at 1920. The scan field, the notice and the card match. Q2 is closed. |
| 1920 | The session pane is capped at 1605–1903 (298 px, the design's 298). The form fills the rest as intended. |
| 390 | **Q1:** Containers and Courier are 175/175. The 13/14 px text fits, and "Internal, QA sampling" is whole. No regression. |
| — | Accepted, not re-reported: the 14 px "Sample" heading, and the 4 px grid gap. |

## Instrument

| Width | Finding |
|---|---|
| all | **Regressions:** none. "Log maintenance" ink is 105 px at 390, against 106 at P: whole, so S10(b) holds. |
| 1215/1000 | **S1, cards 12 px apart: closed.** Measured: the columns are 867→880 at 1215 (724→737 at 1000), and Details→Recent events is 316→329. The design has 14, within one step. |
| 390 | **Q1, Book/Log maintenance: closed.** They are 16–168 and 173–325, 152 px each, where P had 132/174. The design has 148/148. |
| 390 | **S7/P6, stacked captions: closed.** "Location" is 43 px wide (12 px) at both P and Q. Its value grew from 96 to 104 px and "Methods"' value from 172 to 187 (13 px), so the caption is now visibly smaller than the value, as in the design. |
| 390 | **L, screen: Book and Log maintenance are 4 px apart** (169–172 blank), against the design's 8. `32adacbcb` set `gap = 2` on results, review and plate only. **Fix:** `.gap = 2` on the compact actions row (`instrument.cpp:388`). |
| 1000 | No clipping. Event 1 wraps at a word ("Run / held."). "next 14-Jan-2027" is whole. |
| — | Accepted: 27 px pitch. |

## Audit

| Width | Finding |
|---|---|
| 1000 | **Regressions:** none. The `3fb45e873` fix holds: "20-Oct-2026 00:00" and "24-Oct-2026 23:59" are whole beside the calendar glyph (checked in a zoomed crop). "OOS-2026-031" is whole. Reason wraps only at words. |
| 1215/1920 | **Regression, L, screen: the rail's side inset is 8 px everywhere**, where it was 12 at P and is 14 in the design (`padding(3, 2)`). The change was made for the 785 px page, but it applies at the judged width too. The table did not move. **Fix:** this needs an inset that follows the width, which the vocabulary does not have per breakpoint (inferred). The alternative is to accept it. It is noted rather than counted high. |
| 1920 | The rail is capped (216–438) and the table fills the rest. As intended, and unchanged from P. |
| 390 | **Q1, Filters · 4 and Export CSV: closed.** They are 16–192 and 197–373, 177 px each, where P had 179/175. |
| 390 | **L, screen: the pair is 4 px apart**, against the design's 8. **Fix:** `.gap = 2` (`audit.cpp:452`). |
| — | Accepted: the rail at 210, the data, and A11 (Load more is grey). |

## Samples

| Width | Finding |
|---|---|
| 1215 | **Regressions:** none. Only the head controls and the bulk bar changed (13 px). The table is unchanged. |
| 1000 | No clipping. Product, lot and test cells wrap at spaces ("Paracetamol 500 mg / · L-26-0377"). Clear and Register samples are whole. |
| 390 | **Q1, Filters · 2 and Sort: closed.** They are 16–192 and 197–373, 177/177, where P had 158/196. The design has 173/175. "Sort: due, earliest first" is whole. |
| 390 | **L, screen: the pair is 4 px apart**, against the design's 8–9. **Fix:** `.gap = 2` (`samples.cpp:695`). |
| — | Accepted: the bulk bar, the 30 px rows, and the data. |

## Summary

**Regressions: 3.**
- **M, receipt 1000:** the condition label is elided. This contradicts `dcefd3d15`'s message.
- **L, receipt 1215:** the Time column is 54 px, against 62 at P.
- **L, audit 1215/1920:** the rail inset is 8 px, against 12 at P.

All three are side effects of fitting 13 px text on the 785 px page.

**Batch Q items on these screens:**
- **Closed, 8:**
  - Q2, 13 px body, on all five screens;
  - Q1 equal stretch pairs: samples 390, audit 390 and instrument 390;
  - Q4/S6, receipt's legend at 12 px;
  - S1, instrument's cards 12 px apart;
  - S7, stacked captions visibly 12 px against 13 px values;
  - S10(b), button captions whole.
- **Open, 1:** "phone button pairs 8" was applied only to results, review and plate. Instrument, audit and samples at 390 are still 4 px apart.
- **Known residual:** receipt's "Release, finished produc" at 1000 is a silent clip. Yes, a framework report should catch it: see below.

**Remaining differences: 0 H, 2 M, 5 L.**
- **Screen, 1 M and 5 L:**
  - M: receipt 1000, the checkbox label is elided;
  - L: receipt's Time column;
  - L: audit's inset;
  - L: the 4 px pairs on instrument, audit and samples at 390. Fix: `gap = 2`.
- **Framework, 1 M:** a read-only `textInput` clips its value silently. It shows no ellipsis and no overflow report, while a checkbox label elides.

**Possible library finding (for the dispatcher):** the 8 px compact button pair is now set by hand in six places:
results, review and plate, plus instrument, audit and samples, which still need it. By AGENTS.md, the same example fix
in a second place points at a missing seam. A row of push buttons on a compact page could default to the design's 8 px
gap, or there could be an `actions` row kind. This is inferred from the code pattern. I have not checked whether
`ui::row` has a button-row default today.

**1000-px check:** there are no mid-word breaks and no overlap on any of the five screens. The only lost content is on
receipt: the elided condition label (new) and the clipped sample type (known).

**1920:** the receipt session pane and the audit rail stop at the mockup width. The rest of each page fills as
intended.

**Not verified:**
- I built and ran nothing.
- The kv row pitch is attributed to the font's line height by inference only.
- I did not read `MorphTextInput.qml` or `MorphCheckBox.qml`.

**What would change the verdicts:** a re-grab where receipt 1000 shows the label whole would retract the regression. A
read-only input that already reports overflow in some other channel would retract the framework item.
