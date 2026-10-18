# OTA security

OTA release checks and firmware downloads require HTTPS on every redirect and
retry. They verify the certificate chain, validity dates, and destination
hostname. A clock sync has up to five seconds to complete before either
operation; an invalid clock or one behind the saved trusted-time floor stops the
operation before a download or OTA partition write. The existing SNTP service
provides time; it is not an authenticated time source.

`src/network/OtaTrustRoots.h` contains the Sectigo E46 and ISRG Root X1 trust
anchors used by the GitHub API, release page, and release asset CDN. Maintainers
must check those endpoints' certificate chains when changing the release host or
when its CA changes, and ship updated roots before the old trust path stops
working. These roots are public certificates, not secrets. The pinned SDK's TLS verification correction is applied by `scripts/patch_secure_net.py` during
firmware builds and host-test configuration; incompatible SDK source stops the
build rather than skipping the correction.
