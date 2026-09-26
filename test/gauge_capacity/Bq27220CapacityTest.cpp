// A BQ27220 model with just enough registers and Data Memory to hold the capacity load to the
// sequence of the BQ27220 Technical Reference Manual (SLUUBD4A) section 6.1, byte for byte.
#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "Bq27220Capacity.h"

namespace {

constexpr uint16_t TARGET = 650;
constexpr uint16_t DM_BASE = 0x9180;  // Data Memory RAM window of table 3-2
constexpr uint16_t DM_FCC = 0x929D;
constexpr uint16_t DM_DC = 0x929F;

class FakeGauge final : public Bq27220Capacity::Bus {
 public:
  // SEC[1:0]: 3 sealed, 2 unsealed (the state after a reset, TRM 3.2), 1 full access.
  uint8_t sec = 2;
  bool cfgUpdate = false;
  uint32_t now = 0;
  uint32_t enterDelayMs = 900;  // under the "up to 1 second" of 6.1 step 4
  uint32_t exitDelayMs = 400;
  bool neverEnters = false;
  int failAt = -1;              // index of the transaction the bus refuses
  int dcRegisterOverride = -1;  // DesignCapacity() disagreeing with Data Memory
  uint16_t fullChargeCapacity = 3000;
  int commits = 0;
  int reinits = 0;
  std::vector<std::string> log;

  FakeGauge() {
    setDm(DM_FCC, 3000);
    setDm(DM_DC, 3000);
    // Neighbours in the block, so a checksum over the wrong bytes shows.
    setDm(0x929B, 0x102A);
    setDm(0x92A3, 3700);
    setDm(0x92A5, 100);
    setDm(0x92A7, 3743);
  }

  void setDm(const uint16_t address, const uint16_t value) {
    dm[address - DM_BASE] = value >> 8;
    dm[address - DM_BASE + 1] = value & 0xFF;
  }
  uint16_t getDm(const uint16_t address) const {
    return static_cast<uint16_t>((dm[address - DM_BASE] << 8) | dm[address - DM_BASE + 1]);
  }

  bool write(const uint8_t reg, const uint8_t* data, const uint8_t count) override {
    std::string entry = "W";
    char hex[4];
    std::snprintf(hex, sizeof(hex), "%02X=", reg);
    entry += hex;
    for (uint8_t i = 0; i < count; ++i) {
      std::snprintf(hex, sizeof(hex), "%02X", data[i]);
      entry += hex;
    }
    if (!record(entry.c_str())) return false;
    settle();
    if (reg == 0x00) {
      // Like the X3's gauge: a Control() subcommand sent a byte at a time is ignored.
      if (count != 2) return true;
      const uint16_t code = static_cast<uint16_t>(data[0] | (data[1] << 8));
      // Like the X3's gauge: a key word sent right after another one is ignored.
      const bool key = code == 0x0414 || code == 0x3672 || code == 0xFFFF;
      if (key && keySeen && now - lastKeyAt < 1) return true;
      if (key) {
        keySeen = true;
        lastKeyAt = now;
      }
      subcommand(code);
      return true;
    }
    for (uint8_t i = 0; i < count; ++i) writeByte(static_cast<uint8_t>(reg + i), data[i]);
    return true;
  }

  void pause(uint32_t) override {}

  bool read(const uint8_t reg, uint8_t* out, const uint8_t count) override {
    char entry[16];
    std::snprintf(entry, sizeof(entry), "R%02X:%u", reg, count);
    if (!record(entry)) return false;
    settle();
    for (uint8_t i = 0; i < count; ++i) out[i] = byteAt(static_cast<uint8_t>(reg + i));
    // Like the X3's gauge: reading MACDataSum() or MACDataLen() moves MACData() on to the next 32 bytes.
    if (reg <= 0x61 && reg + count > 0x60) {
      macAddress = static_cast<uint16_t>(macAddress + 32);
      loadBlock();
    }
    return true;
  }

  // The checksum 2.30 describes, over the whole block: an oracle independent of 6.1 step 11.
  uint8_t blockSum(const std::array<uint8_t, 32>& data, const uint8_t length) const {
    unsigned sum = (macAddress & 0xFF) + (macAddress >> 8);
    for (uint8_t i = 0; i + 4 < length; ++i) sum += data[i];
    return static_cast<uint8_t>(255 - (sum & 0xFF));
  }

 private:
  static constexpr uint8_t LENGTH = 0x24;  // 6.1 step 13

  void writeByte(const uint8_t reg, const uint8_t value) {
    if (reg == 0x3E) {
      macAddress = static_cast<uint16_t>((macAddress & 0xFF00) | value);
    } else if (reg == 0x3F) {
      macAddress = static_cast<uint16_t>((macAddress & 0x00FF) | (value << 8));
      loadBlock();
    } else if (reg >= 0x40 && reg < 0x60) {
      block[reg - 0x40] = value;
    } else if (reg == 0x60) {
      pendingSum = value;
    } else if (reg == 0x61) {
      commit(value);
    }
  }

  bool record(const char* entry) {
    const bool refused = failAt >= 0 && static_cast<int>(log.size()) == failAt;
    log.emplace_back(std::string(entry) + (refused ? "!" : ""));
    return !refused;
  }

  void settle() {
    if (changeAt >= 0 && now >= static_cast<uint32_t>(changeAt)) {
      cfgUpdate = enteringConfig;
      changeAt = -1;
    }
  }

  void subcommand(const uint16_t code) {
    if (code == 0x3672 && lastSub == 0x0414 && sec == 3) sec = 2;
    if (code == 0xFFFF && lastSub == 0xFFFF && sec == 2) sec = 1;
    if (code == 0x0090 && sec == 1 && !neverEnters) {
      enteringConfig = true;
      changeAt = static_cast<int64_t>(now + enterDelayMs);
    }
    if (code == 0x0091 || code == 0x0092) {
      if (code == 0x0091) {
        fullChargeCapacity = getDm(DM_FCC);  // 1.1.10: FCC copies Learned Full Charge Capacity
        ++reinits;
      }
      enteringConfig = false;
      changeAt = static_cast<int64_t>(now + exitDelayMs);
    }
    if (code == 0x0030) sec = 3;
    lastSub = code == 0xFFFF && lastSub == 0xFFFF ? 0 : code;
  }

  void loadBlock() {
    for (uint8_t i = 0; i < 32; ++i) {
      const int offset = macAddress - DM_BASE + i;
      block[i] = offset >= 0 && offset < static_cast<int>(dm.size()) ? dm[offset] : 0;
    }
    stored = block;
  }

  void commit(const uint8_t length) {
    // The block moves into RAM only in CONFIG UPDATE, with FULL ACCESS and the right checksum.
    if (!cfgUpdate || sec != 1 || length != LENGTH || pendingSum != blockSum(block, length)) return;
    for (uint8_t i = 0; i + 4 < length; ++i) dm[macAddress - DM_BASE + i] = block[i];
    stored = block;
    ++commits;
  }

  uint8_t byteAt(const uint8_t reg) const {
    const uint16_t dc = dcRegisterOverride >= 0 ? static_cast<uint16_t>(dcRegisterOverride) : getDm(DM_DC);
    const uint16_t status = static_cast<uint16_t>((cfgUpdate ? 0x0400 : 0) | (sec << 1) | 0x20);
    switch (reg) {
      case 0x3A:
        return status & 0xFF;
      case 0x3B:
        return status >> 8;
      case 0x3C:
        return dc & 0xFF;
      case 0x3D:
        return dc >> 8;
      case 0x12:
        return fullChargeCapacity & 0xFF;
      case 0x13:
        return fullChargeCapacity >> 8;
      case 0x3E:
        return macAddress & 0xFF;
      case 0x3F:
        return macAddress >> 8;
      case 0x60:
        return blockSum(stored, LENGTH);
      case 0x61:
        return LENGTH;
      default:
        return reg >= 0x40 && reg < 0x60 ? block[reg - 0x40] : 0;
    }
  }

  std::array<uint8_t, 0x180> dm{};
  std::array<uint8_t, 32> block{};
  std::array<uint8_t, 32> stored{};
  uint16_t macAddress = 0;
  uint8_t pendingSum = 0;
  uint16_t lastSub = 0;
  bool keySeen = false;
  uint32_t lastKeyAt = 0;
  bool enteringConfig = false;
  int64_t changeAt = -1;
};

// Ticks the load every 10 ms, as the main loop would, until it ends or the time runs out.
void run(Bq27220Capacity& load, FakeGauge& gauge, const uint32_t forMs = 20000) {
  for (uint32_t t = 0; t <= forMs && load.result() == Bq27220Capacity::Result::Pending; t += 10) {
    gauge.now = t;
    load.tick(gauge, t);
  }
}

std::vector<std::string> param(const char* low, const char* oldSum) {
  return {std::string("W3E=") + low + "92",   "R3E:2", "R40:2", "R60:2", std::string("W3E=") + low + "92", "W40=028A",
          std::string("W60=") + oldSum + "24"};
}

std::vector<std::string> join(std::vector<std::vector<std::string>> parts) {
  std::vector<std::string> all;
  for (auto& part : parts) all.insert(all.end(), part.begin(), part.end());
  return all;
}

std::string checksumFor(FakeGauge& gauge, const uint16_t address, const uint16_t value) {
  FakeGauge copy = gauge;
  copy.setDm(address, value);
  // Select the block in a copy and read what 2.30 gives for it.
  copy.failAt = -1;
  const uint8_t addressBytes[] = {static_cast<uint8_t>(address & 0xFF), static_cast<uint8_t>(address >> 8)};
  copy.write(0x3E, addressBytes, 2);
  uint8_t sum = 0;
  copy.read(0x60, &sum, 1);
  char text[4];
  std::snprintf(text, sizeof(text), "%02X", sum);
  return text;
}

}  // namespace

TEST(Bq27220Capacity, LoadsTheBatteryCapacityInTheManualsOrder) {
  FakeGauge gauge;
  const std::string fccSum = checksumFor(gauge, DM_FCC, TARGET);
  const std::string dcSum = checksumFor(gauge, DM_DC, TARGET);
  Bq27220Capacity load(TARGET);
  run(load, gauge);

  const auto expected = join({
      {"R3C:2", "R3A:2"},
      {"W00=FFFF", "W00=FFFF"},  // FULL ACCESS: unsealed is not enough
      {"R3A:2"},
      {"W00=9000"},  // ENTER_CFG_UPDATE
      {"R3B:1"},     // 2 s later, already set
      param("9D", fccSum.c_str()),
      param("9F", dcSum.c_str()),
      {"W00=9100"},  // EXIT_CFG_UPDATE_REINIT
      {"R3B:1"},
      {"W00=3000"},  // SEALED
      {"R3C:2"},
  });
  EXPECT_EQ(gauge.log, expected);
  EXPECT_EQ(load.result(), Bq27220Capacity::Result::Loaded);
  EXPECT_EQ(load.designCapacity(), TARGET);
  EXPECT_EQ(gauge.getDm(DM_DC), TARGET);
  EXPECT_EQ(gauge.getDm(DM_FCC), TARGET);
  EXPECT_EQ(gauge.commits, 2);
  EXPECT_EQ(gauge.reinits, 1);
  EXPECT_EQ(gauge.fullChargeCapacity, TARGET);
  EXPECT_EQ(gauge.sec, 3);
  EXPECT_FALSE(gauge.cfgUpdate);
  // The neighbours in the block are as they were.
  EXPECT_EQ(gauge.getDm(0x929B), 0x102A);
  EXPECT_EQ(gauge.getDm(0x92A3), 3700);
}

TEST(Bq27220Capacity, ReadsTheConfigUpdateFlagNoSoonerThanTwoSecondsAndAtMostTwiceASecond) {
  FakeGauge gauge;
  gauge.enterDelayMs = 3100;
  Bq27220Capacity load(TARGET);
  std::vector<uint32_t> reads;
  uint32_t enteredAt = 0;
  for (uint32_t t = 0; t <= 20000 && load.result() == Bq27220Capacity::Result::Pending; t += 10) {
    gauge.now = t;
    const size_t before = gauge.log.size();
    load.tick(gauge, t);
    for (size_t i = before; i < gauge.log.size(); ++i) {
      if (gauge.log[i] == "W00=9000") enteredAt = t;
      if (gauge.log[i] == "R3B:1") reads.push_back(t - enteredAt);
    }
  }
  EXPECT_EQ(load.result(), Bq27220Capacity::Result::Loaded);
  ASSERT_GE(reads.size(), 4u);
  EXPECT_EQ(reads[0], 2000u);
  EXPECT_EQ(reads[1], 2500u);
  EXPECT_EQ(reads[2], 3000u);
  EXPECT_EQ(reads[3], 3500u);  // set 3100 after the command, seen here
}

TEST(Bq27220Capacity, SealedGaugeTakesTheUnsealKeysFirstAndIsSealedAgain) {
  FakeGauge gauge;
  gauge.sec = 3;
  Bq27220Capacity load(TARGET);
  run(load, gauge);
  ASSERT_GE(gauge.log.size(), 7u);
  const std::vector<std::string> start(gauge.log.begin(), gauge.log.begin() + 7);
  EXPECT_EQ(start,
            (std::vector<std::string>{"R3C:2", "R3A:2", "W00=1404", "W00=7236", "W00=FFFF", "W00=FFFF", "R3A:2"}));
  EXPECT_EQ(load.result(), Bq27220Capacity::Result::Loaded);
  EXPECT_EQ(gauge.sec, 3);
}

TEST(Bq27220Capacity, AnyCapacityOtherThanTheDefaultIsLeftAlone) {
  for (const uint16_t dc : {uint16_t{650}, uint16_t{2999}, uint16_t{3001}, uint16_t{1200}, uint16_t{0}}) {
    FakeGauge gauge;
    gauge.setDm(DM_DC, dc);
    Bq27220Capacity load(TARGET);
    run(load, gauge);
    EXPECT_EQ(gauge.log, std::vector<std::string>{"R3C:2"}) << dc;
    EXPECT_EQ(load.result(), Bq27220Capacity::Result::NotNeeded) << dc;
    EXPECT_EQ(gauge.getDm(DM_DC), dc);
    EXPECT_EQ(gauge.sec, 2) << "the access mode is not touched either";
  }
}

TEST(Bq27220Capacity, NoTargetMeansNoBusTraffic) {
  FakeGauge gauge;
  Bq27220Capacity load(0);
  run(load, gauge);
  EXPECT_TRUE(gauge.log.empty());
  EXPECT_EQ(load.result(), Bq27220Capacity::Result::NotNeeded);
}

TEST(Bq27220Capacity, ALearnedFullChargeCapacityIsKept) {
  FakeGauge gauge;
  gauge.setDm(DM_FCC, 820);
  Bq27220Capacity load(TARGET);
  run(load, gauge);
  EXPECT_EQ(load.result(), Bq27220Capacity::Result::Loaded);
  EXPECT_EQ(gauge.getDm(DM_FCC), 820);
  EXPECT_EQ(gauge.getDm(DM_DC), TARGET);
  EXPECT_EQ(gauge.commits, 1);
}

TEST(Bq27220Capacity, DataMemoryDisagreeingWithDesignCapacityIsNotWritten) {
  FakeGauge gauge;
  gauge.setDm(DM_DC, 1200);
  gauge.dcRegisterOverride = 3000;
  Bq27220Capacity load(TARGET);
  run(load, gauge);
  EXPECT_EQ(load.result(), Bq27220Capacity::Result::Failed);
  EXPECT_EQ(load.failedAt(), Bq27220Capacity::Stage::Block);
  EXPECT_EQ(gauge.getDm(DM_DC), 1200);
  // The Learned Full Charge Capacity went in first, so the exit reinitializes.
  EXPECT_EQ(gauge.reinits, 1);
  EXPECT_EQ(gauge.sec, 3);
  EXPECT_FALSE(gauge.cfgUpdate);
}

TEST(Bq27220Capacity, ConfigUpdateThatNeverComesGivesUpWithoutWriting) {
  FakeGauge gauge;
  gauge.neverEnters = true;
  Bq27220Capacity load(TARGET);
  run(load, gauge);
  EXPECT_EQ(load.result(), Bq27220Capacity::Result::Failed);
  EXPECT_EQ(load.failedAt(), Bq27220Capacity::Stage::WaitEnter);
  int polls = 0;
  for (const auto& entry : gauge.log) {
    EXPECT_NE(entry.substr(0, 3), "W40") << "no Data Memory write";
    polls += entry == "R3B:1";
  }
  EXPECT_LE(polls, 8);  // 2 s to 5 s at two a second, then one after the exit
  ASSERT_GE(gauge.log.size(), 5u);
  const std::vector<std::string> tail(gauge.log.end() - 5, gauge.log.end());
  EXPECT_EQ(tail, (std::vector<std::string>{"R3B:1", "W00=9200", "R3B:1", "W00=3000", "R3C:2"}));
  EXPECT_EQ(gauge.reinits, 0);
  EXPECT_EQ(gauge.sec, 3);
}

TEST(Bq27220Capacity, EveryRefusedTransactionEndsCleanSealedAndOnce) {
  FakeGauge clean;
  Bq27220Capacity reference(TARGET);
  run(reference, clean);
  const size_t transactions = clean.log.size();
  ASSERT_GT(transactions, 20u);

  for (size_t failAt = 0; failAt < transactions; ++failAt) {
    FakeGauge gauge;
    gauge.failAt = static_cast<int>(failAt);
    Bq27220Capacity load(TARGET);
    run(load, gauge);
    SCOPED_TRACE("refused transaction " + std::to_string(failAt) + " " + clean.log[failAt]);
    ASSERT_NE(load.result(), Bq27220Capacity::Result::Pending);
    const size_t ended = gauge.log.size();
    // Nothing more once it has ended: no second attempt.
    for (uint32_t t = 30000; t < 60000; t += 10) {
      gauge.now = t;
      load.tick(gauge, t);
    }
    EXPECT_EQ(gauge.log.size(), ended);
    EXPECT_FALSE(load.running());

    int enters = 0;
    bool sentSomething = false;
    for (const auto& entry : gauge.log) sentSomething |= entry[0] == 'W';
    for (const auto& entry : gauge.log) enters += entry == "W00=9000";
    EXPECT_LE(enters, 1);
    // Capacity and Learned Full Charge Capacity are either untouched or loaded, never anything else.
    EXPECT_TRUE(gauge.getDm(DM_DC) == 3000 || gauge.getDm(DM_DC) == TARGET);
    EXPECT_TRUE(gauge.getDm(DM_FCC) == 3000 || gauge.getDm(DM_FCC) == TARGET);
    if (failAt == transactions - 1) {
      // Only the closing DesignCapacity() read was refused: the load itself went in.
      EXPECT_EQ(load.result(), Bq27220Capacity::Result::Failed);
      EXPECT_EQ(gauge.getDm(DM_DC), TARGET);
      continue;
    }
    EXPECT_EQ(load.result(), Bq27220Capacity::Result::Failed);
    if (!sentSomething) {
      // Refused before a single write: the gauge is as it was.
      EXPECT_EQ(gauge.sec, 2);
      continue;
    }
    const std::string refused = gauge.log[failAt];
    const bool sealRefused = refused == "W00=3000!";
    if (!sealRefused) {
      EXPECT_EQ(gauge.sec, 3) << "sealed again";
      // The last subcommand sent is SEALED.
      ASSERT_GE(gauge.log.size(), 2u);
      EXPECT_EQ(gauge.log[gauge.log.size() - 2], "W00=3000");
    }
    // A refused exit is not sent twice: the gauge leaves CONFIG UPDATE by itself after about
    // 240 s (TRM 4.6).
    const bool exitRefused = refused == "W00=9100!";
    if (!exitRefused) EXPECT_FALSE(gauge.cfgUpdate) << "out of CONFIG UPDATE";
  }
}

TEST(Bq27220Capacity, SleepDuringTheLoadLeavesConfigUpdateAndSealsAtOnce) {
  FakeGauge gauge;
  Bq27220Capacity load(TARGET);
  uint32_t t = 0;
  bool entered = false;
  for (; t <= 10000 && !entered; t += 10) {
    gauge.now = t;
    load.tick(gauge, t);
    entered = !gauge.log.empty() && gauge.log.back() == "W00=9000";
  }
  ASSERT_TRUE(load.running());
  gauge.now = t + 1000;
  const size_t before = gauge.log.size();
  load.abandon(gauge);
  const std::vector<std::string> after(gauge.log.begin() + static_cast<long>(before), gauge.log.end());
  EXPECT_EQ(after, (std::vector<std::string>{"W00=9200", "W00=3000", "R3C:2"}));
  EXPECT_EQ(load.result(), Bq27220Capacity::Result::Failed);
  EXPECT_FALSE(load.running());
  EXPECT_EQ(gauge.sec, 3);
  gauge.now = t + 3000;
  load.tick(gauge, t + 3000);
  EXPECT_EQ(gauge.log.size(), before + 3);
}

TEST(Bq27220Capacity, SleepWhileTheKeysAreGoingOutOnlySeals) {
  FakeGauge gauge;
  Bq27220Capacity load(TARGET);
  gauge.now = 0;
  load.tick(gauge, 0);  // the check, then the first key word on the next tick
  gauge.now = 10;
  load.tick(gauge, 10);
  ASSERT_TRUE(load.running());
  const size_t before = gauge.log.size();
  gauge.now = 1000;
  load.abandon(gauge);
  const std::vector<std::string> after(gauge.log.begin() + static_cast<long>(before), gauge.log.end());
  EXPECT_EQ(after, (std::vector<std::string>{"W00=3000", "R3C:2"}));
  EXPECT_EQ(gauge.sec, 3);
  EXPECT_FALSE(gauge.cfgUpdate);
}

TEST(Bq27220Capacity, ChecksumReplacementMatchesTheWholeBlockSum) {
  // 6.1 step 11 against the sum of 2.30 over a whole block, for every old and new value byte.
  FakeGauge gauge;
  std::array<uint8_t, 32> data{};
  for (uint8_t i = 0; i < 32; ++i) data[i] = static_cast<uint8_t>(i * 37 + 11);
  for (unsigned oldMsb = 0; oldMsb < 256; oldMsb += 17) {
    for (unsigned newValue = 0; newValue < 65536; newValue += 4099) {
      data[0] = static_cast<uint8_t>(oldMsb);
      data[1] = static_cast<uint8_t>(oldMsb ^ 0x5A);
      const uint8_t oldSum = gauge.blockSum(data, 0x24);
      const uint8_t replaced =
          Bq27220Capacity::replaceChecksum(oldSum, data[0], data[1], newValue >> 8, newValue & 0xFF);
      data[0] = static_cast<uint8_t>(newValue >> 8);
      data[1] = static_cast<uint8_t>(newValue & 0xFF);
      EXPECT_EQ(replaced, gauge.blockSum(data, 0x24));
    }
  }
}
