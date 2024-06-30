/**
 * @file develop_deps.hpp
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
#ifndef __DZLIB_DEVELOP_DEPS_H__
#define __DZLIB_DEVELOP_DEPS_H__

#ifdef DZLIB_TMOL_EXTERNAL
#include <toml11/toml.hpp>
#else
#include "./deps/toml11/toml.hpp"
#endif  // DZLIB_TMOL_EXTERNAL

#ifdef DZLIB_MAGIC_ENUM_EXTERNAL
#include <magic_enum/magic_enum.hpp>
#else
#include "./deps/magic_enum/magic_enum.hpp"
#endif  // DZLIB_MAGIC_ENUM_EXTERNAL

#ifdef DZLIB_NLOHMANN_EXTERNAL
#include <nlohmann/json.hpp>
#else
#include "./deps/nlohmann/json.hpp"
#endif  // DZLIB_NLOHMANN_EXTERNAL

#endif  // __DZLIB_DEVELOP_DEPS_H__