# Install and use eventpp in your project

- [Install and use eventpp in your project](#install-and-use-eventpp-in-your-project)
	- [Include the source code in your project directly](#include-the-source-code-in-your-project-directly)
	- [Use CMake FetchContent](#use-cmake-fetchcontent)
	- [Use Vcpkg package manager](#use-vcpkg-package-manager)
	- [Use Conan package manager](#use-conan-package-manager)
	- [Use Hunter package manager](#use-hunter-package-manager)
	- [Use Homebrew on macOS (or Linux)](#use-homebrew-on-macos-or-linux)
	- [Install using CMake locally and use it in CMake](#install-using-cmake-locally-and-use-it-in-cmake)

`eventpp` package is available in C++ package managers Vcpkg, Conan, Hunter, and Homebrew.  
`eventpp` is header only and not requires building. There are various methods to use `eventpp`.  
Here lists all possible methods to use `eventpp`.  

## Include the source code in your project directly

eventpp is header only library. Just clone the source code, or use git submodule, then add the 'include' folder inside eventpp to your project include directory, then you can use the library.
You don't need to link to any source code.

## Use CMake FetchContent

Add below code to your CMake file

```
include(FetchContent)
FetchContent_Declare(
    eventpp
    GIT_REPOSITORY https://gitee.com/liudegui/eventpp.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(eventpp)
target_include_directories(your_target PRIVATE ${eventpp_SOURCE_DIR}/include)
```

Then `eventpp` is available to your project. If `GIT_TAG` is omitted, the latest code on master branch will be used.

## Use Vcpkg / Conan / Hunter / Homebrew

> **Note**: The packages available via Vcpkg, Conan, Hunter, and Homebrew are from the original [wqking/eventpp](https://github.com/wqking/eventpp) v0.1.3, which does not include the performance optimizations (OPT-1 ~ OPT-14) in this fork. For the optimized version, use the CMake FetchContent method above.

## Install using CMake locally and use it in CMake

Note: this is only an alternative, you should use the FetchContent method instead of this.  
If you are going to use eventpp in CMake managed project, you can install eventpp then use it in CMake.  
In eventpp root folder, run the commands,  
```
mkdir build
cd build
cmake ..
sudo make install
```

Then in the project CMakeLists.txt,   
```
# the project target is mytest, just for example
add_executable(mytest test.cpp)

find_package(eventpp)
if(eventpp_FOUND)
    target_link_libraries(mytest eventpp::eventpp)
else(eventpp_FOUND)
    message(FATAL_ERROR "eventpp library is not found")
endif(eventpp_FOUND)
```

Note: when using this method with MingW on Windows, by default CMake will install eventpp in non-writable system folder and get error. You should specify another folder to install. To do so, replace `cmake ..` with `cmake .. -DCMAKE_INSTALL_PREFIX="YOUR_NEW_LIB_FOLDER"`.
