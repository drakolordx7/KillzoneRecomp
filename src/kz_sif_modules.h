#pragma once

#include <cstdint>

struct R5900Context;
class PS2Runtime;

void kzSceSifSearchModuleByName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void kzSceSifUnloadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void kzLgkbmInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
