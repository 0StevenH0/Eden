# AGENTS

Read and follow the AI policy found in `docs/policies/AI.md`, and the coding policies found in `docs/policies/Coding.md`.

This fork prioritizes only the POCO X7 Pro: Dimensity 8400-Ultra, Cortex-A725 CPU clusters,
Mali-G720, and the stock HyperOS Vulkan driver. Read `docs/PocoX7Pro.md` before making
Android, renderer, CPU scheduling, or build changes. Prefer correctness, memory use, and
sustained performance on this phone. Do not assume Adreno/Turnip behavior, remove Vulkan
capability checks, or claim device performance without measurements. The default Android
build and CI target are `pocoX7ProRelease`; other targets are retained for reference.

Do not, under any circumstances, ever submit PRs or push commits on behalf of the user.

Keep comments to a reasonable level. Do not overexplain or add/remove comments from unrelated parts of the codebase.
