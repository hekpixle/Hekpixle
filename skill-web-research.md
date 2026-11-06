---
name: web-research
description: Method for researching a question on the web with web_search, web_extract and the browser, and writing a sourced answer.
---

# Web research

1. Rewrite the question into 2–4 short, different search queries (`web_search`).
2. Open the 3–5 most promising results with `web_extract` (batch up to 5 URLs). Prefer primary sources: official docs, papers, government/company pages.
3. If a page returns little text or needs login/JS, use `browser_navigate` + `browser_snapshot`.
4. Cross-check important numbers/claims in at least two independent sources; note disagreements.
5. For big comparisons use `delegate_task` to research items in parallel, then merge the summaries.
6. Answer with: a direct conclusion first, key facts, then a short source list (title + URL). Say clearly what you could not verify.
7. If the user may need it again, save the finished report with `write_file` and mention the path.
