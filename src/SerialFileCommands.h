#pragma once

struct SerialCommand;

// The serial console's file commands: LS, GET, PUT, MKDIR and RM (protocol in
// util/SerialTransfer.h). Each runs to completion inside the main loop, so no
// screen changes while a file moves. Changes are accepted only while Home is in
// front; LS and GET work on any screen. Built with ENABLE_SERIAL_FILES, which
// release builds set too (unlike the rest of the console).
void runSerialFileCommand(const SerialCommand& cmd);
