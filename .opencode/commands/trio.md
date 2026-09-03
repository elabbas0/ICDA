---
description: Run trio debate (2x Muse Spark 1.3 advocate/critic + MiMo coder) then implement.
agent: trio
---

Run the trio workflow on: $ARGUMENTS

1. In PARALLEL via Task tool, spawn:
   - coder: "Gather facts for: $ARGUMENTS. Return file:line evidence, max 300 words."
   - advocate: "Propose ONE concrete solution for: $ARGUMENTS. Max 300 words."
2. Spawn:
   - critic: "Review the advocate proposal against coder facts. Return blockers/concerns/verdict (approve / approve-with-fixes / reject)."
   - coder follow-up only if facts disputed.
3. If reject, one advocate revision + one critic re-check. Max 3 rounds. You break ties.
4. Delegate implementation to coder with the exact winning plan ("Implement this approved plan: <plan>. Verify with build/tests."), review the diff, verify, report Decision + Changes (file:line) + Verification.

If $ARGUMENTS contains "discuss only" or "plan only", stop before step 4.
