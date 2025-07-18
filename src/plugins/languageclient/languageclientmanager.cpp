// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "languageclientmanager.h"

#include "languageclientsymbolsupport.h"
#include "languageclienttr.h"
#include "locatorfilter.h"

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/editormanager/ieditor.h>
#include <coreplugin/find/searchresultwindow.h>
#include <coreplugin/icore.h>
#include <coreplugin/navigationwidget.h>

#include <extensionsystem/pluginmanager.h>

#include <languageserverprotocol/messages.h>
#include <languageserverprotocol/progresssupport.h>

#include <projectexplorer/buildconfiguration.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/projectmanager.h>

#include <texteditor/ioutlinewidget.h>
#include <texteditor/textdocument.h>
#include <texteditor/texteditor.h>
#include <texteditor/textmark.h>

#include <utils/algorithm.h>
#include <utils/theme/theme.h>
#include <utils/shutdownguard.h>
#include <utils/utilsicons.h>

#include <QTimer>

using namespace ExtensionSystem;
using namespace LanguageServerProtocol;
using namespace ProjectExplorer;

namespace LanguageClient {

static Q_LOGGING_CATEGORY(Log, "qtc.languageclient.manager", QtWarningMsg)

static LanguageClientManager *managerInstance = nullptr;

class LanguageClientManagerPrivate
{
    LanguageCurrentDocumentFilter m_currentDocumentFilter;
    LanguageAllSymbolsFilter m_allSymbolsFilter;
    LanguageClassesFilter m_classFilter;
    LanguageFunctionsFilter m_functionFilter;
};

LanguageClientManager::LanguageClientManager()
{
    setObjectName("LanguageClientManager");

    managerInstance = this;
    d.reset(new LanguageClientManagerPrivate);
    using namespace Core;
    connect(EditorManager::instance(), &EditorManager::editorOpened,
            this, &LanguageClientManager::editorOpened);
    connect(EditorManager::instance(), &EditorManager::documentOpened,
            this, &LanguageClientManager::documentOpened);
    connect(EditorManager::instance(), &EditorManager::documentClosed,
            this, &LanguageClientManager::documentClosed);
    connect(ProjectManager::instance(), &ProjectManager::buildConfigurationAdded,
            this, &LanguageClientManager::buildConfigurationAdded);
    connect(ProjectManager::instance(), &ProjectManager::projectRemoved,
            this, [this](Project *project) { project->disconnect(this); });

    ExtensionSystem::PluginManager::addObject(this);
}

LanguageClientManager::~LanguageClientManager()
{
    ExtensionSystem::PluginManager::removeObject(this);
    QTC_ASSERT(m_clients.isEmpty(), qDeleteAll(m_clients));
    qDeleteAll(m_currentSettings);
    managerInstance = nullptr;
}

void LanguageClient::LanguageClientManager::addClient(Client *client)
{
    QTC_ASSERT(managerInstance, return);
    QTC_ASSERT(client, return);

    if (managerInstance->m_clients.contains(client))
        return;

    qCDebug(Log) << "add client: " << client->name() << client;
    managerInstance->m_clients << client;
    connect(client, &Client::finished, managerInstance, [client]() { clientFinished(client); });
    connect(client,
            &Client::initialized,
            managerInstance,
            [client](const LanguageServerProtocol::ServerCapabilities &capabilities) {
                emit managerInstance->clientInitialized(client);
                managerInstance->m_inspector.clientInitialized(client->name(), capabilities);
            });
    connect(client,
            &Client::capabilitiesChanged,
            managerInstance,
            [client](const DynamicCapabilities &capabilities) {
                managerInstance->m_inspector.updateCapabilities(client->name(), capabilities);
            });
    connect(client,
            &Client::destroyed,
            managerInstance, [client]() {
                QTC_ASSERT(!managerInstance->m_clients.contains(client),
                           managerInstance->m_clients.removeAll(client));
                for (QList<Client *> &clients : managerInstance->m_clientsForSetting) {
                    QTC_CHECK(clients.removeAll(client) == 0);
                }
            });

    Project *project = client->project();

    if (!project)
        project = ProjectManager::startupProject();
    if (project)
        client->updateConfiguration(ProjectSettings(project).workspaceConfiguration());

    emit managerInstance->clientAdded(client);
}

void LanguageClientManager::restartClient(Client *client)
{
    QTC_ASSERT(managerInstance, return);
    if (!client)
        return;
    managerInstance->m_restartingClients.insert(client);
    if (client->reachable())
        client->shutdown();
}

void LanguageClientManager::clientStarted(Client *client)
{
    qCDebug(Log) << "client started: " << client->name() << client;
    QTC_ASSERT(managerInstance, return);
    QTC_ASSERT(client, return);
    if (client->state() != Client::Uninitialized) // do not proceed if we already received an error
        return;
    if (PluginManager::isShuttingDown()) {
        clientFinished(client);
        return;
    }
    client->initialize();
    const QList<TextEditor::TextDocument *> &clientDocs
        = managerInstance->m_clientForDocument.keys(client);
    for (TextEditor::TextDocument *document : clientDocs)
        client->openDocument(document);
}

void LanguageClientManager::clientFinished(Client *client)
{
    QTC_ASSERT(managerInstance, return);

    if (managerInstance->m_restartingClients.remove(client)) {
        client->resetRestartCounter();
        client->reset();
        client->start();
        return;
    }

    constexpr int restartTimeoutS = 5;
    const bool unexpectedFinish = client->state() != Client::Shutdown
                                  && client->state() != Client::ShutdownRequested;

    const QList<TextEditor::TextDocument *> &clientDocs
        = managerInstance->m_clientForDocument.keys(client);
    if (unexpectedFinish) {
        if (!PluginManager::isShuttingDown()) {
            const bool shouldRestart = client->state() > Client::FailedToInitialize
                                       && client->state() != Client::FailedToShutdown;
            if (shouldRestart && client->reset()) {
                qCDebug(Log) << "restart unexpectedly finished client: " << client->name() << client;
                client->log(
                    Tr::tr("Unexpectedly finished. Restarting in %1 seconds.").arg(restartTimeoutS));
                QTimer::singleShot(restartTimeoutS * 1000, client, [client]() { client->start(); });
                for (TextEditor::TextDocument *document : clientDocs) {
                    client->deactivateDocument(document);
                    if (Core::EditorManager::currentEditor()->document() == document)
                        TextEditor::IOutlineWidgetFactory::updateOutline();
                }
                return;
            }
            qCDebug(Log) << "client finished unexpectedly: " << client->name() << client;
            client->log(Tr::tr("Unexpectedly finished."));
        }
    }

    if (unexpectedFinish || !QTC_GUARD(clientDocs.isEmpty())) {
        for (TextEditor::TextDocument *document : clientDocs)
            openDocumentWithClient(document, nullptr);
    }

    deleteClient(client, unexpectedFinish);
    if (isShutdownFinished())
        emit managerInstance->shutdownFinished();
}

Client *LanguageClientManager::startClient(const BaseSettings *setting,
                                           BuildConfiguration *bc)
{
    QTC_ASSERT(managerInstance, return nullptr);
    QTC_ASSERT(setting, return nullptr);
    QTC_ASSERT(setting->isValid(), return nullptr);
    Client *client = setting->createClient(bc);
    QTC_ASSERT(client, return nullptr);
    qCDebug(Log) << "start client: " << client->name() << client;
    client->start();
    managerInstance->m_clientsForSetting[setting->m_id].append(client);
    return client;
}

const QList<Client *> LanguageClientManager::clients()
{
    QTC_ASSERT(managerInstance, return {});
    return managerInstance->m_clients;
}

void LanguageClientManager::shutdownClient(Client *client)
{
    if (!client)
        return;
    qCDebug(Log) << "request client shutdown: " << client->name() << client;
    // reset and deactivate the documents for that client by assigning a null client already when
    // requesting the shutdown so they can get reassigned to another server right after this request
    for (TextEditor::TextDocument *document : managerInstance->m_clientForDocument.keys(client))
        openDocumentWithClient(document, nullptr);
    if (client->reachable())
        client->shutdown();
    else if (client->state() != Client::Shutdown && client->state() != Client::ShutdownRequested)
        deleteClient(client);
}

void LanguageClientManager::deleteClient(Client *client, bool unexpected)
{
    QTC_ASSERT(managerInstance, return);
    QTC_ASSERT(client, return);
    qCDebug(Log) << "delete client: " << client->name() << client;
    client->disconnect(managerInstance);
    managerInstance->m_clients.removeAll(client);
    for (QList<Client *> &clients : managerInstance->m_clientsForSetting)
        clients.removeAll(client);

    // a deleteLater is not sufficient here as it pastes the delete later event at the end
    // of the main event loop and when the plugins are shutdown we spawn an additional eventloop
    // that will not handle the delete later event. Use invokeMethod with Qt::QueuedConnection
    // instead.
    QMetaObject::invokeMethod(client, [client] {delete client;}, Qt::QueuedConnection);
    managerInstance->trackClientDeletion(client);

    if (!PluginManager::isShuttingDown())
        emit instance()->clientRemoved(client, unexpected);
}

void LanguageClientManager::shutdown()
{
    QTC_ASSERT(managerInstance, return);
    qCDebug(Log) << "shutdown manager";
    const auto clients = managerInstance->clients();
    for (Client *client : clients)
        shutdownClient(client);
    QTimer::singleShot(3000, managerInstance, [] {
        const auto clients = managerInstance->clients();
        for (Client *client : clients)
            deleteClient(client);
        emit managerInstance->shutdownFinished();
    });
}

LanguageClientManager *LanguageClientManager::instance()
{
    return managerInstance;
}

QList<Client *> LanguageClientManager::clientsSupportingDocument(
    const TextEditor::TextDocument *doc, bool onlyReachable)
{
    QTC_ASSERT(managerInstance, return {});
    QTC_ASSERT(doc, return {};);
    return Utils::filtered(
        onlyReachable ? managerInstance->reachableClients() : managerInstance->m_clients,
        [doc](Client *client) { return client->isSupportedDocument(doc); });
}

void LanguageClientManager::writeSettings()
{
    // do not write settings before they have been initialized
    QTC_ASSERT(LanguageClientSettings::initialized(), return);
    LanguageClientSettings::toSettings(Core::ICore::settings(), managerInstance->m_currentSettings);
}

void LanguageClientManager::applySettings()
{
    QTC_ASSERT(managerInstance, return);
    qDeleteAll(managerInstance->m_currentSettings);
    managerInstance->m_currentSettings
        = Utils::transform(LanguageClientSettings::pageSettings(), &BaseSettings::copy);
    const QList<BaseSettings *> restarts = LanguageClientSettings::changedSettings();
    writeSettings();

    for (BaseSettings *settings : restarts)
        applySettings(settings);
}

void LanguageClientManager::applySettings(const QString &settingsId)
{
    if (BaseSettings *settings = Utils::findOrDefault(
            LanguageClientSettings::pageSettings(), Utils::equal(&BaseSettings::m_id, settingsId))) {
        applySettings(settings);
    }
}

void LanguageClientManager::applySettings(BaseSettings *setting)
{
    QList<TextEditor::TextDocument *> documents;
    const QList<Client *> currentClients = clientsForSetting(setting);
    for (Client *client : currentClients) {
        documents << managerInstance->m_clientForDocument.keys(client);
        shutdownClient(client);
    }
    for (auto document : std::as_const(documents))
        managerInstance->m_clientForDocument.remove(document);
    if (!setting->isValid())
        return;
    if (setting->m_startBehavior == BaseSettings::AlwaysOn
        || setting->m_startBehavior == BaseSettings::RequiresFile) {
        if (!setting->m_enabled)
            return;
        auto ensureClient = [setting, client = static_cast<Client *>(nullptr)]() mutable {
            if (!client)
                client = startClient(setting);
            return client;
        };
        if (setting->m_startBehavior == BaseSettings::AlwaysOn)
            ensureClient();

        for (TextEditor::TextDocument *previousDocument : std::as_const(documents)) {
            if (setting->m_languageFilter.isSupported(previousDocument)) {
                auto client = ensureClient();
                QTC_ASSERT(client, return);
                openDocumentWithClient(previousDocument, client);
            }
        }
        const QList<Core::IDocument *> &openedDocuments = Core::DocumentModel::openedDocuments();
        for (Core::IDocument *document : openedDocuments) {
            if (documents.contains(document))
                continue; // already handled above
            if (auto textDocument = qobject_cast<TextEditor::TextDocument *>(document)) {
                if (setting->m_languageFilter.isSupported(document)) {
                    auto client = ensureClient();
                    QTC_ASSERT(client, return);
                    client->openDocument(textDocument);
                }
            }
        }
    } else if (setting->m_startBehavior == BaseSettings::RequiresProject) {
        const QList<Core::IDocument *> &openedDocuments = Core::DocumentModel::openedDocuments();
        QHash<Project *, Client *> clientForProject;
        for (Core::IDocument *document : openedDocuments) {
            auto textDocument = qobject_cast<TextEditor::TextDocument *>(document);
            if (!textDocument || !setting->m_languageFilter.isSupported(textDocument))
                continue;
            const Utils::FilePath filePath = textDocument->filePath();
            for (Project *project : ProjectManager::projects()) {
                for (Target *target : project->targets()) {
                    for (BuildConfiguration *bc : target->buildConfigurations()) {
                        if (!setting->isValidOnBuildConfiguration(bc))
                            continue;
                        const bool settingIsEnabled
                            = ProjectSettings(project).enabledSettings().contains(setting->m_id)
                              || (setting->m_enabled
                                  && !ProjectSettings(project).disabledSettings().contains(setting->m_id));
                        if (!settingIsEnabled)
                            continue;
                        if (project->isKnownFile(filePath)) {
                            Client *client = clientForProject[project];
                            if (!client) {
                                client = startClient(setting, bc);
                                if (!client)
                                    continue;
                                clientForProject[project] = client;
                            }
                            client->openDocument(textDocument);
                        }
                    }
                }
            }
        }
    }
}

QList<BaseSettings *> LanguageClientManager::currentSettings()
{
    QTC_ASSERT(managerInstance, return {});
    return managerInstance->m_currentSettings;
}

void LanguageClientManager::registerClientSettings(BaseSettings *settings)
{
    QTC_ASSERT(managerInstance, return);
    LanguageClientSettings::addSettings(settings);
    managerInstance->applySettings();
}

void LanguageClientManager::enableClientSettings(const QString &settingsId, bool enable)
{
    QTC_ASSERT(managerInstance, return);
    LanguageClientSettings::enableSettings(settingsId, enable);
    managerInstance->applySettings();
}

QList<Client *> LanguageClientManager::clientsForSettingId(const QString &settingsId)
{
    QTC_ASSERT(managerInstance, return {});
    auto instance = managerInstance;
    return instance->m_clientsForSetting.value(settingsId);
}

QList<Client *> LanguageClientManager::clientsForSetting(const BaseSettings *setting)
{
    QTC_ASSERT(setting, return {});
    return clientsForSettingId(setting->m_id);
}

const BaseSettings *LanguageClientManager::settingForClient(Client *client)
{
    QTC_ASSERT(managerInstance, return nullptr);
    for (auto it = managerInstance->m_clientsForSetting.cbegin();
         it != managerInstance->m_clientsForSetting.cend(); ++it) {
        const QString &id = it.key();
        for (const Client *settingClient : it.value()) {
            if (settingClient == client) {
                return Utils::findOrDefault(managerInstance->m_currentSettings,
                                            [id](BaseSettings *setting) {
                                                return setting->m_id == id;
                                            });
            }
        }
    }
    return nullptr;
}

QList<Client *> LanguageClientManager::clientsByName(const QString &name)
{
    QTC_ASSERT(managerInstance, return {});

    return Utils::filtered(managerInstance->m_clients, [name](const Client *client) {
        return client->name() == name;
    });
}

void LanguageClientManager::updateWorkspaceConfiguration(const Project *project,
                                                         const QJsonValue &json)
{
    for (Client *client : std::as_const(managerInstance->m_clients)) {
        Project *clientProject = client->project();
        if (!clientProject || clientProject == project)
            client->updateConfiguration(json);
    }
}

Client *LanguageClientManager::clientForDocument(TextEditor::TextDocument *document)
{
    QTC_ASSERT(managerInstance, return nullptr);
    return document == nullptr ? nullptr
                               : managerInstance->m_clientForDocument.value(document).data();
}

Client *LanguageClientManager::clientForFilePath(const Utils::FilePath &filePath)
{
    return clientForDocument(TextEditor::TextDocument::textDocumentForFilePath(filePath));
}

const QList<Client *> LanguageClientManager::clientsForBuildConfiguration(const BuildConfiguration *bc)
{
    return Utils::filtered(managerInstance->m_clients, [bc](const Client *c) {
        return c->buildConfiguration() == bc;
    });
}

void LanguageClientManager::openDocumentWithClient(TextEditor::TextDocument *document, Client *client)
{
    if (!document)
        return;
    Client *currentClient = clientForDocument(document);
    client = client && client->activatable() ? client : nullptr;
    if (client == currentClient)
        return;
    const bool firstOpen = !managerInstance->m_clientForDocument.remove(document);
    if (firstOpen) {
        connect(
            document, &QObject::destroyed, managerInstance, [document, path = document->filePath()] {
                const QPointer<Client> client = managerInstance->m_clientForDocument.take(document);
                QTC_ASSERT(!client, client->hideDiagnostics(path));
            });
    }
    if (currentClient)
        currentClient->deactivateDocument(document);
    managerInstance->m_clientForDocument[document] = client;
    if (client) {
        qCDebug(Log) << "open" << document->filePath() << "with" << client->name() << client;
        if (!client->documentOpen(document))
            client->openDocument(document);
        else
            client->activateDocument(document);
    } else if (Core::EditorManager::currentDocument() == document) {
        TextEditor::IOutlineWidgetFactory::updateOutline();
    }
}

void LanguageClientManager::logJsonRpcMessage(const LspLogMessage::MessageSender sender,
                                              const QString &clientName,
                                              const LanguageServerProtocol::JsonRpcMessage &message)
{
    instance()->m_inspector.log(sender, clientName, message);
}

void LanguageClientManager::showInspector()
{
    QString clientName;
    if (Client *client = clientForDocument(TextEditor::TextDocument::currentTextDocument()))
        clientName = client->name();
    instance()->m_inspector.show(clientName);
}

QList<Client *> LanguageClientManager::reachableClients()
{
    return Utils::filtered(m_clients, &Client::reachable);
}

void LanguageClientManager::editorOpened(Core::IEditor *editor)
{
    using namespace TextEditor;
    using namespace Core;

    if (auto *textEditor = qobject_cast<BaseTextEditor *>(editor)) {
        if (TextEditorWidget *widget = textEditor->editorWidget()) {
            connect(widget, &TextEditorWidget::requestLinkAt, this,
                    [document = textEditor->textDocument()]
                    (const QTextCursor &cursor, const Utils::LinkHandler &callback, bool resolveTarget) {
                        if (auto client = clientForDocument(document)) {
                            client->findLinkAt(document,
                                               cursor,
                                               callback,
                                               resolveTarget,
                                               LinkTarget::SymbolDef);
                        }
                    });
            connect(widget, &TextEditorWidget::requestTypeAt, this,
                    [document = textEditor->textDocument()]
                    (const QTextCursor &cursor, const Utils::LinkHandler &callback, bool resolveTarget) {
                        if (auto client = clientForDocument(document)) {
                            client->findLinkAt(document,
                                               cursor,
                                               callback,
                                               resolveTarget,
                                               LinkTarget::SymbolTypeDef);
                        }
                    });
            connect(widget, &TextEditorWidget::requestUsages, this,
                    [document = textEditor->textDocument()](const QTextCursor &cursor) {
                        if (auto client = clientForDocument(document))
                            client->symbolSupport().findUsages(document, cursor);
                    });
            connect(widget, &TextEditorWidget::requestRename, this,
                    [document = textEditor->textDocument()](const QTextCursor &cursor) {
                        if (auto client = clientForDocument(document))
                            client->symbolSupport().renameSymbol(document, cursor);
                    });
            connect(widget, &TextEditorWidget::requestCallHierarchy, this,
                    [this, document = textEditor->textDocument()]() {
                        if (clientForDocument(document)) {
                            emit openCallHierarchy();
                            NavigationWidget::activateSubWidget(Constants::CALL_HIERARCHY_FACTORY_ID,
                                                                Side::Left);
                        }
                    });
            connect(widget, &TextEditorWidget::cursorPositionChanged, this, [widget]() {
                if (Client *client = clientForDocument(widget->textDocument()))
                    if (client->reachable())
                        client->cursorPositionChanged(widget);
            });
            if (TextEditor::TextDocument *document = textEditor->textDocument()) {
                if (Client *client = m_clientForDocument[document])
                    client->activateEditor(editor);
                else
                    autoSetupLanguageServer(document);
            }
        }
    }
}

static QList<BaseSettings *> sortedSettingsForDocument(Core::IDocument *document)
{
    const QList<BaseSettings *> prefilteredSettings
        = Utils::filtered(LanguageClientManager::currentSettings(), [](BaseSettings *setting) {
              return setting->isValid() && setting->m_enabled;
          });

    const Utils::MimeType mimeType = Utils::mimeTypeForName(document->mimeType());
    if (mimeType.isValid()) {
        QList<BaseSettings *> result;
        // prefer exact mime type matches
        result << Utils::filtered(prefilteredSettings, [mimeType](BaseSettings *setting) {
            return setting->m_languageFilter.mimeTypes.contains(mimeType.name());
        });

        // add filePath matches next
        result << Utils::filtered(prefilteredSettings, [document](BaseSettings *setting) {
            return setting->m_languageFilter.isSupported(document->filePath(), {});
        });

        // add parent mime type matches last
        Utils::visitMimeParents(mimeType, [&](const Utils::MimeType &mt) -> bool {
            result << Utils::filtered(prefilteredSettings, [mt](BaseSettings *setting) {
                return setting->m_languageFilter.mimeTypes.contains(mt.name());
            });
            return true; // continue
        });
        return Utils::filteredUnique(result);
    }

    return Utils::filtered(prefilteredSettings, [document](BaseSettings *setting) {
        return setting->m_languageFilter.isSupported(document);
    });
}

void LanguageClientManager::documentOpenedForProject(
    TextEditor::TextDocument *textDocument, BaseSettings *setting, const QList<Client *> &clients)
{
    const Utils::FilePath &filePath = textDocument->filePath();
    for (Project *project : ProjectManager::projects()) {
        // check whether file is part of this project
        if (!project->isKnownFile(filePath) && !filePath.isChildOf(project->projectDirectory()))
            continue;
        for (Target *target : project->targets()) {
            const bool activateDocument = project->activeTarget() == target;
            for (BuildConfiguration *bc : target->buildConfigurations()) {
                // check whether we already have a client running for this project
                Client *clientForBc
                    = Utils::findOrDefault(clients, Utils::equal(&Client::buildConfiguration, bc));

                // create a client only when valid on the current project
                if (!clientForBc) {
                    if (!setting->isValidOnBuildConfiguration(bc))
                        continue;
                    clientForBc = startClient(setting, bc);
                }

                QTC_ASSERT(clientForBc, continue);
                if (activateDocument && clientForBc->activatable()
                    && target->activeBuildConfiguration() == bc) {
                    openDocumentWithClient(textDocument, clientForBc);
                } else
                    clientForBc->openDocument(textDocument);
            }
        }
    }
}

void LanguageClientManager::documentOpened(Core::IDocument *document)
{
    auto textDocument = qobject_cast<TextEditor::TextDocument *>(document);
    if (!textDocument)
        return;

    // check whether we have to start servers for this document
    const QList<BaseSettings *> settings = sortedSettingsForDocument(document);
    QList<Client *> allClients;
    for (BaseSettings *setting : settings) {
        const QList<Client *> clients = clientsForSetting(setting);
        switch (setting->m_startBehavior) {
        case BaseSettings::RequiresProject: {
            documentOpenedForProject(textDocument, setting, clients);
            break;
        }
        case BaseSettings::RequiresFile: {
            if (clients.isEmpty()) {
                Client *client = startClient(setting);
                QTC_ASSERT(client, break);
                allClients << client;
            } else {
                allClients << clients;
            }
            break;
        }
        case BaseSettings::AlwaysOn:
            allClients << clients;
            break;
        case BaseSettings::LastSentinel:
            break;
        }
    }

    for (auto client : std::as_const(allClients)) {
        if (m_clientForDocument[textDocument] || !client->activatable())
            client->openDocument(textDocument);
        else
            openDocumentWithClient(textDocument, client);
    }
}

void LanguageClientManager::documentClosed(Core::IDocument *document)
{
    openDocumentWithClient(qobject_cast<TextEditor::TextDocument *>(document), nullptr);
}

void LanguageClientManager::updateProject(BuildConfiguration *bc)
{
    for (BaseSettings *setting : std::as_const(m_currentSettings)) {
        if (setting->isValid()
            && setting->m_enabled
            && setting->m_startBehavior == BaseSettings::RequiresProject) {
            if (Utils::findOrDefault(clientsForSetting(setting),
                                     [bc](const QPointer<Client> &client) {
                                     return client->buildConfiguration() == bc;
                                     })
                == nullptr) {
                Client *newClient = nullptr;
                const QList<Core::IDocument *> &openedDocuments = Core::DocumentModel::openedDocuments();
                for (Core::IDocument *doc : openedDocuments) {
                    if (setting->m_languageFilter.isSupported(doc)
                            && bc->project()->isKnownFile(doc->filePath())) {
                        if (auto textDoc = qobject_cast<TextEditor::TextDocument *>(doc)) {
                            if (!newClient)
                                newClient = startClient(setting, bc);
                            if (!newClient)
                                break;
                            newClient->openDocument(textDoc);
                        }
                    }
                }
            }
        }
    }
}

void LanguageClientManager::buildConfigurationAdded(BuildConfiguration *bc)
{
    connect(
        bc->project(),
        &ProjectExplorer::Project::fileListChanged,
        this,
        [this, bc = QPointer<BuildConfiguration>(bc)] {
            if (bc)
                updateProject(bc);
        });
    const QList<Client *> &clients = reachableClients();
    for (Client *client : clients)
        client->buildConfigurationOpened(bc);
}

void LanguageClientManager::trackClientDeletion(Client *client)
{
    QTC_ASSERT(!m_scheduledForDeletion.contains(client->id()), return);
    m_scheduledForDeletion.insert(client->id());
    connect(client, &QObject::destroyed, this, [this, id = client->id()] {
        m_scheduledForDeletion.remove(id);
        if (isShutdownFinished())
            emit shutdownFinished();
    });
}

bool LanguageClientManager::isShutdownFinished()
{
    if (!PluginManager::isShuttingDown())
        return false;
    QTC_ASSERT(managerInstance, return true);
    return managerInstance->m_clients.isEmpty()
           && managerInstance->m_scheduledForDeletion.isEmpty();
}

void setupLanguageClientManager()
{
    static Utils::GuardedObject theLanguageClientManager{new LanguageClientManager};
}

} // namespace LanguageClient
