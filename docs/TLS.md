# TLS and server certificate verification

ERPL-OData verifies the TLS server certificate of **every** HTTPS request it makes:
OData services, SAP Datasphere, SAP Analytics Cloud, ODP, Microsoft Graph,
Business Central, Dataverse, Delta Sharing, and every OAuth2 token endpoint.

This was not always the case. Until the fix for
[#63](https://github.com/DataZooDE/erpl-odata/issues/63) the HTTP client called
`enable_server_certificate_verification(false)` unconditionally, which meant any
on-path attacker could present an arbitrary certificate and read or modify every
credential and token the extension sent. Verification is now on by default and
can only be turned off deliberately.

## Settings

| Setting | Type | Default | Meaning |
|---|---|---|---|
| `erpl_ca_cert_file` | VARCHAR | `''` | Path to a PEM CA bundle used to verify server certificates. |
| `erpl_unsafe_disable_server_cert_verification` | VARCHAR | `''` | Disables verification entirely. Only accepts the literal string `I_UNDERSTAND_THIS_IS_INSECURE`. |

Both settings are process-wide for the DuckDB instance and take effect on the
next request.

## If you talk to an on-premise system with a private or self-signed certificate

This is the case that the old, insecure default silently papered over. There are
two paths, and the first is strongly preferred.

### 1. Trust the issuing CA (recommended)

Export the CA certificate that signed your SAP / on-premise server certificate
(for a self-signed server certificate, that is the server certificate itself) to
a PEM file and point the extension at it:

```sql
SET erpl_ca_cert_file = '/etc/erpl/my-corporate-ca.pem';

SELECT * FROM odata_read('https://sap.internal.example.com/sap/opu/odata/...');
```

The connection stays authenticated: only certificates chaining to that CA are
accepted, and an attacker with a different certificate is still rejected.

**Important:** when `erpl_ca_cert_file` is set it *replaces* the platform trust
store rather than adding to it. If the same session also talks to public
services (Microsoft Graph, SAP Cloud, `services.odata.org`), concatenate your
private CA onto a copy of the public bundle:

```bash
cat /etc/ssl/certs/ca-certificates.crt my-corporate-ca.pem > /etc/erpl/erpl-ca-bundle.pem
```

### 2. Disable verification (last resort)

If you cannot obtain the CA certificate, verification can be switched off. The
setting is deliberately awkward and refuses every value but one:

```sql
SET erpl_unsafe_disable_server_cert_verification = 'I_UNDERSTAND_THIS_IS_INSECURE';
```

Anything else — `true`, `1`, `yes` — raises an error pointing back at
`erpl_ca_cert_file`. While the opt-out is active, every HTTPS request emits a
`WARN`-level trace line naming the host it is contacting insecurely:

```sql
SET erpl_trace_enabled = TRUE;
SET erpl_trace_level = 'WARN';
```

Setting it back to `''` restores verification immediately.

## Where the trust store comes from

- **Windows** — the system certificate store, loaded by the HTTP client itself.
- **macOS** — the keychain if the client was built with keychain support, else
  `/etc/ssl/cert.pem`.
- **Linux and everything else** — the extension probes `$SSL_CERT_FILE` and then
  the usual distribution locations
  (`/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`,
  `/etc/ssl/ca-bundle.pem`, `/etc/pki/tls/cacert.pem`, `/etc/ssl/cert.pem`) and
  uses the first that exists.

  The probe exists because ERPL-OData statically links a vcpkg-built OpenSSL whose
  compiled-in `OPENSSLDIR` usually does not exist on the machine running the
  extension; without the probe `SSL_CTX_set_default_verify_paths()` yields an
  empty trust store and *every* handshake fails. If your distribution keeps its
  bundle somewhere unusual, set `SSL_CERT_FILE` or `erpl_ca_cert_file`.

---

## Pointing a reader at a different host

Separate from TLS, but the same kind of decision: `erpl_unsafe_allow_custom_service_urls`.

Business Central and Datasphere obtain their OAuth token for a **fixed audience** — Business
Central's is hardcoded to `https://api.businesscentral.dynamics.com`, Datasphere's is a
fixed `default`/`apiaccess` scope. So pointing one of those readers at an https host it was
not built for hands that host a credential minted for a different one. By default this is
refused:

```
Business Central 'environment' points at 'api.example.com'. This service's OAuth token
audience is fixed, so its token would be sent to a host it was not minted for.
```

If you are deliberately proxying the service, opt in:

```sql
SET erpl_unsafe_allow_custom_service_urls = 'I_UNDERSTAND_THIS_SENDS_TOKENS_ELSEWHERE';
```

Like the TLS opt-out, it accepts only that exact literal.

**Loopback addresses never need this setting.** `localhost` and `127.0.0.1` are always
permitted, over http or https, which is what makes the readers testable against a local
server.

**Dataverse is deliberately not gated.** Its scope is `environment_url + "/.default"`, so
the token is minted for whichever host you configure — a customer-specific org URL is the
normal product, not an exposure. Gating it would have required every real deployment to set
an unsafe flag for ordinary use, and a flag needed for normal operation is not a gate.

See GitHub #199.
