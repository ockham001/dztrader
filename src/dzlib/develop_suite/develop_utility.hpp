/**
 * @file develop_utility.hpp
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
#ifndef __DZLIB_DEVELOP_UTILITY_H__
#define __DZLIB_DEVELOP_UTILITY_H__

#include "develop_def.hpp"
namespace dzlib {
namespace detail {

template <typename T>
inline std::string FormatSize(T bytes, T unit_value,
                              const std::string& unit_str, int precision = -1) {
  using namespace std;
  if (bytes % unit_value == 0) {
    return std::format("{}{}", bytes / unit_value, unit_str);
  } else {
    const double k = static_cast<double>(bytes) / unit_value;
    if (precision >= 0) {
      return format("{:.{}f}{}", k, precision, unit_str);
    } else {
      if (k < 10.0)
        precision = 3;
      else if (k < 100.0)
        precision = 2;
      else
        precision = 1;
      return format("{:.{}f}{}", k, precision, unit_str);
    }
  }
}
}  // namespace detail

template <typename T>
inline std::string FormatBytesAsString(T bytes, int precision = -1) {
  if (bytes < KB) {
    return std::format("{}B", bytes);
  } else if (bytes < MB) {
    return detail::FormatSize(bytes, KB, "KB", precision);
  } else if (bytes < GB) {
    return detail::FormatSize(bytes, MB, "MB", precision);
  } else if (bytes < TB) {
    return detail::FormatSize(bytes, GB, "GB", precision);
  } else {
    return detail::FormatSize(bytes, TB, "TB", precision);
  }
}

}  // namespace dzlib

#endif  // __DZLIB_DEVELOP_UTILITY_H__