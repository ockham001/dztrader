/**
 * @file def.h
 * @author ockham (ockham001@foxmail.com)
 * @brief
 * @date 2024-06-17
 *
 * @copyright Copyright (C) 2024, ockham. All rights reserved.
 * This program is distributed under the terms of the GNU Lesser General
 * Public License version 3 (LGPLv3) as published by the Free Software
 * Foundation. See accompanying file COPYING.LESSER or visit
 * https://opensource.org/license/lgpl-3-0 for full license details.
 *
 *
 */
#ifndef __DZLIB_DEF_H__
#define __DZLIB_DEF_H__
#include <stdint.h>

extern "C" {

const uint16_t kDzLenSymbol = 81;
const uint16_t kDzLenDepth = 5;
const uint16_t kDzLenGeneralName = 64;

enum DzErrorID {
  DzErrorNone = 0,
  DzErrorUnknown = -1,
};

struct DzTick {
  char symbol[kDzLenSymbol];
  uint16_t depth;
  int32_t date;
  int32_t time;
  int64_t nanoseconds;

  double open_price;
  double high_price;
  double low_price;
  double last_price;

  double prev_close_price;
  double prev_settlement_price;

  double avg_price;
  double volume;
  double open_interest;

  double upper_limit_price;
  double lower_limit_price;

  double bid_price[kDzLenDepth];
  double bid_volume[kDzLenDepth];
  double ask_price[kDzLenDepth];
  double ask_volume[kDzLenDepth];
};

struct DzBar {
  char symbol[kDzLenSymbol];
  int32_t date;
  int32_t time;

  int time_scale;
  int period_count;
  int sourt_key;

  double open;
  double high;
  double low;
  double close;

  double volume;
  double open_interest;
};
}

#endif  // __DZLIB_DEF_H__