# Update signing

The committed public bundle contains the maintainer signing key. Its private key
is stored outside the repository and must be added to the signing environment
before releasing. Never use a contributor's example key in production.

Generate a P-256 key on a trusted machine, outside the repository:

```sh
umask 077
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out /secure/path/update-private.pem
openssl pkey -in /secure/path/update-private.pem -pubout -out scripts/update-signing-public.pem
python3 scripts/update-key-header.py scripts/update-signing-public.pem src/UpdateKey.hpp
python3 tools/qa/test_update_manifest.py
```

Commit only the public PEM and generated header. Keep an encrypted backup of the
private key. In GitHub Settings → Environments, create `release-signing`, require
maintainer approval and restrict deployment to release tags. Store the entire
private PEM as its environment secret `UPDATE_SIGNING_KEY`. The release workflow
uses this secret only in the signing job. Never commit it, paste it into a chat,
or put it in build artifacts.

## Planned rotation

1. Append the new public PEM to `scripts/update-signing-public.pem`, keeping the old
   public key, and regenerate the header. Release this bridge version signed with
   the **old** private key. Old clients can verify it; the new build trusts both keys.
2. After clients have installed the bridge, change the signing secret to the new
   private key. Subsequent releases can remove the old public key and regenerate
   the header. Workflow verification accepts any key in the committed bundle.

Clients that skip the bridge still trust only the old key. They must install the
bridge or a newer version manually once signing switches. The updater uses the
latest release, so it does not automatically traverse bridge versions. If that
tradeoff is unacceptable, keep signing with the old key until a separate migration
mechanism is available.

A lost sole private key cannot authenticate a bridge. A compromised key cannot
securely revoke itself for old clients. Both require a trusted manual installation
for affected clients; a key list does not solve those recovery cases.
