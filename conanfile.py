from conan import ConanFile
from conan.errors import ConanException
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class DztraderConan(ConanFile):
    name = "dztrader"
    package_type = "application"

    def set_version(self):
        """
        版本号从根 CMakeLists.txt 的 project(VERSION) 读取（ADR 0038）。

        单一真相源在 CMake：version.h 从它生成、dz_version_* 全由它来，
        这里再硬编码一份就会漂移（曾出现两处手工同步、无校验）。
        """
        import re
        import pathlib

        cmake_lists = pathlib.Path(self.recipe_folder) / "CMakeLists.txt"
        text = cmake_lists.read_text(encoding="utf-8")
        match = re.search(
            r"^\s*VERSION\s+(\d+\.\d+\.\d+)\s*$", text, re.MULTILINE
        )
        if not match:
            raise ConanException(
                f"cannot read project(VERSION) from {cmake_lists}"
            )
        self.version = match.group(1)

    settings = "os", "arch", "compiler", "build_type"

    # boost 组件裁剪：禁用项目不用的组件以加速编译
    # 直接使用组件：system,thread,regex,date_time,locale,stacktrace,iostreams,
    #               program_options,context + asio(header-only) + ssl(openssl)
    # 传递依赖必须一并启用：
    #   thread    -> atomic,chrono,container,date_time,exception
    #   process   -> context,filesystem   (process 本身 header-only)
    #   filesystem-> atomic
    #   iostreams -> random,regex
    #   locale    -> filesystem,thread,regex
    default_options = {
        # spdlog 用 std::format（匹配项目 SPDLOG_USE_STD_FORMAT 宏，不依赖 fmt）
        # 注意：选项名是 use_std_fmt（不是 std_format）
        "spdlog/*:use_std_fmt": True,
        # spdlog compiled 静态库：用 spdlog 默认 level 名（trace/debug/info/warning/error/critical）
        # 不再自定义 SPDLOG_LEVEL_NAMES，与 spdlog 原生行为一致，便于 AI 识别
        "spdlog/*:header_only": False,
        "boost/*:without_atomic": False,
        "boost/*:without_chrono": False,
        "boost/*:without_cobalt": True,
        "boost/*:without_container": False,
        "boost/*:without_context": False,
        "boost/*:without_contract": True,
        "boost/*:without_coroutine": True,
        "boost/*:without_date_time": False,
        "boost/*:without_exception": False,
        "boost/*:without_fiber": True,
        "boost/*:without_filesystem": False,
        "boost/*:without_graph": True,
        "boost/*:without_graph_parallel": True,
        "boost/*:without_iostreams": False,
        "boost/*:without_json": True,
        "boost/*:without_locale": False,
        "boost/*:without_log": True,
        "boost/*:without_math": True,
        "boost/*:without_mpi": True,
        "boost/*:without_nowide": True,
        "boost/*:without_program_options": False,
        "boost/*:without_python": True,
        "boost/*:without_random": False,
        "boost/*:without_regex": False,
        "boost/*:without_serialization": True,
        "boost/*:without_stacktrace": False,
        "boost/*:without_system": False,
        "boost/*:without_test": True,
        "boost/*:without_thread": False,
        "boost/*:without_timer": True,
        "boost/*:without_type_erasure": True,
        "boost/*:without_wave": True,
        "boost/*:without_ssl": False,  # openssl=true -> 启用 boost::asio::ssl
        # drogon: 关闭 boost 集成（trantor 自带事件循环，与项目 boost 1.90.0 解耦）
        # 否则 drogon recipe 硬依赖 boost/1.83.0，与项目 boost/1.90.0 冲突
        "drogon/*:with_boost": False,
    }

    def requirements(self):
        # 核心依赖（固定版本）
        self.requires("boost/1.90.0")
        # Boost.MySQL 驱动直连 OpenSSL（TLS + caching_sha2 认证）：显式声明为直接依赖，
        # 不靠 drogon 的传递引入（版本与当前解析结果一致）；ZLIB 经 openssl 导入目标传递
        self.requires("openssl/3.6.4")
        self.requires("spdlog/1.17.0")
        self.requires("toml11/4.4.0")
        self.requires("sqlitecpp/3.3.3")
        self.requires("magic_enum/0.9.7")
        self.requires("nlohmann_json/3.12.0")
        self.requires("gtest/1.17.0")
        self.requires("drogon/1.9.12")
        self.requires("msgpack-cxx/7.0.0")

    def generate(self):
        CMakeToolchain(self).generate()
        CMakeDeps(self).generate()
