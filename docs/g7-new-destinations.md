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
