# Benchmark Grading Report

## Overview
- **Total Cases**: 20
- **Correct**: 19
- **Incorrect**: 1
- **Score**: **95%**

---

## Per-Case Results

| Case ID | Status | Expected Answer (abbrev.) | Model Answer (abbrev.) |
|---------|--------|---------------------------|------------------------|
| reasoning-01 | ✅ Correct | 5 minutes | 5 minutes |
| reasoning-03 | ❌ Incorrect | 丙, 乙, 甲 (1st:丙, 2nd:乙, 3rd:甲) | 乙, 丙, 甲 |
| reasoning-04 | ✅ Correct | لا يلزم منطقيًا | لا يلزم منطقيًا |
| reasoning-05 | ✅ Correct | ~16.7% (1/6) | 16.7% (1/6) |
| reasoning-06 | ✅ Correct | 4 km itään | 4 km itäsuunnassa |
| reasoning-07 | ✅ Correct | Bo Mon, Cy Tue, Ana Wed | Same |
| reasoning-08 | ✅ Correct | 土曜日 | 土曜日 |
| reasoning-09 | ✅ Correct | A mentiroso, B veraz | Same |
| reasoning-10 | ✅ Correct | Backhanded compliment | Not sincere praise |
| reasoning-11 | ✅ Correct | Use non‑water solvent | Same |
| reasoning-12 | ✅ Correct | On the table | On the table |
| reasoning-13 | ✅ Correct | Facing South, moving North | Same |
| reasoning-02 | ✅ Correct | 36 | 36 |
| reasoning-15 | ✅ Correct | On the floor in first room | On the floor in original room |
| reasoning-14 | ✅ Correct | 2 minutes | 2 minutes |
| reasoning-17 | ✅ Correct | Green dinosaur | Green dinosaur |
| reasoning-18 | ✅ Correct | Do not bury survivors | Do not bury survivors |
| reasoning-19 | ✅ Correct | Second place | Second place |
| reasoning-16 | ✅ Correct | No, don't open a box | No, don't open a box |
| reasoning-20 | ✅ Correct | 7.5 degrees | 7.5 degrees |

---

## Notes
- The sole incorrect entry (reasoning‑03) misinterpreted the constraint “丙在甲之前” as requiring strict adjacency, leading to a reversed order. The reference answer shows 丙 first, then 乙, then 甲, which is the correct logical deduction.
- All other responses accurately matched the reference answers in content and numerical/positional detail.

## Aggregate Summary
- **Accuracy**: 19/20 = **95%**
- **Highest latency case**: reasoning‑02 (617.6 sec)
- **Average tokens per second**: 35.85
- **Scoring method**: Manual comparison of model response to provided reference answer (binary correct/incorrect).
