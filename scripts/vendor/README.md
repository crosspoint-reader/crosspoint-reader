# Vendored build tools

These files are copied unchanged from their upstream releases so that
`scripts/build_html.py` can minify the web UI without installing packages.

| File | Upstream | Version | Licence |
| --- | --- | --- | --- |
| `rjsmin.py` | https://github.com/ndparker/rjsmin | 1.3.0 (PyPI) | Apache-2.0, see `LICENSE-Apache-2.0` |
| `rcssmin.py` | https://github.com/ndparker/rcssmin | 1.3.0 (PyPI) | Apache-2.0, see `LICENSE-Apache-2.0` |

SHA-256 of the vendored files:

```
fac51dffdb149d4097f30a1c756bd2e31902af77d80d15f1f77d874be93462fa  rjsmin.py
ed7ded47b3e6b1729c31d8a8ea1997f963caab614adcc2d12e2cad5165eeb936  rcssmin.py
```

To update, replace the `.py` file with the one from the new PyPI source
release and update this table and the checksums.
