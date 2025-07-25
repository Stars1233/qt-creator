// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "utils_global.h"

#include "filepath.h"

#include <QHash>
#include <QStringList>
#include <QUrl>

namespace Utils {
class QrcParser;

class QTCREATOR_UTILS_EXPORT FileInProjectFinder
{
public:

    using FileHandler = std::function<void(const FilePath &, int)>;
    using DirectoryHandler = std::function<void(const QStringList &, int)>;

    FileInProjectFinder();
    ~FileInProjectFinder();

    void setProjectDirectory(const FilePath &absoluteProjectPath);
    FilePath projectDirectory() const;

    void setProjectFiles(const FilePaths &projectFiles);
    void setSysroot(const FilePath &sysroot);

    void addMappedPath(const FilePath &localFilePath, const QString &remoteFilePath);

    FilePaths findFile(const QUrl &fileUrl, bool *success = nullptr) const;
    bool findFileOrDirectory(const FilePath &originalPath, FileHandler fileHandler = nullptr,
                             DirectoryHandler directoryHandler = nullptr) const;

    FilePaths searchDirectories() const;
    void setAdditionalSearchDirectories(const FilePaths &searchDirectories);

private:
    struct PathMappingNode
    {
        FilePath localPath;
        QHash<QString, PathMappingNode> children;
    };

    struct CacheEntry {
        FilePaths paths;
        int matchLength = 0;
    };

    class QrcUrlFinder {
    public:
        FilePaths find(const QUrl &fileUrl) const;
        void setProjectFiles(const FilePaths &projectFiles);
    private:
        FilePaths m_allQrcFiles;
        mutable QHash<QUrl, FilePaths> m_fileCache;
        mutable QHash<FilePath, std::shared_ptr<QrcParser>> m_parserCache;
    };

    static CacheEntry findInSearchPath(const FilePath &searchPath, const FilePath &filePath,
                                       FileHandler fileHandler, DirectoryHandler directoryHandler);
    FilePaths filesWithSameFileName(const QString &fileName) const;
    FilePaths pathSegmentsWithSameName(const QString &path) const;

    bool checkRootDirectory(const FilePath &originalPath, DirectoryHandler directoryHandler) const;
    bool checkMappedPath(const FilePath &originalPath,
                         FileHandler fileHandler,
                         DirectoryHandler directoryHandler) const;
    bool checkCache(const FilePath &originalPath,
                    FileHandler fileHandler,
                    DirectoryHandler directoryHandler) const;
    bool checkProjectDirectory(const FilePath &originalPath,
                               FileHandler fileHandler,
                               DirectoryHandler directoryHandler) const;
    bool checkProjectFiles(const FilePath &originalPath,
                           FileHandler fileHandler,
                           DirectoryHandler directoryHandler) const;
    bool checkSearchPaths(const FilePath &originalPath,
                          FileHandler fileHandler,
                          DirectoryHandler directoryHandler) const;
    bool checkSysroot(const FilePath &originalPath,
                      FileHandler fileHandler,
                      DirectoryHandler directoryHandler) const;
    bool handleSuccess(const FilePath &originalPath, const FilePaths &found, int confidence,
                       const char *where) const;

    static int commonPostFixLength(const FilePath &candidatePath, const FilePath &filePathToFind);
    static FilePaths bestMatches(const FilePaths &filePaths, const FilePath &filePathToFind);

    FilePath m_projectDir;
    FilePath m_sysroot;
    FilePaths m_projectFiles;
    FilePaths m_searchDirectories;
    PathMappingNode m_pathMapRoot;

    mutable QHash<FilePath, CacheEntry> m_cache;
    QrcUrlFinder m_qrcUrlFinder;
};

QTCREATOR_UTILS_EXPORT FilePath chooseFileFromList(const FilePaths &candidates);

} // namespace Utils
