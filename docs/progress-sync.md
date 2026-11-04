# Reading Progress Sync

[Back to the User Guide](../USER_GUIDE.md#transfer-and-sync)

CrossPoint can sync reading progress with KOReader-compatible sync servers.
It also interoperates with KOReader apps/devices when they use the same server and credentials.

Progress is stored under a document hash, an identifier derived from the book file. **Document Matching** selects the primary hash:

- **Filename** hashes the filename, including its extension, without the folder path. Renaming the file changes this hash.
- **Binary** hashes samples of the file's contents, independently of its filename. For reliable matching, use identical file content on every device. Conversion or optimization can change this hash.

The default **Smart Sync** behavior checks both the primary and alternate hashes. If the primary has no progress, it can use progress stored under the alternate hash. This lets identical content with different filenames match when progress exists under the binary hash. When both hashes have progress, Smart Sync compares the records. **Ask every time** checks only the primary hash.

**Server Type** controls which progress and metadata format CrossPoint sends:

- **CrossPoint** - For the default service or a self-hosted CrossPoint-compatible sync server. Sends standard KOReader progress plus CrossPoint's richer reading position when available.
- **KoSync** - For standard KOReader Sync servers such as `sync.koreader.rocks` or `koreader/kosync`. Uses the strict standard payload without CrossPoint-specific rich position data.
- **Other** - For custom servers that explicitly support CrossPoint's enhanced payload. Rich position is enabled, and when **Send Metadata** is on CrossPoint may also send recognized ISBN, ASIN, series name, and series index in addition to title/author metadata.

**Send Metadata** remains the privacy switch for bibliographic data. Turn it off if you want to sync progress without sending title, author, ISBN/ASIN, or series information.

## Option A: CrossPoint Sync Server (`sync.crosspointreader.com`, default)

When **Sync Server URL** is left empty, CrossPoint uses the free CrossPoint sync server at `https://sync.crosspointreader.com`. It speaks the standard KOReader sync protocol (so KOReader apps can use it too). CrossPoint records page starts as chapter-content offsets and sends the corresponding standard KOReader XPath, so devices with different fonts or layouts can return to the same text.

1. On each CrossPoint device:

   - Go to **Settings -> Connectivity -> CrossPoint Sync**.

   - Set **Username** and **Password** (enter the plain password; CrossPoint computes MD5 internally, and use the same values on all devices).

   - Leave **Sync Server URL** empty (or set it to `https://sync.crosspointreader.com`).

   - Set **Server Type** to **CrossPoint**.

   - On the first device, run **Sign Up** once to create the account directly from the device. On every other device, just run **Authenticate**.

Accounts are per server. Existing `sync.koreader.rocks` credentials do not exist on the CrossPoint server; either sign up again with the same username/password or use Option B to keep using the legacy server.

## Option B: Legacy Public KOReader Server (`sync.koreader.rocks`)

Use this if you already sync KOReader devices against the official public server.

1. On each CrossPoint device:

   - Go to **Settings -> Connectivity -> CrossPoint Sync**.

   - Set **Sync Server URL** to `https://sync.koreader.rocks` (required; an empty URL now points at the CrossPoint server instead).

   - Set **Server Type** to **KoSync**.

   - Set **Username** and **Password** to your existing KOReader Sync credentials.

   - Run **Authenticate**.

2. If you do not have an account yet, run **Sign Up** on the device, or register once with curl. The curl examples require Python 3 and OpenSSL:

```bash
USERNAME="user"
PASSWORD="$(python3 -c 'import getpass; print(getpass.getpass("Sync password: "))')"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"
REQUEST_BODY="$(python3 -c 'import json, sys; print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))' "$USERNAME" "$PASSWORD_MD5")"

curl -i "https://sync.koreader.rocks/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "$REQUEST_BODY"
```

When this returns `HTTP 402` with `{"code":2002,"message":"Username is already registered."}`, pick a different username or use that existing account.

## Option C: Self-Hosted Server (Docker Compose)

1. Start a sync server:

```bash
mkdir -p kosync-quickstart
cd kosync-quickstart

cat > compose.yaml <<'YAML'
services:
  kosync:
    image: koreader/kosync:latest
    ports:
      - "7200:7200"
      - "17200:17200"
    volumes:
      - ./data/redis:/var/lib/redis
    environment:
      - ENABLE_USER_REGISTRATION=true
    restart: unless-stopped
YAML

# Docker
docker compose up -d

# Podman (alternative)
podman compose up -d
```

> [!NOTE]
> After creating your users, set `ENABLE_USER_REGISTRATION=false` explicitly in `compose.yaml`.
> Then rerun `docker compose up -d` or `podman compose up -d` to recreate the container with registration disabled.
> Do not remove the variable: the [image defaults it to `true`](https://github.com/koreader/koreader-sync-server/blob/master/Dockerfile).

2. Verify the server:

```bash
curl -H "Accept: application/vnd.koreader.v1+json" "http://<server-ip>:17200/healthcheck"
# Expected: {"state":"OK"}
```

3. Register a user once.
   CrossPoint authenticates against KOReader Sync (`koreader/kosync`) using an MD5 key, so register using the MD5 of your password:

> [!WARNING]
> Sending a reusable MD5-derived password over plain HTTP is insecure.
> Create unique sync-only credentials and do not reuse main account passwords.
> Prefer `https://<server-ip>:7200` whenever traffic leaves a fully trusted LAN or when using untrusted networks.
> Use `curl -k` only for self-signed certificate testing.

```bash
USERNAME="user"
PASSWORD="$(python3 -c 'import getpass; print(getpass.getpass("Sync password: "))')"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"
REQUEST_BODY="$(python3 -c 'import json, sys; print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))' "$USERNAME" "$PASSWORD_MD5")"

curl -i "http://<server-ip>:17200/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "$REQUEST_BODY"
```

If this returns `HTTP 402` with `{"code":2002,"message":"Username is already registered."}`, the account already exists.

4. On each CrossPoint device:

   - Go to **Settings -> Connectivity -> CrossPoint Sync**.

   - Set **Username** and **Password** (enter the plain password; CrossPoint computes MD5 internally, and use the same values on all devices).

   - Set **Sync Server URL** to `http://<server-ip>:17200`.

   - Set **Server Type** to **KoSync**.

   - Run **Authenticate**.

If you use the HTTPS listener, use `https://<server-ip>:7200` (`curl -k` only for self-signed certificate testing).

## Syncing While Reading

Once any of the options above is set up, press **Confirm** while reading to open the reader menu, then select **Sync Progress**. Alternatively, set **Settings -> Controls -> Long-Press Menu** to **CrossPoint Sync** and hold Confirm to launch sync directly.

- **Sync Clippings** is off by default. Enable it only for a compatible CrossPoint Sync server; manual **Sync Progress** then uploads and downloads this book’s clippings too. Leave it off for ordinary KOSync servers.
- With **Sync Behavior** set to **Ask every time**, choose **Apply Remote** to jump to remote progress or **Upload Local** to push current progress.
- With **Sync Behavior** set to **Smart sync**, CrossPoint auto-resolves simple cases: upload when no remote progress exists, confirm and leave both unchanged when local and remote progress are already synchronized, upload when local progress is further ahead, or apply remote when remote progress is further ahead.
