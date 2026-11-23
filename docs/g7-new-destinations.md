# G7: a destination the restore asked for and the policy refused

The G7 restore starts with three approved hosts (host/install_g7.c: gs.apple.com, gs.apple.com.akadns.net, gs.apple.com.v.aaplimg.com, port 443). Anything else is refused and
printed after the run as `name/address:port reason` (host name and port only; no URL, path, query or ECID). Nothing is added from a guess.

Before a host goes on the list, write down (in the commit message or the review that approves it):

1. Source: where it was seen (the refused-destination line of which run) and what in the code asked for it (URLAsset, image key, FDR, other).
2. Purpose: what the request is for and whether the restore can finish without it.
3. Exact host and port (no wildcard, no suffix); https only for URLs.
4. Approval: who approved, and for which run. A new host is a separate approval; it never rides along with another change.

Then add it to the table in host/install_g7.c, extend tests/test_g7_net.c (allowed, wrong port, look-alike) and run tools/g7-net-test.sh.
