#pragma once
// A test's view of the pipeline's log lines (LoggingImpl.cpp): called with every line before it is
// printed, the message already formatted. One observer at a time; nullptr removes it. Lets a test
// tell the phases of a book load apart by the lines that end them, without hooks in the code.
using LogObserver = void (*)(const char* level, const char* origin, const char* message);
void setLogObserver(LogObserver observer);
