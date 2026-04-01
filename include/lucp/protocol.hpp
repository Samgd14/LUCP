#pragma once

#include <cstdint>

namespace lucp
{

  constexpr uint8_t MAGIC_0 = 0xFA;
  constexpr uint8_t MAGIC_1 = 0x51;
  constexpr uint16_t HEADER_SIZE = 4;

  // Return codes
  constexpr int OK = 0;
  constexpr int ERR_INVALID_ID = -1;
  constexpr int ERR_QUEUE_FULL = -2;
  constexpr int ERR_PAL_SEND = -3;
  constexpr int ERR_BAD_ARG = -4;
  constexpr int ERR_NOT_IMPLEMENTED = -5;

} // namespace lucp
