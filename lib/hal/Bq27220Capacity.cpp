#include "Bq27220Capacity.h"

// Section numbers are those of the BQ27220 Technical Reference Manual, SLUUBD4A.
namespace {
constexpr uint8_t CONTROL = 0x00;                 // Control() (2.2)
constexpr uint8_t OPERATION_STATUS = 0x3A;        // OperationStatus() (2.27)
constexpr uint8_t OPERATION_STATUS_HIGH = 0x3B;   // its high byte, read alone in 6.1 steps 4 and 15
constexpr uint8_t CFGUPDATE_BIT = 1 << 2;         // bit 2 of that byte: CONFIG UPDATE mode
constexpr uint8_t DESIGN_CAPACITY = 0x3C;         // DesignCapacity() (2.28)
constexpr uint8_t MAC_CONTROL = 0x3E;             // ManufacturerAccessControl()
constexpr uint8_t MAC_DATA = 0x40;                // MACData() (2.29)
constexpr uint8_t MAC_DATA_SUM = 0x60;            // MACDataSum() (2.30), MACDataLen() at 0x61
constexpr uint8_t MAC_DATA_LEN_MIN = 2 + 2 + 2;   // address, one two-byte parameter, sum and length
constexpr uint8_t MAC_DATA_LEN_MAX = 2 + 32 + 2;  // a full 32-byte block

constexpr uint16_t UNSEAL_KEY_1 = 0x0414;
constexpr uint16_t UNSEAL_KEY_2 = 0x3672;
constexpr uint16_t FULL_ACCESS_KEY = 0xFFFF;  // sent twice
// The X3's gauge ignores key words sent back to back; spaced this far apart each one takes.
constexpr uint32_t KEY_GAP_MS = 1500;
// Time for the gauge to bring a newly selected block into MACData().
constexpr uint32_t SELECT_SETTLE_MS = 10;
constexpr uint16_t SEALED = 0x0030;
constexpr uint16_t ENTER_CFG_UPDATE = 0x0090;
constexpr uint16_t EXIT_CFG_UPDATE_REINIT = 0x0091;
constexpr uint16_t EXIT_CFG_UPDATE = 0x0092;

// CEDV Profile 1 in Data Memory (table 3-2), two bytes, most significant first.
constexpr uint16_t DM_FULL_CHARGE_CAPACITY = 0x929D;  // Learned Full Charge Capacity
constexpr uint16_t DM_DESIGN_CAPACITY = 0x929F;

// SEC[1:0], OperationStatus() bits 2:1.
constexpr uint8_t SEC_SEALED = 0b11;
constexpr uint8_t SEC_FULL_ACCESS = 0b01;

// CONFIG UPDATE is read no sooner than 2 s after the command (2.2.21), then at most twice a second
// (5.3). Past 5 s the load gives up.
constexpr uint32_t FLAG_FIRST_READ_MS = 2000;
constexpr uint32_t FLAG_POLL_MS = 500;
constexpr uint32_t FLAG_GIVE_UP_MS = 5000;

bool readWord(Bq27220Capacity::Bus& bus, const uint8_t reg, uint16_t& value) {
  uint8_t bytes[2];
  if (!bus.read(reg, bytes, 2)) return false;
  value = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
  return true;
}

uint8_t security(const uint16_t operationStatus) { return (operationStatus >> 1) & 0b11; }

bool due(const uint32_t nowMs, const uint32_t atMs) { return static_cast<int32_t>(nowMs - atMs) >= 0; }

bool control(Bq27220Capacity::Bus& bus, const uint16_t subcommand) {
  // Both bytes in one write: the X3's gauge ignores a subcommand sent a byte at a time (6.1 step 1).
  const uint8_t bytes[] = {static_cast<uint8_t>(subcommand & 0xFF), static_cast<uint8_t>(subcommand >> 8)};
  return bus.write(CONTROL, bytes, 2);
}
}  // namespace

uint8_t Bq27220Capacity::replaceChecksum(const uint8_t oldSum, const uint8_t oldMsb, const uint8_t oldLsb,
                                         const uint8_t newMsb, const uint8_t newLsb) {
  const uint8_t rest = static_cast<uint8_t>(255 - oldSum - oldMsb - oldLsb);
  return static_cast<uint8_t>(255 - static_cast<uint8_t>(rest + newMsb + newLsb));
}

Bq27220Capacity::Param Bq27220Capacity::writeParam(Bus& bus, const uint16_t address, const bool required) {
  const uint8_t addressLow = address & 0xFF;
  const uint8_t addressHigh = address >> 8;
  const uint8_t addressBytes[] = {addressLow, addressHigh};
  if (!bus.write(MAC_CONTROL, addressBytes, 2)) return Param::Failed;
  bus.pause(SELECT_SETTLE_MS);
  // Reading ManufacturerAccessControl() back confirms the block MACData() now holds (2.29).
  uint8_t selected[2];
  if (!bus.read(MAC_CONTROL, selected, 2) || selected[0] != addressLow || selected[1] != addressHigh) {
    return Param::Failed;
  }
  // Old value first: reading MACDataSum() and MACDataLen() moves MACData() on to the next 32 bytes,
  // so the block is selected again before the new value goes in.
  uint8_t oldValue[2] = {};
  uint8_t oldSumAndLength[2] = {};
  if (!bus.read(MAC_DATA, oldValue, 2) || !bus.read(MAC_DATA_SUM, oldSumAndLength, 2)) return Param::Failed;
  const uint8_t oldMsb = oldValue[0];
  const uint8_t oldLsb = oldValue[1];
  const uint8_t oldSum = oldSumAndLength[0];
  const uint8_t length = oldSumAndLength[1];
  if (length < MAC_DATA_LEN_MIN || length > MAC_DATA_LEN_MAX) return Param::Failed;
  if (((oldMsb << 8) | oldLsb) != TI_DEFAULT_MAH) return required ? Param::Failed : Param::Skipped;
  const uint8_t newMsb = target >> 8;
  const uint8_t newLsb = target & 0xFF;
  wroteData = true;
  if (!bus.write(MAC_CONTROL, addressBytes, 2)) return Param::Failed;
  bus.pause(SELECT_SETTLE_MS);
  // Writing MACDataLen() after the checksum moves the block into RAM (6.1 step 13).
  const uint8_t value[] = {newMsb, newLsb};
  const uint8_t sumAndLength[] = {replaceChecksum(oldSum, oldMsb, oldLsb, newMsb, newLsb), length};
  if (!bus.write(MAC_DATA, value, 2) || !bus.write(MAC_DATA_SUM, sumAndLength, 2)) return Param::Failed;
  return Param::Written;
}

void Bq27220Capacity::wait(const uint32_t nowMs, const Stage next) {
  stage = next;
  sentAt = nowMs;
  nextAt = nowMs + FLAG_FIRST_READ_MS;
}

void Bq27220Capacity::tick(Bus& bus, const uint32_t nowMs) {
  switch (stage) {
    case Stage::Check: {
      if (target == 0) {
        outcome = Result::NotNeeded;
        stage = Stage::Done;
        return;
      }
      uint16_t status = 0;
      if (!readWord(bus, DESIGN_CAPACITY, dcRead) ||
          (dcRead == TI_DEFAULT_MAH && !readWord(bus, OPERATION_STATUS, status))) {
        // Nothing was sent to the gauge: nothing to undo.
        failStage = stage;
        outcome = Result::Failed;
        stage = Stage::Done;
        return;
      }
      if (dcRead != TI_DEFAULT_MAH) {
        // Already loaded since the gauge last lost power, or configured by someone else.
        outcome = Result::NotNeeded;
        stage = Stage::Done;
        return;
      }
      // Unseal if sealed, then FULL ACCESS, which the gauge does not boot in. One key word a tick.
      keyCount = 0;
      if (security(status) == SEC_SEALED) {
        keys[keyCount++] = UNSEAL_KEY_1;
        keys[keyCount++] = UNSEAL_KEY_2;
      }
      if (security(status) != SEC_FULL_ACCESS) {
        keys[keyCount++] = FULL_ACCESS_KEY;
        keys[keyCount++] = FULL_ACCESS_KEY;
      }
      keysSent = 0;
      stage = Stage::Access;
      nextAt = nowMs;
      return;
    }
    case Stage::Access: {
      if (!due(nowMs, nextAt)) return;
      if (keysSent < keyCount) {
        if (!control(bus, keys[keysSent++])) return giveUp(bus, nowMs);
        nextAt = nowMs + KEY_GAP_MS;
        return;
      }
      uint16_t status = 0;
      if (!readWord(bus, OPERATION_STATUS, status) || security(status) != SEC_FULL_ACCESS) return giveUp(bus, nowMs);
      inConfigUpdate = true;
      if (!control(bus, ENTER_CFG_UPDATE)) return giveUp(bus, nowMs);
      return wait(nowMs, Stage::WaitEnter);
    }
    case Stage::WaitEnter: {
      if (!due(nowMs, nextAt)) return;
      uint8_t high = 0;
      if (!bus.read(OPERATION_STATUS_HIGH, &high, 1)) return giveUp(bus, nowMs);
      if (!(high & CFGUPDATE_BIT)) {
        if (due(nowMs, sentAt + FLAG_GIVE_UP_MS)) return giveUp(bus, nowMs);
        nextAt = nowMs + FLAG_POLL_MS;
        return;
      }
      stage = Stage::Block;
      // The reinit copies Learned Full Charge Capacity into FullChargeCapacity(), so it goes in
      // first: if Design Capacity then fails, it still reads 3000 and the next start loads both
      // again. A learned value other than the default is kept.
      if (writeParam(bus, DM_FULL_CHARGE_CAPACITY, false) == Param::Failed ||
          writeParam(bus, DM_DESIGN_CAPACITY, true) != Param::Written) {
        return giveUp(bus, nowMs);
      }
      inConfigUpdate = false;
      if (!control(bus, EXIT_CFG_UPDATE_REINIT)) {
        failed = true;
        failStage = stage;
        return seal(bus);
      }
      return wait(nowMs, Stage::WaitExit);
    }
    case Stage::WaitExit: {
      if (!due(nowMs, nextAt)) return;
      uint8_t high = 0;
      const bool read = bus.read(OPERATION_STATUS_HIGH, &high, 1);
      if (read && (high & CFGUPDATE_BIT) && !due(nowMs, sentAt + FLAG_GIVE_UP_MS)) {
        nextAt = nowMs + FLAG_POLL_MS;
        return;
      }
      if (!read || (high & CFGUPDATE_BIT)) {
        if (!failed) failStage = stage;
        failed = true;
      }
      return seal(bus);
    }
    case Stage::Block:
    case Stage::Seal:
    case Stage::Done:
      return;
  }
}

void Bq27220Capacity::giveUp(Bus& bus, const uint32_t nowMs) {
  failed = true;
  failStage = stage;
  if (inConfigUpdate) {
    inConfigUpdate = false;
    // Reinit only when a block write had started, so the gauge restarts from what its RAM holds.
    if (control(bus, wroteData ? EXIT_CFG_UPDATE_REINIT : EXIT_CFG_UPDATE)) return wait(nowMs, Stage::WaitExit);
  }
  seal(bus);
}

void Bq27220Capacity::seal(Bus& bus) {
  // Whatever the access mode was at boot: end equipment keeps the gauge SEALED (2.2.15).
  stage = Stage::Seal;
  if (!control(bus, SEALED)) {
    if (!failed) failStage = stage;
    failed = true;
  }
  if (!readWord(bus, DESIGN_CAPACITY, dcRead)) dcRead = 0;
  outcome = !failed && dcRead == target ? Result::Loaded : Result::Failed;
  if (outcome == Result::Failed && !failed) failStage = stage;
  stage = Stage::Done;
}

void Bq27220Capacity::abandon(Bus& bus) {
  if (!running()) return;
  failed = true;
  failStage = stage;
  if (inConfigUpdate) {
    inConfigUpdate = false;
    control(bus, wroteData ? EXIT_CFG_UPDATE_REINIT : EXIT_CFG_UPDATE);
  }
  seal(bus);
}
