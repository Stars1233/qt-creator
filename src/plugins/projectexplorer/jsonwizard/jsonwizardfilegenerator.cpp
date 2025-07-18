// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "jsonwizardfilegenerator.h"

#include "jsonwizardgeneratorfactory.h"

#include "../projectexplorertr.h"
#include "jsonwizard.h"
#include "jsonwizardfactory.h"

#include <coreplugin/editormanager/editormanager.h>

#include <utils/algorithm.h>
#include <utils/macroexpander.h>
#include <utils/qtcassert.h>
#include <utils/stringutils.h>
#include <utils/templateengine.h>

#include <QVariant>

using namespace Utils;

namespace ProjectExplorer::Internal {

class JsonWizardFileGenerator final : public JsonWizardGenerator
{
public:
    Result<> setup(const QVariant &data);

    Core::GeneratedFiles fileList(MacroExpander *expander,
                                  const FilePath &wizardDir,
                                  const FilePath &projectDir,
                                  QString *errorMessage) final;

    Result<> writeFile(const JsonWizard *wizard, Core::GeneratedFile *file) final;

private:
    class File {
    public:
        bool keepExisting = false;
        FilePath source;
        FilePath target;
        QVariant condition = true;
        QVariant isBinary = false;
        QVariant overwrite = false;
        QVariant openInEditor = false;
        QVariant openAsProject = false;
        QVariant isTemporary = false;

        QList<JsonWizard::OptionDefinition> options;
    };

    Result<Core::GeneratedFile> generateFile(const File &file, MacroExpander *expander);

    QList<File> m_fileList;

    friend QDebug &operator<<(QDebug &debug, const File &file)
    {
        debug << "WizardFile{"
              << "source:" << file.source
              << "; target:" << file.target
              << "; condition:" << file.condition
              << "; options:" << file.options
              << "}";
        return debug;
    }
};

Result<> JsonWizardFileGenerator::setup(const QVariant &data)
{
    const Result<QVariantList> list = JsonWizardFactory::objectOrList(data);
    if (!list)
        return ResultError(list.error());

    for (const QVariant &d : *list) {
        if (d.typeId() != QMetaType::QVariantMap)
            return ResultError(Tr::tr("Files data list entry is not an object."));

        File f;

        const QVariantMap tmp = d.toMap();
        f.source = FilePath::fromSettings(tmp.value(QLatin1String("source")));
        f.target = FilePath::fromSettings(tmp.value(QLatin1String("target")));
        f.condition = tmp.value(QLatin1String("condition"), true);
        f.isBinary = tmp.value(QLatin1String("isBinary"), false);
        f.overwrite = tmp.value(QLatin1String("overwrite"), false);
        f.openInEditor = tmp.value(QLatin1String("openInEditor"), false);
        f.isTemporary = tmp.value(QLatin1String("temporary"), false);
        f.openAsProject = tmp.value(QLatin1String("openAsProject"), false);

        Result<JsonWizard::OptionDefinitions> res =
            JsonWizard::parseOptions(tmp.value(QLatin1String("options")));
        if (!res)
            return ResultError(res.error());

        f.options = *res;

        if (f.source.isEmpty() && f.target.isEmpty())
            return ResultError(Tr::tr("Source and target are both empty."));

        if (f.target.isEmpty())
            f.target = f.source;

        m_fileList << f;
    }

    return ResultOk;
}

Result<Core::GeneratedFile> JsonWizardFileGenerator::generateFile(const File &file, MacroExpander *expander)
{
    // Read contents of source file
    const Result<QByteArray> contents = file.source.fileContents();
    if (!contents)
        return ResultError(contents.error());

    // Generate file information:
    Core::GeneratedFile gf;
    gf.setFilePath(file.target);

    if (!file.keepExisting) {
        if (file.isBinary.toBool()) {
            gf.setBinary(true);
            gf.setBinaryContents(contents.value());
        } else {
            // TODO: Document that input files are UTF8 encoded!
            gf.setBinary(false);
            MacroExpander nested;

            // evaluate file options once:
            QHash<QString, QString> options;
            for (const JsonWizard::OptionDefinition &od : std::as_const(file.options)) {
                if (od.condition(*expander))
                    options.insert(od.key(), od.value(*expander));
            }

            nested.registerExtraResolver([&options](QString n, QString *ret) -> bool {
                if (!options.contains(n))
                    return false;
                *ret = options.value(n);
                return true;
            });
            nested.registerExtraResolver([expander](QString n, QString *ret) {
                return expander->resolveMacro(n, ret);
            });

            const Result<QString> res =
                    TemplateEngine::processText(&nested,
                                    QString::fromUtf8(normalizeNewlines(contents.value())));
            gf.setContents(res.value_or(QString()));
            if (!res) {
                return ResultError(Tr::tr("When processing \"%1\":<br>%2")
                        .arg(file.source.toUserOutput(), res.error()));
            }
        }
        if (!file.source.isResourceFile()) // resource files mess up permissions, stay with default
            gf.setPermissions(file.source.permissions());
    }

    Core::GeneratedFile::Attributes attributes;
    if (JsonWizard::boolFromVariant(file.openInEditor, expander))
        attributes |= Core::GeneratedFile::OpenEditorAttribute;
    if (JsonWizard::boolFromVariant(file.openAsProject, expander))
        attributes |= Core::GeneratedFile::OpenProjectAttribute;
    if (JsonWizard::boolFromVariant(file.overwrite, expander))
        attributes |= Core::GeneratedFile::ForceOverwrite;
    if (JsonWizard::boolFromVariant(file.isTemporary, expander))
        attributes |= Core::GeneratedFile::TemporaryFile;

    if (file.keepExisting)
        attributes |= Core::GeneratedFile::KeepExistingFileAttribute;

    gf.setAttributes(attributes);
    return gf;
}

Core::GeneratedFiles JsonWizardFileGenerator::fileList(MacroExpander *expander,
                                                       const FilePath &wizardDir, const FilePath &projectDir,
                                                       QString *errorMessage)
{
    errorMessage->clear();

    const QList<File> enabledFiles
            = Utils::filtered(m_fileList, [&expander](const File &f) {
                                  return JsonWizard::boolFromVariant(f.condition, expander);
                              });

    const QList<File> concreteFiles
            = Utils::transform(enabledFiles,
                               [&expander, &wizardDir, &projectDir](const File &f) -> File {
                                  // Return a new file with concrete values based on input file:
                                  File file = f;

                                  file.keepExisting = file.source.isEmpty();
                                  file.target = projectDir.resolvePath(expander->expand(file.target));
                                  file.source = file.keepExisting
                                          ? file.target
                                          : wizardDir.resolvePath(expander->expand(file.source));
                                  file.isBinary = JsonWizard::boolFromVariant(file.isBinary, expander);

                                  return file;
                               });

    QList<File> fileList;
    QList<File> dirList;
    std::tie(fileList, dirList)
            = Utils::partition(concreteFiles, [](const File &f) { return !f.source.isDir(); });

    const QSet<FilePath> knownFiles = Utils::transform<QSet>(fileList, &File::target);

    for (const File &dir : std::as_const(dirList)) {
        FilePath sourceDir(dir.source);
        const FilePaths entries =
                sourceDir.dirEntries(QDir::NoDotAndDotDot | QDir::Files| QDir::Hidden);

        for (const FilePath &entry : entries) {
            const QString relativeFilePath = entry.relativeChildPath(sourceDir).path();
            const FilePath targetPath = dir.target / relativeFilePath;

            if (knownFiles.contains(targetPath))
                continue;

            // initialize each new file with properties (isBinary etc)
            // from the current directory json entry
            File newFile = dir;
            newFile.source = dir.source / relativeFilePath;
            newFile.target = targetPath;
            fileList.append(newFile);
        }
    }

    const Core::GeneratedFiles result
        = Utils::transform(fileList, [this, &expander, &errorMessage](const File &f) {
              const Result<Core::GeneratedFile> file = generateFile(f, expander);
              if (!file)
                  *errorMessage = file.error();
              return file.value_or(Core::GeneratedFile());
          });

    if (Utils::contains(result, [](const Core::GeneratedFile &gf) {
            return gf.filePath().isEmpty();
        }))
        return Core::GeneratedFiles();

    return result;
}

Result<> JsonWizardFileGenerator::writeFile(const JsonWizard *wizard, Core::GeneratedFile *file)
{
    Q_UNUSED(wizard)
    if (file->attributes() & Core::GeneratedFile::KeepExistingFileAttribute)
        return ResultOk;
    return file->write();
}

void setupJsonWizardFileGenerator()
{
    static JsonWizardGeneratorTypedFactory<JsonWizardFileGenerator> theFileGeneratorFactory("File");
}

} // ProjectExplorer
