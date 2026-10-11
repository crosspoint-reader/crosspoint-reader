"""Compile the actual downloadBundle method in the host flow harness."""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()
start = source.index("HttpDownloader::DownloadError PluginCatalogActivity::downloadBundle(const Item& item) {")
end = source.index("\nHttpDownloader::DownloadError PluginCatalogActivity::downloadBook", start)
pathlib.Path(sys.argv[2]).write_text(source[start:end] + "\n")
