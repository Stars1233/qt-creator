// Copyright (C) 2025 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once
#include "devcontainer_global.h"
#include "devcontainerconfig.h"

#include <tasking/tasktree.h>

#include <utils/environment.h>
#include <utils/filepath.h>
#include <utils/processinterface.h>
#include <utils/result.h>

#include <memory>

namespace DevContainer {

struct InstancePrivate;

struct DEVCONTAINER_EXPORT InstanceConfig
{
    Utils::FilePath dockerCli;
    Utils::FilePath dockerComposeCli;
    Utils::FilePath workspaceFolder;
    Utils::FilePath configFilePath;

    bool runProcessesInTerminal = false;

    std::vector<std::variant<Mount, QString>> mounts;

    Utils::Environment localEnvironment = Utils::Environment::systemEnvironment();

    using LogFunction = std::function<void(const QString &)>;
    LogFunction logFunction = [](const QString &msg) { qDebug().noquote() << msg; };

    QString jsonToString(const QJsonValue &value) const;
    QString devContainerId() const;
};

struct DEVCONTAINER_EXPORT RunningInstanceData
{
    Utils::OsType osType;
    Utils::OsArch osArch;
    Utils::Environment remoteEnvironment;
};

using RunningInstance = std::shared_ptr<RunningInstanceData>;

class DEVCONTAINER_EXPORT Instance
{
public:
    explicit Instance(Config config, InstanceConfig instanceConfig);
    static Utils::Result<std::unique_ptr<Instance>> fromFile(
        const Utils::FilePath &filePath, InstanceConfig instanceConfig);
    static std::unique_ptr<Instance> fromConfig(
        const Config &config, InstanceConfig instanceConfig = {});

    static Utils::Result<Config> configFromFile(
        const Utils::FilePath &filePath, InstanceConfig instanceConfig);

    ~Instance();

    Utils::Result<> up(const RunningInstance &runningInstance); // Create and start the container
    Utils::Result<> down(); // Stop and remove the container

    Utils::ProcessInterface *createProcessInterface(const RunningInstance &runningInstance) const;

    Utils::Result<Tasking::Group> upRecipe(const RunningInstance &runningInstance) const;
    Utils::Result<Tasking::Group> downRecipe() const;

    const Config &config() const;

private:
    std::unique_ptr<InstancePrivate> d;
};

} // namespace DevContainer
