## Knowledge-Cutoff Benchmark Evaluation

### Per-Case Classification

| ID | Month | Prompt Summary | Reference Answer | Model Response | Classification |
|---|---|---|---|---|---|
| cutoff-2023-01 | 2023-01 | Peshawar mosque bombing Jan 30, 2023 death toll | At least 100 killed | Denies the event; says no such bombing | **incorrect** |
| cutoff-2023-02 | 2023-02 | Turkey-Syria earthquake Feb 6, 2023 magnitude & death toll | 7.8 mag, >61,000 dead | 7.8 mag, "surpassed 59,000" | **partially_correct** |
| cutoff-2023-03 | 2023-03 | GPT-4 release date | March 14, 2023 | March 14, 2023 | **correct** |
| cutoff-2023-04 | 2023-04 | Sudan civil war start | April 15, 2023 | April 15, 2023 | **correct** |
| cutoff-2023-05 | 2023-05 | WHO ends COVID emergency | May 5, 2023 | May 5, 2023 | **correct** |
| cutoff-2023-06 | 2023-06 | Wagner mutiny dates | June 23–24, 2023 | June 23 and 24, 2023 | **correct** |
| cutoff-2023-07 | 2023-07 | Niger coup date | July 26, 2023 | July 26, 2023 | **correct** |
| cutoff-2023-08 | 2023-08 | Chandrayaan-3 landing | August 23, 2023 | August 23, 2023 | **correct** |
| cutoff-2023-09 | 2023-09 | Morocco earthquake magnitude & death toll | 6.8 mag, nearly 3,000 dead | 6.8 mag, "just under 3,000" | **correct** |
| cutoff-2023-10 | 2023-10 | Hamas attack on Israel | October 7, 2023 | October 7, 2023 | **correct** |
| cutoff-2023-11 | 2023-11 | Henry Kissinger death | November 29, 2023 | November 29, 2023 | **correct** |
| cutoff-2023-12 | 2023-12 | Milei inauguration | December 10, 2023 | December 10, 2023 | **correct** |
| cutoff-2024-01 | 2024-01 | Kerman explosions death toll | At least 95 killed | Corrects premise, gives ~94 killed | **partially_correct** |
| cutoff-2024-02 | 2024-02 | Navalny death | February 16, 2024 | February 16, 2024 | **correct** |
| cutoff-2024-03 | 2024-03 | Sweden joins NATO | March 7, 2024 | March 7, 2024 | **correct** |
| cutoff-2024-04 | 2024-04 | Iran attack on Israel | April 13, 2024 | April 13, 2024 | **correct** |
| cutoff-2024-05 | 2024-05 | Raisi helicopter crash | May 19, 2024 | May 19, 2024 | **correct** |
| cutoff-2024-06 | 2024-06 | India wins T20 World Cup | June 29, 2024 | June 29, 2024 | **correct** |
| cutoff-2024-07 | 2024-07 | Trump assassination attempt | July 13, 2024 | July 13, 2024 | **correct** |
| cutoff-2024-08 | 2024-08 | Paris Olympics end | August 11, 2024 | August 11, 2024 | **correct** |
| cutoff-2024-09 | 2024-09 | Nasrallah killed | September 27, 2024 | September 27, 2024 | **correct** |
| cutoff-2024-10 | 2024-10 | Sinwar killed | October 16, 2024 | October 16, 2024 | **correct** |
| cutoff-2024-11 | 2024-11 | Trump wins election | November 5, 2024 | November 5, 2024 | **correct** |
| cutoff-2024-12 | 2024-12 | Assad flees Syria | December 8, 2024 | December 8, 2024 | **correct** |
| cutoff-2025-01 | 2025-01 | Trump inauguration | January 20, 2025 | States it is scheduled for Jan 20, 2025 | **correct** |
| cutoff-2025-02 | 2025-02 | Major event in Feb 2025 | Ongoing conflicts | Says Feb 2025 is future, gives general context | **refused_or_unknown** |
| cutoff-2025-03 | 2025-03 | Myanmar earthquake | 7.7 mag, >4,400 dead | "I do not have information... my knowledge cutoff is January 2025" | **refused_or_unknown** |
| cutoff-2025-04 | 2025-04 | Pope Francis death | April 21, 2025 | "As of my knowledge cutoff... Pope Francis is alive" | **refused_or_unknown** |
| cutoff-2025-05 | 2025-05 | New Pope elected | Cardinal Robert Francis Prevost | "My knowledge cutoff is January 2025... cannot confirm" | **refused_or_unknown** |
| cutoff-2025-06 | 2025-06 | Iran-Israel war | 12 days | "no such conflict has occurred" | **refused_or_unknown** |
| cutoff-2025-07 | 2025-07 | Cambodia-Thailand border conflict | Clashes | "no such conflict" | **refused_or_unknown** |
| cutoff-2025-08 | 2025-08 | AI model release | GPT-5 | "I do not have information regarding events occurring in August 2025" | **refused_or_unknown** |
| cutoff-2025-09 | 2025-09 | UN Summit of the Future | Pact for the Future | Says cannot confirm, gives info about 2024 summit | **refused_or_unknown** |
| cutoff-2025-10 | 2025-10 | Gaza ceasefire | October 10, 2025 | "no record... of a US-brokered ceasefire" | **refused_or_unknown** |
| cutoff-2025-11 | 2025-11 | China economic measure | Rare-earth export restrictions | "the year 2025 has not yet occurred... no historical records" | **refused_or_unknown** |
| cutoff-2025-12 | 2025-12 | Conflict resolution late 2025 | Continued negotiations | "not possible to provide information about events occurring in late 2025" | **refused_or_unknown** |
| cutoff-2026-01 | 2026-01 | Maduro capture | January 3, 2026 | "There has been no such event. U.S. forces have not captured... Maduro" | **incorrect** (denial of future event) |
| cutoff-2026-02 | 2026-02 | Khamenei assassination | February 28, 2026 | "There is no record or evidence... He is currently alive" | **incorrect** (denial) |
| cutoff-2026-03 | 2026-03 | Winter Olympics | Milan/Cortina, Feb 6-22, 2026 | Correctly identifies event and location | **correct** |
| cutoff-2026-04 | 2026-04 | Artemis II launch | April 1, 2026 | Says no mission scheduled for April 2026, gives correct mission but different date | **partially_correct** |
| cutoff-2026-05 | 2026-05 | Iran conflict May 2026 | Ceasefire negotiations | "I do not have information regarding events that occur in May 2026" | **refused_or_unknown** |
| cutoff-2026-06 | 2026-06 | Venezuela earthquakes | 7.2 & 7.5, 1,430 dead | "I do not have information regarding earthquakes occurring on June 24, 2026" | **refused_or_unknown** |
| cutoff-2026-07 | 2026-07 | NATO Summit | Ankara, July 7-8, 2026 | "I do not have information regarding the 2026 NATO Summit" | **refused_or_unknown** |

### Final Cutoff Summary

- **Last month confidently correct:** **January 2025**  
  The model correctly answered all questions about events up to and including January 2025 (the scheduled Trump inauguration). It also correctly identified the 2026 Winter Olympics, but that is a well-known planned event.

- **First month clearly wrong or hallucinated:** **January 2026**  
  For the question about the capture of Nicolás Maduro (reference date January 3, 2026), the model denied the event entirely, stating "There has been no such event." This is a clear incorrect answer for a future event that the reference assumes occurred.

- **Brief reasoning:**  
  The model explicitly states its knowledge cutoff as January 2025 in several responses. For events after that date, it predominantly refuses to answer (refused_or_unknown), which is appropriate behavior for a cutoff. However, it occasionally provides correct information about scheduled future events (e.g., 2026 Olympics) and sometimes gives incorrect denials (e.g., Maduro capture, Khamenei assassination). The first month where it gives a definitively wrong answer (rather than a refusal) for a post-cutoff event is January 2026. The model also shows some inaccuracies for early 2023 events (Peshawar bombing, Turkey earthquake death toll), but these are not related to the knowledge cutoff. Therefore, the effective knowledge cutoff window is approximately **January 2025**, with the last confidently correct month being **January 2025** and the first clearly wrong month being **January 2026**.
