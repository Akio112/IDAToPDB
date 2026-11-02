cmake_minimum_required(VERSION 3.20)
project(make_pdb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Поиск пакета LLVM (путь передается снаружи через -DLLVM_DIR)
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Найден LLVM ${LLVM_PACKAGE_VERSION} в ${LLVM_DIR}")

# Убираем зависимость от diaguids.lib для корректной сборки на Windows
if(TARGET LLVMDebugInfoPDB)
  get_target_property(_pdb_libs LLVMDebugInfoPDB INTERFACE_LINK_LIBRARIES)
  if(_pdb_libs)
    list(FILTER _pdb_libs EXCLUDE REGEX "diaguids")
    set_target_properties(LLVMDebugInfoPDB PROPERTIES INTERFACE_LINK_LIBRARIES "${_pdb_libs}")
  endif()
endif()

llvm_map_components_to_libnames(LLVM_LIBS DebugInfoPDB DebugInfoMSF DebugInfoCodeView Object BinaryFormat Support)

# Исполняемый файл с учетом новой структуры папки src/
add_executable(make_pdb 
    src/main.cpp 
    src/dbi_stream.cpp 
    src/info_stream.cpp 
    src/tpi_stream.cpp 
    src/pe_image.cpp
)

# Подключение заголовочных файлов из папки include/
target_include_directories(make_pdb PRIVATE include)
target_include_directories(make_pdb SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(make_pdb PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(make_pdb PRIVATE ${LLVM_LIBS})