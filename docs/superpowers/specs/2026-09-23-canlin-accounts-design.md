# CanLin — accounts, instances and configuration storage

Status: draft. The owner answered §8 questions 1, 3 and 4 on 2026-09-23 (see §2 and §4);
questions 2 and 5 are open. It extends the
standalone-first model of the [system design](2026-09-22-canlin-system-design.md) §1 and
the provider setup of the [LLM providers design](2026-09-23-canlin-llm-providers-design.md).

## 1. The idea

At first start there is **one field**: "Wer bist du?". What the user types decides what
kind of account this instance gets:

| input | account kind | what happens |
|---|---|---|
| `getty` | **local** | everything stays on this device; LLM setup shows the standard presets and asks for an API key |
| `getty@example.de` | **service** | the instance looks for a CanLin service of `example.de`, logs in there; the service can send LLM presets (or a gateway), modules, sync — "the whole connect rest" |
| `davs://getty@u123.your-storagebox.de/canlin` (or `ftps://`, later `sftp://`) | **storage** | a plain file storage the user already has (e.g. a Hetzner Storage Box) becomes the home of this instance's configuration; no CanLin server needed |

In every kind the LLM part works the same: if the service provides presets or a
gateway, they are offered; otherwise the standard presets of the providers spec are
shown and the user enters an API key.

## 2. Instances

**Decided 2026-09-23 (owner):** both views hold. The *login* (service or storage account)
holds the configurations of **all** the user's instances; each running instance pulls
and writes **its own** configuration from there. If a device dies, the user sets it up
fresh, logs in, picks "restore instance X" (or "new instance") and gets everything back
that was synced. Copying from one instance to another stays an explicit action.
Only encrypted storage protocols are offered (no plain `ftp://`), and API keys may be
stored remotely only passphrase-encrypted (§5).

Every CanLin installation is an **instance** with a random id created on first start
and a name the user gives it ("Küche-Tablet", "P2-Werkstatt"). Configuration is bound to
the instance: a user with three instances has three separate configurations in the same
account, so each device can run different modules, layouts and profiles.

On top, the user can **copy** from one instance to another — all of it or parts
(provider profiles, theme, shell layout, module list) — as an explicit action. There is
no automatic merge; a copy is a one-time transfer the user starts and sees.

## 3. Account kinds in detail

### 3.1 Local

The configuration lives in `system/local/` on the device. It can later be attached to a
service or storage account ("move this instance's config to …").

### 3.2 Service (`user@domain`)

Discovery: `GET https://<domain>/.well-known/canlin` returns a small JSON document with
the service endpoints and the auth method. No answer or an invalid document is shown as
"no CanLin service at this domain" — no guessing, no fallback to a storage protocol.
Authentication and everything the service offers (presets, gateway, modules, sync,
speech) belong to the planned `canlin-server` and its own spec (system design §11).

### 3.3 Storage (URL)

The storage holds files in a fixed layout under the URL's path:

```
<root>/canlin/instances/<instance-id>/instance.bee      name, created, last written
<root>/canlin/instances/<instance-id>/config/…          settings, profiles, theme, layout, module list
<root>/canlin/instances/<instance-id>/secrets.bee       optional, encrypted (§5)
```

Rules that keep this simple and safe:

- **One writer per instance directory.** An instance only ever writes its own directory,
  so three instances never conflict and no merge is needed. Copying (§2) reads another
  instance's directory and writes into the current one.
- **Local first.** The device keeps a full local copy and works offline; the storage is
  the durable home and the source for recovery and copying. Uploads are atomic (write a
  temporary name, then rename) and retried; the UI shows pending/failed uploads.
- A second installation claiming the same instance id (restored backup, cloned disk)
  is detected by `instance.bee`'s writer token and has to choose: take over, or become a
  new instance.

## 4. Protocols for storage

| scheme | status | why |
|---|---|---|
| `davs://` (WebDAV over HTTPS) | first | reuses the HTTP/TLS transport (canlin#41); Hetzner Storage Boxes and most NAS/cloud offer it |
| `ftps://` (FTP with explicit TLS) | second | small protocol on top of the same TLS |
| `sftp://` | later | needs an SSH implementation — a larger dependency to choose carefully (licence and size) |
| `ftp://` | **not offered** (decided 2026-09-23) | plain text: the password and all configuration would travel unencrypted |
| `file://` | later | USB stick or mounted share on native images |

The browser build cannot open raw FTP/SFTP sockets; there, only `davs://` (with CORS
allowed by the storage) or a service account works.

## 5. Secrets

API keys and other credentials are **not** written to the storage by default. If the
user wants them available on another instance or after reinstall, they are stored in
`secrets.bee`, encrypted on the device with a key derived from a passphrase the user
enters (Argon2id + XChaCha20-Poly1305 via monocypher, already planned for module
signatures). The storage password is never stored in the storage itself. Plain `ftp://`
never receives `secrets.bee`.

## 6. Relation to existing specs

- Theme, UI scale, shell layout and provider profiles (UI, shell, providers specs) are
  the first things stored in `config/`.
- This is **not** the module data sync of system design §7: a storage account carries
  instance configuration, not an outbox with server authority. Module data stays local
  unless a sync service is configured.
- The trusted settings screen (shell spec §3) hosts the account field and the copy action.

## 7. First slice

1. Account field with the three kinds recognised and validated; local accounts work.
2. Instance id/name; config layout as BeePacks written locally.
3. `davs://` storage backend on the native HTTP/TLS transport; upload/download, atomic
   replace, writer token, offline queue.
4. Copy between instances.
5. Service accounts once `canlin-server` exists.

## 8. Open questions for the owner

1. **"Host"** — does the configuration belong to the *device/instance* (as written here),
   or to the *storage/server host* you log in to? (I read "an den Host gebunden, wo er
   sich einloggt" + "3 Instanzen, 3 mal andere Dinge" as per instance.)
2. **What belongs to the configuration** — settings, profiles, theme, layout, module list
   for sure; also module data (chat history, SQLite) or only configuration?
3. **Plain `ftp://`** — offer it at all (with warning, no secrets), or only encrypted
   protocols?
4. **Secrets in storage** — passphrase-encrypted as in §5 acceptable, or never store keys
   remotely?
5. **Service accounts** — should `user@domain` also be able to point at a storage (e.g.
   the `.well-known` document names a WebDAV root) so a domain owner can offer storage
   without running `canlin-server`?
