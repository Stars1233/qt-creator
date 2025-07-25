cmake_minimum_required(VERSION 3.20)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake")

project(qtcreatorcdbext)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED QTCREATORCDBEXT_INSTALL_LLVM)
  set(QTCREATORCDBEXT_INSTALL_LLVM YES) # default
endif()

set(ArchSuffix "32")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(ArchSuffix "64")
endif()

if(MSVC_CXX_ARCHITECTURE_ID MATCHES "^ARM")
  set(ArchSuffix "arm${ArchSuffix}")
endif()

if (NOT QT_CREATOR_API_DEFINED)
  # standalone build
  include(QtCreatorIDEBranding)
  include(QtCreatorAPI)
  include(QtCreatorSbom)
  qtc_handle_compiler_cache_support()

  # Need to look for Qt6 optionally, so that we can use the SBOM feature, even though the
  # project doesn't link to Qt.
  find_package(Qt6
    COMPONENTS Core
  )

  set(QT_CREATOR_CDB_EXT_STANDALONE_BUILD TRUE)
  qtc_setup_sbom()
  qtc_sbom_compute_cpe(project_cpe
      VENDOR "qt"
      PRODUCT "${PROJECT_NAME}"
      VERSION "${IDE_VERSION}"
  )
  set(QT_SBOM_LICENSE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../LICENSES")
  qtc_sbom_begin_project(
    SBOM_PROJECT_NAME "${PROJECT_NAME}-${ArchSuffix}"
    QT_REPO_PROJECT_NAME "${PROJECT_NAME}-${ArchSuffix}"
    PURL_NAMESPACE "qt"
    PURL_NAME "${PROJECT_NAME}"
    CPE "${project_cpe}"
  )

  # Compile for x86, x64 and arm64
  if (NOT ${PROJECT_NAME}-MultiBuild AND NOT MINGW)
    include(ExternalProject)

    set(generator "Visual Studio 16 2019")
    if(CMAKE_CXX_COMPILER MATCHES "Microsoft Visual Studio/2022/")
      set(generator "Visual Studio 17 2022")
    endif()

    string(REPLACE ";" "|" CMAKE_PREFIX_PATH_ALT_SEP "${CMAKE_PREFIX_PATH}")

    macro (setup_library arch install_llvm)
      ExternalProject_Add(${arch}-bld
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
        CMAKE_GENERATOR "${generator}"
        CMAKE_GENERATOR_PLATFORM "${arch}"
        LIST_SEPARATOR |
        CMAKE_ARGS
          -D${PROJECT_NAME}-MultiBuild=ON
          -DPythonTargetArchDll=${PythonTarget${arch}Dll}
          -DPython3_ROOT_DIR=${Python3_ROOT_DIR}
          -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH_ALT_SEP}
          -DQTCREATORCDBEXT_INSTALL_LLVM=${install_llvm}
        BUILD_COMMAND
          ${CMAKE_COMMAND} --build . --config ${CMAKE_BUILD_TYPE}
        INSTALL_COMMAND
          ${CMAKE_COMMAND} --install . --config ${CMAKE_BUILD_TYPE}
                           --prefix "${CMAKE_BINARY_DIR}" --component qtcreatorcdbext
      )
    endmacro()

    if (NOT QTCREATORCDBEXT_BUILD_ARCHS)
      set(QTCREATORCDBEXT_BUILD_ARCHS arm64 win32 x64)
    endif()
    set(install_llvm YES)
    foreach(arch IN LISTS QTCREATORCDBEXT_BUILD_ARCHS)
      setup_library(${arch} ${install_llvm})
      set(install_llvm NO)
    endforeach()

    list(LENGTH QTCREATORCDBEXT_BUILD_ARCHS build_archs_length)
    if (build_archs_length GREATER 0)
      install(
        DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        DESTINATION .
        COMPONENT qtcreatorcdbext
      )
      install(CODE
        "if (EXISTS \"${CMAKE_BINARY_DIR}/bin\")
          message(\"Copying ${CMAKE_BINARY_DIR}/bin to ${CMAKE_INSTALL_PREFIX}\")
          file(COPY \"${CMAKE_BINARY_DIR}/bin\" DESTINATION \"${CMAKE_INSTALL_PREFIX}\")
         endif()"
        COMPONENT qtcreatorcdbext
      )
    endif()

    return()
  endif()
endif()

if (NOT WIN32 OR NOT MSVC OR BUILD_DESIGNSTUDIO)
  return()
endif()

if (NOT EXISTS "${CMAKE_BINARY_DIR}/lib/qtcreatorcdbext${ArchSuffix}")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/lib/qtcreatorcdbext${ArchSuffix}")
endif()

add_qtc_library(qtcreatorcdbext SHARED
  COMPONENT qtcreatorcdbext
  DEPENDS dbgeng
  DESTINATION lib/qtcreatorcdbext${ArchSuffix}/
  SOURCES
    common.cpp common.h
    containers.cpp containers.h
    eventcallback.cpp eventcallback.h
    extensioncontext.cpp extensioncontext.h
    gdbmihelpers.cpp gdbmihelpers.h
    iinterfacepointer.h
    knowntype.h
    outputcallback.cpp outputcallback.h
    qtcreatorcdbext.def
    qtcreatorcdbextension.cpp
    stringutils.cpp stringutils.h
    symbolgroup.cpp symbolgroup.h
    symbolgroupnode.cpp symbolgroupnode.h
    symbolgroupvalue.cpp symbolgroupvalue.h
)

qtc_library_enabled(_library_enabled qtcreatorcdbext)
if (_library_enabled)
  # statically link MSVC runtime
  set_property(TARGET qtcreatorcdbext PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(qtcreatorcdbext PUBLIC /EHsc)

  find_package(Python3 3.8 COMPONENTS Development)

  if (NOT ${Python3_Development_FOUND})
    message(WARNING "PythonLibs (at least version 3.8) not found. qtcreatorcdbext will be built without Python support.")
    return()
  endif()

  set(PythonRegex "^(.*)/(.*)/(python([0-9]+))${CMAKE_IMPORT_LIBRARY_SUFFIX}$")
  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(PythonRegex "^(.*)/(.*)/(python([0-9]+)_d)${CMAKE_IMPORT_LIBRARY_SUFFIX}$")
  endif()

  foreach(lib IN LISTS Python3_LIBRARIES)
    if (lib MATCHES ${PythonRegex})
      if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(PythonZipFileName "python${CMAKE_MATCH_4}_d.zip")
      else()
        set(PythonZipFileName "python${CMAKE_MATCH_4}.zip")
      endif()
      set(PythonNameWithVersion "${CMAKE_MATCH_3}")

      set(PythonDll "${CMAKE_MATCH_1}/${PythonNameWithVersion}${CMAKE_SHARED_LIBRARY_SUFFIX}")
      set(PythonExe "${CMAKE_MATCH_1}/python${CMAKE_EXECUTABLE_SUFFIX}")
      set(PythonZip "${CMAKE_MATCH_1}/${PythonZipFileName}")

      break()
    endif()
  endforeach()

  if (NOT PythonDll)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
      message(WARNING "The Debug build of Qt Creator requires Debug Python libraries. Please check your Python installation")
    endif()
    message(WARNING "PythonDll not found. qtcreatorcdbext will be built without Python support.")
    return()
  endif()

  # Support for cross-compilation for arm64 on a x64 system
  if (MSVC_CXX_ARCHITECTURE_ID STREQUAL "ARM64" AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "ARM64")
    set(arm64_on_arm64 ON)
  endif()
  if (MSVC_CXX_ARCHITECTURE_ID STREQUAL "x64" AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "AMD64")
    set(x64_on_x64 ON)
  endif()

  if (NOT arm64_on_arm64 AND NOT x64_on_x64)
    find_program(dumpbin_executable dumpbin)
    find_program(lib_executable lib)

    string(TOLOWER ${MSVC_CXX_ARCHITECTURE_ID} lower_arch_name)
    if (lower_arch_name STREQUAL "arm64")
      set(python_suffix "arm64")
    elseif (lower_arch_name STREQUAL "x64")
      set(python_suffix "amd64")
    else()
      set(python_suffix "win32")
    endif()

    if (NOT dumpbin_executable OR NOT lib_executable)
      message(WARNING "Couldn't locate dumpbin.exe or lib.exe executables")
      return()
    endif()

    if (Python3_VERSION VERSION_LESS "3.11.0" AND lower_arch_name STREQUAL "arm64")
      message(WARNING "Python 3.11.0 needs to be installed. This version is the first version that has arm64 Windows support")
      return()
    endif()

    file(TO_NATIVE_PATH ${PythonDll} NativePythonDll)
    execute_process(
      COMMAND ${dumpbin_executable} /exports ${NativePythonDll}
      OUTPUT_VARIABLE dumpbin_output
      RESULT_VARIABLE dumpbin_result)

    string(REGEX REPLACE ".*[ \t]+ordinal[ \t]+hint[ \t]+RVA[ \t]+name[\r\n][\r\n]" "" dumpbin_output "${dumpbin_output}")
    string(REGEX REPLACE "[\r\n][ \t]+Summary[\r\n].*" "" dumpbin_output "${dumpbin_output}")
    string(REGEX REPLACE "([ \t]+[0-9]+)([ \t]+[a-fA-F0-9]+)([ \t]+[a-fA-F0-9]+)[ \t]+([a-zA-Z0-9_]+)( = [a-zA-Z0-9_]+[\r\n]|[\r\n])" "\\4;" filter_output "${dumpbin_output}")

    string(APPEND pythondef "LIBRARY ${PythonNameWithVersion}\nEXPORTS\n")
    foreach(var IN LISTS filter_output)
      if (var)
        string(APPEND pythondef "${var}\n")
      endif()
    endforeach()
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${PythonNameWithVersion}.def "${pythondef}")

    execute_process(
      COMMAND "${lib_executable}"
              /def:${CMAKE_CURRENT_BINARY_DIR}/${PythonNameWithVersion}.def
              /out:${CMAKE_CURRENT_BINARY_DIR}/${PythonNameWithVersion}.lib /machine:${lower_arch_name} /nologo)
    set(Python3_LIBRARIES "${CMAKE_CURRENT_BINARY_DIR}/${PythonNameWithVersion}.lib")

    if (NOT PythonTargetArchDll AND ENV{PythonTargetArchDll})
      set(PythonTargetArchDll $ENV{PythonTargetArchDll})
    endif()

    if (NOT PythonTargetArchDll)
      set(python_embed_url "https://www.python.org/ftp/python/${Python3_VERSION}/python-${Python3_VERSION}-embed-${python_suffix}.zip")
      message(STATUS "Downloading ${python_embed_url}")

      foreach(retry RANGE 10)
        file(DOWNLOAD ${python_embed_url} ${CMAKE_CURRENT_BINARY_DIR}/python-embed.zip)
        file(SIZE ${CMAKE_CURRENT_BINARY_DIR}/python-embed.zip fileSize)
        if (fileSize GREATER 0)
          break()
        endif()
      endforeach()

      file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/python-embed)
      file(ARCHIVE_EXTRACT INPUT ${CMAKE_CURRENT_BINARY_DIR}/python-embed.zip DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/python-embed)

      set(PythonTargetArchDll ${CMAKE_CURRENT_BINARY_DIR}/python-embed/${PythonNameWithVersion}${CMAKE_SHARED_LIBRARY_SUFFIX})
    endif()

    if (NOT PythonTargetArchDll)
      message(WARNING "PythonTargetArchDll CMake parameter or ENV{PythonTargetArchDll} was not configured and the Python runtime cannot be configured")
      return()
    endif()

    set(PythonDll "${PythonTargetArchDll}")
  endif()

  extend_qtc_library(qtcreatorcdbext
    DEPENDS "${Python3_LIBRARIES}"
    INCLUDES "${Python3_INCLUDE_DIRS}"
    DEFINES WITH_PYTHON=1 PY_SSIZE_T_CLEAN
    SOURCES
      pycdbextmodule.cpp pycdbextmodule.h
      pyfield.cpp pyfield.h
      pystdoutredirect.cpp pystdoutredirect.h
      pytype.cpp pytype.h
      pyvalue.cpp pyvalue.h
  )

  if (NOT EXISTS "${PythonZip}" AND
      NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/${PythonZipFileName}")
    include(CreatePythonXY)
    create_python_xy("${PythonExe}" "${CMAKE_CURRENT_BINARY_DIR}/${PythonZipFileName}")
  endif()

  if (NOT EXISTS "${PythonZip}" AND
      EXISTS "${CMAKE_CURRENT_BINARY_DIR}/${PythonZipFileName}")
    set(PythonZip "${CMAKE_CURRENT_BINARY_DIR}/${PythonZipFileName}")
  endif()

  list(APPEND deployPythonFiles "${PythonDll}")
  list(APPEND deployPythonFiles "${PythonZip}")

  install(FILES ${deployPythonFiles}
          DESTINATION lib/qtcreatorcdbext${ArchSuffix}/
          COMPONENT qtcreatorcdbext)

  add_custom_target(copy_python_dll ALL VERBATIM)

  qtc_output_binary_dir(output_binary_dir)
  add_custom_command(TARGET copy_python_dll POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${deployPythonFiles} "${output_binary_dir}/lib/qtcreatorcdbext${ArchSuffix}/"
    VERBATIM
  )

  if (QTCREATORCDBEXT_INSTALL_LLVM)
    # Deploy lldb.exe and its Python dependency
    find_package(Clang QUIET)
    if (LLVM_TOOLS_BINARY_DIR AND LLVM_LIBRARY_DIRS)
      file(GLOB python_files RELATIVE ${LLVM_TOOLS_BINARY_DIR} "${LLVM_TOOLS_BINARY_DIR}/python*")
      foreach(lldb_file lldb.exe lldb-dap.exe liblldb.dll ${python_files})
        if (EXISTS ${LLVM_TOOLS_BINARY_DIR}/${lldb_file})
          install(FILES ${LLVM_TOOLS_BINARY_DIR}/${lldb_file}
                  DESTINATION bin/clang/bin
                  COMPONENT qtcreatorcdbext)
        endif()
      endforeach()

      if (EXISTS ${LLVM_LIBRARY_DIRS}/site-packages)
        install(DIRECTORY ${LLVM_LIBRARY_DIRS}/site-packages
                DESTINATION bin/clang/lib
                COMPONENT qtcreatorcdbext
                PATTERN "_lldb.cp*64.pyd" EXCLUDE)
      endif()
    endif()
  endif()

endif()

if(QT_CREATOR_CDB_EXT_STANDALONE_BUILD)
  qtc_sbom_end_project()
endif()
