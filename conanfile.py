"""The Conan recipe of libbdf: `conan create .` builds the library from this
checkout (`--test-folder=packaging/conan/test_package` builds a consumer
against it)."""
import os
import re

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, load


class BdfConan(ConanFile):
    name = "bdf"
    license = "MIT"
    url = "https://github.com/shibukawa/libbdf"
    homepage = "https://github.com/shibukawa/libbdf"
    description = "A C reader and renderer of BDF (Browser-specific Document Format) documents"
    topics = ("document", "viewer", "renderer", "c")
    package_type = "library"
    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False], "with_brotli": [True, False]}
    default_options = {"shared": False, "fPIC": True, "with_brotli": True}
    exports_sources = (
        "CMakeLists.txt",
        "LICENSE",
        "cmake/*",
        "include/*",
        "src/*",
        "third_party/*",
        "tools/*",
        "adapters/*",
    )

    def set_version(self):
        # the version is the one the header states
        header = load(self, os.path.join(self.recipe_folder, "include", "bdf", "bdf.h"))
        self.version = re.search(r'#define BDF_VERSION_STRING\s+"([^"]+)"', header).group(1)

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")
        # C only
        self.settings.rm_safe("compiler.libcxx")
        self.settings.rm_safe("compiler.cppstd")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["BDF_BUILD_TESTS"] = False
        tc.variables["BDF_WITH_BROTLI"] = bool(self.options.with_brotli)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))
        for notice in ("brotli", "bearssl", "go-math", "go-image", "go-text", "unicode"):
            copy(self, "LICENSE", os.path.join(self.source_folder, "third_party", notice),
                 os.path.join(self.package_folder, "licenses", notice))
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "bdf")
        self.cpp_info.set_property("cmake_target_name", "bdf::bdf")
        self.cpp_info.set_property("pkg_config_name", "bdf")
        self.cpp_info.libs = ["bdf"]
        if self.options.shared:
            self.cpp_info.defines = ["BDF_SHARED"]
        if self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs = ["m"]
