# webpack: a small offline collection from web pages

Makes a ZIM file (the format of the Kiwix collections) from chosen pages of a
website, for your own card. Plain Python 3.11+, nothing to install.

    python3 webpack.py recipes/pets.toml "/Volumes/SSK Drive"

writes `pets_en_msd_YYYY-MM.zim` into the staging folder; `cardbuilder.py`
picks it up through the `file =` entry in `library.toml`, indexes it and
copies it like any other collection.

- `zimwrite.py`: the ZIM writer (version 6.1, one uncompressed cluster, the
  header's title list), written from the openZIM format description.
- `recipes/*.toml`: what to fetch: start pages, the links to follow, extra
  pages, and a suffix to strip from titles.

Each page keeps its article (headings, paragraphs, lists, tables, links to
other packed pages); menus, scripts and pictures are dropped. Pages are
stored under their address without `https://www.`. Copies are for personal
use; copyright stays with the site.
