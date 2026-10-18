extern bool testFuses[3];
inline bool esp_efuse_read_field_bit(int id) { return testFuses[id]; }
