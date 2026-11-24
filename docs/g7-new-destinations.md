# G7: a destination the restore asked for and the policy refused

The G7 restore starts with five approved hosts (host/install_g7.c: gs.apple.com, gs.apple.com.akadns.net, gs.apple.com.v.aaplimg.com, wkms-public.apple.com, fcs-keys-pub-prod.cdn-apple.com, all port 443). Anything else is refused and
printed after the run as `name/address:port reason` (host name and port only; no URL, path, query or ECID). Nothing is added from a guess.

Before a host goes on the list, write down (in the commit message or the review that approves it):

1. Source: where it was seen (the refused-destination line of which run) and what in the code asked for it (URLAsset, image key, FDR, other).
2. Purpose: what the request is for and whether the restore can finish without it.
3. Exact host and port (no wildcard, no suffix); https only for URLs.
4. Approval: who approved, and for which run. A new host is a separate approval; it never rides along with another change.

Then add it to the table in host/install_g7.c, extend tests/test_g7_net.c (allowed, wrong port, look-alike) and run tools/g7-net-test.sh.

## Approved so far (beyond the three TSS hosts)

- `wkms-public.apple.com:443` (2026-11-24). Source: `g7-net-deny: purpose=URL host=wkms-public.apple.com port=443` in run log g7_run_1795484190481, at mount_filesystem, followed by `restore_send_url_asset: URLAsset request refused by policy`. Purpose: the URLAsset request the device made while restoring the system image; whether the restore finishes with it is not yet known. The list is shared by TSS and URLAsset, so this host is also reachable for the HTTP purpose. The key host `wkms.sd.apple.com` was NOT approved (no refusal line for it has been seen). Approved by the user for the next run.
- `fcs-keys-pub-prod.cdn-apple.com:443` (2026-11-24). Source: `g7-net-deny: purpose=URL host=fcs-keys-pub-prod.cdn-apple.com port=443 reason=host not on the allow list count=1` in run log g7_run_1795490597501, followed by `restore_send_url_asset: redirect target refused by policy`: it is the redirect target of the request to wkms-public.apple.com. Purpose: the URLAsset download of the restore; whether the restore finishes with it is not yet known. Shared list, so it is also reachable for the HTTP purpose. Approved by the user for the next run.

## Temporary observation mode (approved by the user on 2026-11-24)

Adding one exact host per round (push, CI, APK, install, restore) is too slow while the restore still stalls. Until the real list is known, the runner asks for observation mode
(`hv_net_guard_observe_request(1)`, host/g7_runner_real.c). The G7 allow-list (host/install_g7.c, `k_observe`) then also registers four suffixes:
`apple.com`, `cdn-apple.com`, `aaplimg.com`, `apple.com.akadns.net`.

- A NAMED sub-host of one of them (label boundary; never the suffix itself, `evilapple.com` or `apple.com.evil.com`) is allowed on **443 only**, for the HTTP and URL purposes only.
- Not relaxed: private/loopback/link-local addresses, nameless addresses, FDR, the library's own link, other ports, http, user info. `akadns.net` as a whole is NOT covered (other customers use it).
- Every observed host is written as `g7-net-observe: purpose=<HTTP|URL> host=<name> port=443 count=<n>` (1st, 10th, 100th ...). Names and ports only.
- Exit: after a run, take the observed hosts that the restore really needed, add them as exact entries (this document, host/install_g7.c, tests, tools/g7-net-test.sh), then remove the request in the runner and `k_observe`.
