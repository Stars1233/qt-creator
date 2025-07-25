// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "headerpathfilter.h"

#ifndef UNIT_TESTS
#include <coreplugin/icore.h>
#endif

#include <projectexplorer/project.h>
#include <projectexplorer/projectexplorerconstants.h>

#include <QRegularExpression>

#include <utils/algorithm.h>

using namespace ProjectExplorer;
using namespace Utils;

namespace CppEditor::Internal {

void HeaderPathFilter::process()
{
    const HeaderPaths &headerPaths = projectPart.headerPaths;

    addPreIncludesPath();

    for (const HeaderPath &headerPath : headerPaths)
        filterHeaderPath(headerPath);

    if (useTweakedHeaderPaths != UseTweakedHeaderPaths::No)
        tweakHeaderPaths();
}

bool HeaderPathFilter::isProjectHeaderPath(const FilePath &path) const
{
    return path == projectDirectory || path.isChildOf(projectDirectory) || projectDirectory.isEmpty()
        || path == buildDirectory || path.isChildOf(buildDirectory) || buildDirectory.isEmpty();
}

void HeaderPathFilter::removeGccInternalIncludePaths()
{
    if (projectPart.toolchainType != ProjectExplorer::Constants::GCC_TOOLCHAIN_TYPEID
        && projectPart.toolchainType != ProjectExplorer::Constants::MINGW_TOOLCHAIN_TYPEID) {
        return;
    }

    if (projectPart.toolchainInstallDir.isEmpty())
        return;

    const FilePath gccInstallDir = projectPart.toolchainInstallDir;
    auto isGccInternalInclude = [gccInstallDir](const HeaderPath &headerPath) {
        return headerPath.path == gccInstallDir.pathAppended("include")
               || headerPath.path == gccInstallDir.pathAppended("include-fixed");
    };

    Utils::erase(builtInHeaderPaths, isGccInternalInclude);
}

void HeaderPathFilter::filterHeaderPath(const ProjectExplorer::HeaderPath &headerPath)
{
    if (headerPath.path.isEmpty())
        return;

    switch (headerPath.type) {
    case HeaderPathType::BuiltIn:
        builtInHeaderPaths.push_back(headerPath);
        break;
    case HeaderPathType::System:
    case HeaderPathType::Framework:
        systemHeaderPaths.push_back(headerPath);
        break;
    case HeaderPathType::User:
        if (isProjectHeaderPath(headerPath.path))
            userHeaderPaths.push_back(headerPath);
        else
            systemHeaderPaths.push_back(headerPath);
        break;
    }
}

namespace {

HeaderPaths::iterator resourceIterator(HeaderPaths &headerPaths)
{
    // include/c++, include/g++, libc++\include and libc++abi\include
    static const QString cppIncludes = R"((.*/include/.*(g\+\+|c\+\+).*))"
                                       R"(|(.*libc\+\+/include))"
                                       R"(|(.*libc\+\+abi/include))"
                                       R"(|(/usr/local/include))";
    static const QRegularExpression includeRegExp("\\A(" + cppIncludes + ")\\z");

    return std::stable_partition(headerPaths.begin(),
                                 headerPaths.end(),
                                 [&](const HeaderPath &headerPath) {
                                     return includeRegExp.match(headerPath.path.path()).hasMatch();
                                 });
}

bool isClangSystemHeaderPath(const HeaderPath &headerPath)
{
    // Always exclude clang system includes (including intrinsics) which do not come with libclang
    // that Qt Creator uses for code model.
    // For example GCC on macOS uses system clang include path which makes clang code model
    // include incorrect system headers.
    static const QRegularExpression clangIncludeDir(
        R"(\A.*/lib\d*/clang/\d+(\.\d+){0,2}/include\z)");
    return clangIncludeDir.match(headerPath.path.path()).hasMatch();
}

void removeClangSystemHeaderPaths(HeaderPaths &headerPaths)
{
    auto newEnd = std::remove_if(headerPaths.begin(), headerPaths.end(), isClangSystemHeaderPath);
    headerPaths.erase(newEnd, headerPaths.end());
}

} // namespace

void HeaderPathFilter::tweakHeaderPaths()
{
    removeClangSystemHeaderPaths(builtInHeaderPaths);
    removeGccInternalIncludePaths();

    auto split = resourceIterator(builtInHeaderPaths);

    if (!clangIncludeDirectory.isEmpty())
        builtInHeaderPaths.insert(split, HeaderPath::makeBuiltIn(clangIncludeDirectory.path()));
}

void HeaderPathFilter::addPreIncludesPath()
{
    if (!projectDirectory.isEmpty()) {
        const FilePath rootProjectDirectory = projectDirectory / ".pre_includes";
        systemHeaderPaths.push_back(HeaderPath::makeSystem(rootProjectDirectory));
    }
}

} // namespace CppEditor::Internal
