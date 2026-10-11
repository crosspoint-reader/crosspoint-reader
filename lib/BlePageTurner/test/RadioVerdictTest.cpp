// radioVerdict, the one answer to "may the radio be on now", row by row.

#include <gtest/gtest.h>

#include "RadioPolicy.h"

namespace bleturner {
namespace {

// A book in front with its page shown, nothing else going on.
RadioInputs reading(const Phase phase) {
  RadioInputs in{};
  in.phase = phase;
  in.enabled = true;
  in.where = Where::Reader;
  in.pageShown = true;
  in.heap = {kMinimumFreeBytes, kMinimumLargestBlockBytes};
  return in;
}

struct Row {
  const char* name;
  RadioInputs in;
  Why want;
};

template <typename F>
RadioInputs with(const Phase phase, F change) {
  RadioInputs in = reading(phase);
  change(in);
  return in;
}

TEST(RadioVerdictTest, Table) {
  const Row rows[] = {
      // --- Idle: may this book visit ask for the radio ---
      {"idle: shown page", reading(Phase::Idle), Why::Ok},
      {"idle: turned off", with(Phase::Idle, [](RadioInputs& i) { i.enabled = false; }), Why::Off},
      {"idle: card busy", with(Phase::Idle, [](RadioInputs& i) { i.storageBusy = true; }), Why::StorageBusy},
      {"idle: home", with(Phase::Idle, [](RadioInputs& i) { i.where = Where::Elsewhere; }), Why::NotReading},
      {"idle: page not painted", with(Phase::Idle, [](RadioInputs& i) { i.pageShown = false; }), Why::PageNotShown},
      {"idle: build holds it", with(Phase::Idle, [](RadioInputs& i) { i.heldForBuild = i.idleStopped = true; }),
       Why::BuildNeedsHeap},
      {"idle: idle-stopped", with(Phase::Idle, [](RadioInputs& i) { i.idleStopped = true; }), Why::IdleNoLink},
      {"idle: book indexing", with(Phase::Idle, [](RadioInputs& i) { i.bookIndexing = true; }), Why::Indexing},
      {"idle: heap not asked yet", with(Phase::Idle, [](RadioInputs& i) { i.heap = {0, 0}; }), Why::Ok},
      {"idle: pairing screen skips the book",
       with(Phase::Idle,
            [](RadioInputs& i) {
              i.where = Where::PairingScreen;
              i.pageShown = false;
              i.bookIndexing = true;
            }),
       Why::Ok},
      {"idle: pairing screen still off",
       with(Phase::Idle,
            [](RadioInputs& i) {
              i.where = Where::PairingScreen;
              i.enabled = false;
            }),
       Why::Off},
      {"idle: pairing screen, card busy",
       with(Phase::Idle,
            [](RadioInputs& i) {
              i.where = Where::PairingScreen;
              i.storageBusy = true;
            }),
       Why::StorageBusy},

      // --- BeforeStart: the heap check, both numbers ---
      {"start: exact minimum", reading(Phase::BeforeStart), Why::Ok},
      {"start: one byte short in total",
       with(Phase::BeforeStart, [](RadioInputs& i) { i.heap = {kMinimumFreeBytes - 1, kMinimumLargestBlockBytes}; }),
       Why::HeapLow},
      {"start: block one byte short",
       with(Phase::BeforeStart, [](RadioInputs& i) { i.heap = {kMinimumFreeBytes, kMinimumLargestBlockBytes - 1}; }),
       Why::HeapInPieces},
      // X3, 27/09/2026: refused every 5 s for good, in the book and on Home alike.
      {"start: X3 heap in pieces", with(Phase::BeforeStart, [](RadioInputs& i) { i.heap = {87308, 23540}; }),
       Why::HeapInPieces},
      {"start: short in total is not in pieces",
       with(Phase::BeforeStart,
            [](RadioInputs& i) { i.heap = {kMinimumFreeBytes - 1, kMinimumLargestBlockBytes - 1}; }),
       Why::HeapLow},
      {"start: card busy first",
       with(Phase::BeforeStart,
            [](RadioInputs& i) {
              i.storageBusy = true;
              i.heap = {0, 0};
            }),
       Why::StorageBusy},
      {"start: only heap and card count",
       with(Phase::BeforeStart,
            [](RadioInputs& i) {
              i.enabled = false;
              i.where = Where::Elsewhere;
              i.idleStopped = true;
            }),
       Why::Ok},

      // --- JustStarted: keep the stack only with the reader's block left ---
      {"started: exact block kept", with(Phase::JustStarted, [](RadioInputs& i) { i.heap = {40000, 32768}; }), Why::Ok},
      // The heap check passed, so the bytes were there: a stack that leaves no whole block
      // behind split the heap. It counts toward the restart like a refusal in pieces.
      {"started: block one byte short rolled back",
       with(Phase::JustStarted, [](RadioInputs& i) { i.heap = {50000, 32767}; }), Why::HeapInPieces},
      // X3, 29/09/2026, 5,000-chapter book: 7 of 7 starts rolled back with these numbers.
      {"started: X3 big book", with(Phase::JustStarted, [](RadioInputs& i) { i.heap = {28812, 26612}; }),
       Why::HeapInPieces},
      {"started: card taken meanwhile", with(Phase::JustStarted, [](RadioInputs& i) { i.storageBusy = true; }),
       Why::StorageBusy},

      // --- Running: only off, the card and idleness stop it ---
      {"running: fine", reading(Phase::Running), Why::Ok},
      {"running: turned off", with(Phase::Running, [](RadioInputs& i) { i.enabled = false; }), Why::Off},
      {"running: card busy", with(Phase::Running, [](RadioInputs& i) { i.storageBusy = true; }), Why::StorageBusy},
      {"running: heap never stops it", with(Phase::Running, [](RadioInputs& i) { i.heap = {0, 0}; }), Why::Ok},
      {"running: outside the book it stays", with(Phase::Running, [](RadioInputs& i) { i.where = Where::Elsewhere; }),
       Why::Ok},
      {"running: idle just under the limit", with(Phase::Running, [](RadioInputs& i) { i.idleMs = kIdleOffMs - 1; }),
       Why::Ok},
      {"running: idle at the limit", with(Phase::Running, [](RadioInputs& i) { i.idleMs = kIdleOffMs; }),
       Why::IdleNoLink},
      {"running: idle long past", with(Phase::Running, [](RadioInputs& i) { i.idleMs = kIdleOffMs * 3; }),
       Why::IdleNoLink},
      {"running: a linked remote is never idle",
       with(Phase::Running,
            [](RadioInputs& i) {
              i.linked = true;
              i.idleMs = kIdleOffMs * 100;
            }),
       Why::Ok},
  };
  for (const Row& row : rows) {
    EXPECT_EQ(static_cast<int>(radioVerdict(row.in)), static_cast<int>(row.want)) << row.name;
  }
}

// The numbers a user sees: changing one changes behaviour on the device.
TEST(RadioVerdictTest, Thresholds) {
  EXPECT_EQ(kMinimumFreeBytes, 65536u);
  EXPECT_EQ(kMinimumLargestBlockBytes, 32768u);
  EXPECT_EQ(kIdleOffMs, 5u * 60u * 1000u);
}

}  // namespace
}  // namespace bleturner
