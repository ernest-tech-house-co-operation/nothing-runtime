#!/usr/bin/env python3
# examples/chain_with_python.py
# Second step of the cross-language chain. nth forks python3 to run
# this file. stdout/stderr are streamed through to the user.
import sys

print("[py step 2] received data from JS step 1", file=sys.stderr)
# In v0.1, each chain step is an independent process — state does NOT
# carry over between steps. For real cross-step data sharing, use a
# file or env var. Here we just demonstrate that the chain executes.
data = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
total = sum(x * x for x in data)
print(f"[py step 2] sum of squares: {total}")
print("[py step 2] handing back to JS")
