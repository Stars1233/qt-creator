// Copyright (C) 2019 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "desktoprunconfiguration.h"

#include "buildsystem.h"
#include "deploymentdata.h"
#include "projectexplorerconstants.h"
#include "projectexplorertr.h"
#include "runconfigurationaspects.h"
#include "runcontrol.h"
#include "target.h"

#include <cmakeprojectmanager/cmakeprojectconstants.h>
#include <qbsprojectmanager/qbsprojectmanagerconstants.h>
#include <qmakeprojectmanager/qmakeprojectmanagerconstants.h>

using namespace Utils;
using namespace ProjectExplorer::Constants;

namespace ProjectExplorer::Internal {

enum class Kind { QMake, Qbs, CMake };

template <Kind K>
class DesktopRunConfiguration : public RunConfiguration
{
public:
    DesktopRunConfiguration(BuildConfiguration *bc, Id id)
        : RunConfiguration(bc, id)
    {
        environment.setSupportForBuildEnvironment(bc);

        executable.setDeviceSelector(kit(), ExecutableAspect::RunDevice);

        workingDir.setEnvironment(&environment);

        connect(&useLibraryPaths, &UseLibraryPathsAspect::changed,
                &environment, &EnvironmentAspect::environmentChanged);

        if (HostOsInfo::isMacHost()) {
            connect(&useDyldSuffix, &UseLibraryPathsAspect::changed,
                    &environment, &EnvironmentAspect::environmentChanged);
            environment.addModifier([this](Environment &env) {
                if (useDyldSuffix())
                    env.set(QLatin1String("DYLD_IMAGE_SUFFIX"), QLatin1String("_debug"));
            });
        } else {
            useDyldSuffix.setVisible(false);
        }

        runAsRoot.setVisible(HostOsInfo::isAnyUnixHost());

        environment.addModifier([this](Environment &env) {
            BuildTargetInfo bti = buildTargetInfo();
            if (bti.runEnvModifier) {
                Environment old = env;
                bti.runEnvModifier(env, useLibraryPaths());
                const EnvironmentItems diff = old.diff(env, true);
                for (const EnvironmentItem &i : diff) {
                    switch (i.operation) {
                    case EnvironmentItem::SetEnabled:
                    case EnvironmentItem::Prepend:
                    case EnvironmentItem::Append:
                        env.addItem(std::make_tuple("_QTC_" + i.name, i.value));
                        break;
                    default:
                        break;
                    }
                }
            }
        });

        setUpdater([this] { updateTargetInformation(); });
    }

private:
    void updateTargetInformation();

    FilePath executableToRun(const BuildTargetInfo &targetInfo) const;

    LauncherAspect launcher{this};
    EnvironmentAspect environment{this};
    ExecutableAspect executable{this};
    ArgumentsAspect arguments{this};
    WorkingDirectoryAspect workingDir{this};
    TerminalAspect terminal{this};
    UseDyldSuffixAspect useDyldSuffix{this};
    UseLibraryPathsAspect useLibraryPaths{this};
    RunAsRootAspect runAsRoot{this};
};

template <Kind K>
void DesktopRunConfiguration<K>::updateTargetInformation()
{
    QTC_ASSERT(buildSystem(), return);

    BuildTargetInfo bti = buildTargetInfo();

    auto terminalAspect = aspect<TerminalAspect>();
    terminalAspect->setUseTerminalHint(!bti.targetFilePath.isLocal() ? false : bti.usesTerminal);
    terminalAspect->setEnabled(bti.targetFilePath.isLocal());
    auto launcherAspect = aspect<LauncherAspect>();
    launcherAspect->setVisible(false);

    if constexpr (K == Kind::QMake) {
        FilePath profile = FilePath::fromString(buildKey());
        if (profile.isEmpty())
            setDefaultDisplayName(Tr::tr("Qt Run Configuration"));
        else
            setDefaultDisplayName(profile.completeBaseName());

        emit aspect<EnvironmentAspect>()->environmentChanged();

        auto wda = aspect<WorkingDirectoryAspect>();
        if (!bti.workingDirectory.isEmpty())
            wda->setDefaultWorkingDirectory(bti.workingDirectory);

        aspect<ExecutableAspect>()->setExecutable(bti.targetFilePath);

    } else if constexpr (K == Kind::Qbs) {
        setDefaultDisplayName(bti.displayName);
        const FilePath executable = executableToRun(bti);

        aspect<ExecutableAspect>()->setExecutable(executable);

    } else if constexpr (K == Kind::CMake) {
        if (bti.launchers.size() > 0) {
            launcherAspect->setVisible(true);
            // Use start program by default, if defined (see toBuildTarget() for details)
            launcherAspect->setDefaultLauncher(bti.launchers.last());
            launcherAspect->updateLaunchers(bti.launchers);
        }
        aspect<ExecutableAspect>()->setExecutable(bti.targetFilePath);
        if (!bti.workingDirectory.isEmpty())
            aspect<WorkingDirectoryAspect>()->setDefaultWorkingDirectory(bti.workingDirectory);

        const QStringList argumentsList = bti.additionalData.toMap()["arguments"].toStringList();
        if (!argumentsList.isEmpty())
            aspect<ArgumentsAspect>()->setArguments(
                ProcessArgs::joinArgs(argumentsList, bti.targetFilePath.osType()));

        emit aspect<EnvironmentAspect>()->environmentChanged();
    }
}

template <Kind K>
FilePath DesktopRunConfiguration<K>::executableToRun(const BuildTargetInfo &targetInfo) const
{
    const FilePath appInBuildDir = targetInfo.targetFilePath;
    const DeploymentData deploymentData = buildSystem()->deploymentData();
    if (deploymentData.localInstallRoot().isEmpty())
        return appInBuildDir;

    const QString deployedAppFilePath = deploymentData
            .deployableForLocalFile(appInBuildDir).remoteFilePath();
    if (deployedAppFilePath.isEmpty())
        return appInBuildDir;

    const FilePath appInLocalInstallDir = deploymentData.localInstallRoot() / deployedAppFilePath;
    return appInLocalInstallDir.exists() ? appInLocalInstallDir : appInBuildDir;
}

// Factories

class CMakeRunConfigurationFactory final : public RunConfigurationFactory
{
public:
    CMakeRunConfigurationFactory()
    {
        registerRunConfiguration<DesktopRunConfiguration<Kind::CMake>>(Constants::CMAKE_RUNCONFIG_ID);
        addSupportedProjectType(CMakeProjectManager::Constants::CMAKE_PROJECT_ID);
        addSupportedTargetDeviceType(ProjectExplorer::Constants::DESKTOP_DEVICE_TYPE);
        addSupportedTargetDeviceType(ProjectExplorer::Constants::DOCKER_DEVICE_TYPE);
    }
};

class QbsRunConfigurationFactory final : public RunConfigurationFactory
{
public:
    QbsRunConfigurationFactory()
    {
        registerRunConfiguration<DesktopRunConfiguration<Kind::Qbs>>(Constants::QBS_RUNCONFIG_ID);
        addSupportedProjectType(QbsProjectManager::Constants::PROJECT_ID);
        addSupportedTargetDeviceType(ProjectExplorer::Constants::DESKTOP_DEVICE_TYPE);
        addSupportedTargetDeviceType(ProjectExplorer::Constants::DOCKER_DEVICE_TYPE);
    }
};

class DesktopQmakeRunConfigurationFactory final : public RunConfigurationFactory
{
public:
    DesktopQmakeRunConfigurationFactory()
    {
        registerRunConfiguration<DesktopRunConfiguration<Kind::QMake>>(Constants::QMAKE_RUNCONFIG_ID);
        addSupportedProjectType(QmakeProjectManager::Constants::QMAKEPROJECT_ID);
        addSupportedTargetDeviceType(ProjectExplorer::Constants::DESKTOP_DEVICE_TYPE);
        addSupportedTargetDeviceType(ProjectExplorer::Constants::DOCKER_DEVICE_TYPE);
    }
};

void setupDesktopRunConfigurations()
{
    static DesktopQmakeRunConfigurationFactory theQmakeRunConfigFactory;
    static QbsRunConfigurationFactory theQbsRunConfigFactory;
    static CMakeRunConfigurationFactory theCmakeRunConfigFactory;
}

void setupDesktopRunWorker()
{
    static ProcessRunnerFactory theDesktopRunWorkerFactory({
        Constants::CMAKE_RUNCONFIG_ID,
        Constants::QBS_RUNCONFIG_ID,
        Constants::QMAKE_RUNCONFIG_ID
    });
}

} // ProjectExplorer::Internal
