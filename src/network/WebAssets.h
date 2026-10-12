#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "html/WebBundle.generated.h"

class WebServer;

// The web interface's pages live on the SD card, not in the firmware image.
//
// scripts/build_html.py compiles every page except the setup page into one
// bundle, web-<hash>.bin: the gzip'd files back to back, in the order of
// web_bundle::ENTRIES. The firmware carries only that table and the bundle's
// SHA-256 (html/WebBundle.generated.h). Installing checks a bundle against the
// hash and unpacks it into /.crosspoint/web; serving streams the files from
// there. About 70 KB of flash this way, against a ~2 KB setup page.
//
// The hash is why the source does not matter: a download from the GitHub
// release, an upload through the setup page and a file copied onto the card are
// accepted on the same terms, because only the exact bytes this build expects
// pass. It is also how a firmware update that changed the pages is noticed: the
// installed stamp no longer matches. One that did not change them keeps them,
// since the pages carry nothing build-specific (WelcomePage asks for the version).
namespace WebAssets {

enum class State : uint8_t {
  Installed,  // the stamp matches this build's bundle and its files are present
  Missing,    // nothing installed, or an installed file is gone
  Stale,      // a bundle from another build is installed
};

enum class InstallError : uint8_t {
  None,
  NoRelease,    // this build is not published, so there is nothing to download
  Download,     // the release asset could not be fetched
  NotFound,     // no bundle file on the card
  WrongBundle,  // the file's size or hash is not this build's bundle
  CardError,    // reading or writing the card failed
  OutOfMemory,
};

// Read from the card on first use, then remembered until forget().
State state();
const char* stateName(State state);
// Re-read the state on next use. The web server calls it on start: the card may
// have been swapped, or changed over USB, since the last session.
void forget();

// Streams one bundle file as a gzip-encoded response. Returns false, having
// sent nothing, when the bundle is not installed or the file cannot be read;
// the caller then sends the setup page. A file that turns out to be missing
// marks the bundle Missing, so the setup page reinstalls it.
//
// Browsers keep the file but ask again before every use (no-cache), with the
// bundle's name as its ETag: while the bundle stays, the answer is a bodiless
// 304; once another is installed, the new file. A cache lifetime instead kept a
// replaced bundle's pages and scripts in use for its length, and could pair a
// new page with an old script. Needs If-None-Match among the server's
// collected headers.
bool serve(WebServer& server, web_bundle::Index index);

// A bundle file that pages load by name under /js/ ("jszip.min.js"), streamed like serve(). Only
// the bundle's non-page files are found this way. False, having sent nothing, when this build's
// bundle has no such file or it cannot be served.
bool serveScript(WebServer& server, const char* name);

// The release this build is published under ("" for unpublished builds such as
// env:default), the repository it is published in, and the bundle's asset name.
const char* releaseTag();
const char* repository();
const char* assetName();

// True when a bundle sits in the card's top folder under the asset name: put
// there by the setup page's upload, or copied with a card reader.
bool bundleOnCard();

// Fetch the bundle from this build's GitHub release and install it. Blocks for
// the download; call from a request handler, as the font download does.
InstallError installFromRelease();
// Install the bundle from the card's top folder, and delete that copy.
InstallError installFromCard();
const char* errorText(InstallError error);

}  // namespace WebAssets
