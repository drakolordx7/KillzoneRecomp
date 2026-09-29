#pragma once
#include <cstdint>
#include <vector>
class PS2Memory;
extern std::vector<std::vector<uint8_t>> g_recompGifPackets;
PS2Memory* recompCreateMemory(uint8_t* vu1Code, uint8_t* vu1Data);
void recompMarkCodeModified(PS2Memory* m);
