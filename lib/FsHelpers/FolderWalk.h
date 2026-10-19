#pragma once

#include <string>

// Every file below a folder, in a walk whose memory grows with the depth of the tree, never with
// its width. One folder is open at a time; each folder above it keeps only where its listing
// stopped, a directory position, and is reopened there once the folder below is done. A queue of
// the folders still to visit holds the path of every folder of a level at once: a library of
// thousands of author folders could take the heap, and on this target a growth that fails aborts.
//
// The walk reopens folders by path and seeks back into their listings, so the tree it walks must
// not change while it runs. `visit` may change the card anywhere else.
namespace FolderWalk {

// Takes each file's path, "<dir>/<folder>/.../<name>", and the `ctx` eachFile() was given.
using FileVisitor = void (*)(const std::string& path, void* ctx);

// Hands `visit` every file below `dir`, depth first in listing order; folders are not handed over.
// A folder that will not open, or not seek back, is left out with what is below it. False when
// nothing could be walked: `dir` is no folder, or the buffer for long names was not to be had.
bool eachFile(const std::string& dir, FileVisitor visit, void* ctx);

}  // namespace FolderWalk
