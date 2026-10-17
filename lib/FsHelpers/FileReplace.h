#pragma once

// Putting a file in the place of another without ever losing the one that was there.
//
// FAT cannot rename onto a name that is taken, so a replace has to free the name first. The
// transfers used to free it by removing the old file: an upload removed it before the new data
// had arrived, a WebDAV PUT or MOVE removed it before the rename that then failed, and either way
// a dropped connection or a failed rename left the card with neither copy. Here the old file only
// steps aside, and goes for good once the new one is in place.
//
// Adapted from CrossInk (uxjulia/CrossInk) commit e092f81da (Julia Nguyen), MIT, whose WebDAV
// replaceFile() parks the old file as .davold; shared here by every transfer that replaces a file.
namespace FileReplace {

// Where the old file waits while the new one moves in: "<destPath>.old".
inline constexpr char kAsideSuffix[] = ".old";

// Moves `fromPath` (a complete file, or what a WebDAV MOVE moves) to `destPath`, replacing the file
// there if there is one. The old file is first renamed to <destPath>.old and removed only once
// `fromPath` is in place; when that rename fails it goes back.
//
// False leaves both where they were: `destPath` as it was (or, should even putting it back fail,
// as <destPath>.old), and `fromPath` for the caller to remove or keep. It also refuses, touching
// nothing, when `destPath` is a folder (the old one could not be removed afterwards) or when
// <destPath>.old is already there: an old file that an earlier replace could not put back, or a
// file of the user's. The log says which.
bool replace(const char* fromPath, const char* destPath);

}  // namespace FileReplace
