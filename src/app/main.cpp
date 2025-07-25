// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "../tools/qtcreatorcrashhandler/crashhandlersetup.h"

#include <app/app_version.h>
#include <extensionsystem/iplugin.h>
#include <extensionsystem/pluginerroroverview.h>
#include <extensionsystem/pluginmanager.h>
#include <extensionsystem/pluginspec.h>
#include <qtsingleapplication.h>

#include <utils/algorithm.h>
#include <utils/appinfo.h>
#include <utils/aspects.h>
#include <utils/environment.h>
#include <utils/fileutils.h>
#include <utils/fsengine/fsengine.h>
#include <utils/hostosinfo.h>
#include <utils/plaintextedit/plaintexteditaccessibility.h>
#include <utils/processreaper.h>
#include <utils/qtcsettings.h>
#include <utils/qtcsettings_p.h>
#include <utils/stylehelper.h>
#include <utils/temporarydirectory.h>
#include <utils/terminalcommand.h>
#include <utils/textcodec.h>

#include <QAccessible>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLibraryInfo>
#include <QMessageBox>
#include <QNetworkProxyFactory>
#include <QPixmapCache>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QStyle>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QThreadPool>
#include <QTranslator>

#include <iterator>
#include <optional>
#include <string>
#include <vector>

#ifdef ENABLE_CRASHPAD
#define NOMINMAX
#include "client/crash_report_database.h"
#include "client/crashpad_client.h"
#include "client/crashpad_info.h"
#include "client/settings.h"
#endif

#ifdef ENABLE_SENTRY
#include <sentry.h>

Q_LOGGING_CATEGORY(sentryLog, "qtc.sentry", QtWarningMsg)
#endif

using namespace ExtensionSystem;
using namespace Utils;
using namespace Utils::Internal;

enum { OptionIndent = 4, DescriptionIndent = 34 };

const char corePluginIdC[] = "core";
const char fixedOptionsC[]
    = " [OPTION]... [FILE]...\n"
      "Options:\n"
      "    -help                         Display this help\n"
      "    -version                      Display program version\n"
      "    -client                       Attempt to connect to already running first instance\n"
      "    -clientid                     A postfix for the ID used by -client\n"
      "    -settingspath <path>          Override the default path where user settings are stored\n"
      "    -installsettingspath <path>   Override the default path from where user-independent "
      "settings are read\n"
      "    -temporarycleansettings, -tcs Use clean settings for debug or testing reasons\n"
      "    -pid <pid>                    Attempt to connect to instance given by pid\n"
      "    -block                        Block until editor is closed\n"
      "    -pluginpath <path>            Add a custom search path for plugins\n"
      "    -language <locale>            Set the UI language\n";

const char HELP_OPTION1[] = "-h";
const char HELP_OPTION2[] = "-help";
const char HELP_OPTION3[] = "/h";
const char HELP_OPTION4[] = "--help";
const char VERSION_OPTION[] = "-version";
const char VERSION_OPTION2[] = "--version";
const char CLIENT_OPTION[] = "-client";
const char CLIENTID_OPTION[] = "-clientid";
const char SETTINGS_OPTION[] = "-settingspath";
const char INSTALL_SETTINGS_OPTION[] = "-installsettingspath";
const char TEST_OPTION[] = "-test";
const char STYLE_OPTION[] = "-style";
const char QML_LITE_DESIGNER_OPTION[] = "-qml-lite-designer";
const char TEMPORARY_CLEAN_SETTINGS1[] = "-temporarycleansettings";
const char TEMPORARY_CLEAN_SETTINGS2[] = "-tcs";
const char PID_OPTION[] = "-pid";
const char BLOCK_OPTION[] = "-block";
const char PLUGINPATH_OPTION[] = "-pluginpath";
const char LANGUAGE_OPTION[] = "-language";
const char USER_LIBRARY_PATH_OPTION[] = "-user-library-path"; // hidden option for qtcreator.sh

// Helpers for displaying messages. Note that there is no console on Windows.

// Format as <pre> HTML
static inline QString toHtml(const QString &t)
{
    QString res = t;
    res.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    res.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    res.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    res.insert(0, QLatin1String("<html><pre>"));
    res.append(QLatin1String("</pre></html>"));
    return res;
}

static void displayHelpText(const QString &t)
{
    if (HostOsInfo::isWindowsHost() && qApp)
        QMessageBox::information(nullptr, QLatin1String(Core::Constants::IDE_DISPLAY_NAME), toHtml(t));
    else
        printf("%s", qPrintable(t));
}

static void displayError(const QString &t)
{
    if (HostOsInfo::isWindowsHost() && qApp)
        QMessageBox::critical(nullptr, QLatin1String(Core::Constants::IDE_DISPLAY_NAME), t);
    else
        qCritical("%s", qPrintable(t));
}

static void printVersion(const PluginSpec *coreplugin)
{
    QString version;
    QTextStream str(&version);
    str << '\n' << Core::Constants::IDE_DISPLAY_NAME << ' ' << coreplugin->version()<< " based on Qt " << qVersion() << "\n\n";
    PluginManager::formatPluginVersions(str);
    str << '\n' << coreplugin->copyright() << '\n';
    displayHelpText(version);
}

static void printHelp(const QString &a0)
{
    QString help;
    QTextStream str(&help);
    str << "Usage: " << a0 << fixedOptionsC;
    PluginManager::formatOptions(str, OptionIndent, DescriptionIndent);
    PluginManager::formatPluginOptions(str, OptionIndent, DescriptionIndent);
    displayHelpText(help);
}

QString applicationDirPath(char *arg = nullptr)
{
    static QString dir;

    if (arg)
        dir = QFileInfo(QString::fromLocal8Bit(arg)).dir().absolutePath();

    if (QCoreApplication::instance())
        return QApplication::applicationDirPath();

    return dir;
}

static QString resourcePath()
{
    return QDir::cleanPath(applicationDirPath() + '/' + RELATIVE_DATA_PATH);
}

static inline QString msgCoreLoadFailure(const QString &why)
{
    return QCoreApplication::translate("Application", "Failed to load core: %1").arg(why);
}

static inline int askMsgSendFailed()
{
    return QMessageBox::question(nullptr, QApplication::translate("Application","Could not send message"),
                QCoreApplication::translate("Application", "Unable to send command line arguments "
                                            "to the already running instance. It does not appear to "
                                            "be responding. Do you want to start a new instance of "
                                            "%1?").arg(Core::Constants::IDE_DISPLAY_NAME),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Retry,
                QMessageBox::Retry);
}

static inline FilePaths getPluginPaths()
{
    FilePaths rc;
    rc << appInfo().plugins << appInfo().luaPlugins << appInfo().userLuaPlugins;

    const auto version = [](int micro) {
        return QString("%1.%2.%3").arg(IDE_VERSION_MAJOR).arg(IDE_VERSION_MINOR).arg(micro);
    };

    const int minPatchVersion = qMin(
        IDE_VERSION_RELEASE,
        QVersionNumber::fromString(Core::Constants::IDE_VERSION_COMPAT).microVersion());

    // Local plugin path: <localappdata>/plugins/<ideversion>
    //    where <localappdata> is e.g.
    //    "%LOCALAPPDATA%\QtProject\qtcreator" on Windows Vista and later
    //    "$XDG_DATA_HOME/data/QtProject/qtcreator" or "~/.local/share/data/QtProject/qtcreator" on Linux
    //    "~/Library/Application Support/QtProject/Qt Creator" on Mac
    const FilePath userPluginPath = appInfo().userPluginsRoot.parentDir();

    // Qt Creator X.Y.Z can load plugins from X.Y.(Z-1) etc, so add current and previous
    // patch versions
    for (int patchVersion = IDE_VERSION_RELEASE; patchVersion >= minPatchVersion; --patchVersion)
        rc << userPluginPath / version(patchVersion);
    return rc;
}

// Returns plugin path that is set in install settings.
// The installer (or rather the packaging) can write that to load optional plugins from
// outside the application bundle on macOS, because installing optional plugins into
// the application bundle would break code signing.
static QStringList getInstallPluginPaths()
{
    // uses SystemScope because this really must be an "installation" setting
    QSettings installSettings(QSettings::IniFormat,
                              QSettings::SystemScope,
                              QLatin1String(Core::Constants::IDE_SETTINGSVARIANT_STR),
                              QLatin1String(Core::Constants::IDE_CASED_ID));
    return Utils::transform(installSettings.value("Settings/InstallPluginPaths").toStringList(),
                            [](const QString &path) -> QString {
                                if (QDir::isRelativePath(path))
                                    return applicationDirPath() + '/' + path;
                                return path;
                            });
}

static void setupInstallSettings(QString &installSettingspath, bool redirect = true)
{
    if (!installSettingspath.isEmpty() && !QFileInfo(installSettingspath).isDir()) {
        displayError(QString("-installsettingspath \"%0\" needs to be the path where a %1/%2.ini exist.").arg(installSettingspath,
            QLatin1String(Core::Constants::IDE_SETTINGSVARIANT_STR), QLatin1String(Core::Constants::IDE_CASED_ID)));
        installSettingspath.clear();
    }
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::SystemScope,
                       installSettingspath.isEmpty() ? resourcePath() : installSettingspath);
    if (!redirect) // ignore redirection via Settings/InstallSettings
        return;

    // Check if the default install settings contain a setting for the actual install settings.
    // This can be an absolute path, or a path relative to applicationDirPath().
    // The result is interpreted like -settingspath, but for SystemScope.
    //
    // Through the sdktool split that is upcoming, the new install settings might redirect
    // yet a second time. So try this a few times.
    // (Only the first time with QSettings::UserScope, to allow setting the install settings path
    // in the user settings.)
    static const char kInstallSettingsKey[] = "Settings/InstallSettings";
    QSettings::Scope scope = QSettings::UserScope;
    int count = 0;
    bool containsInstallSettingsKey = false;
    do {
        QSettings installSettings(QSettings::IniFormat, scope,
                                  QLatin1String(Core::Constants::IDE_SETTINGSVARIANT_STR),
                                  QLatin1String(Core::Constants::IDE_CASED_ID));
        containsInstallSettingsKey = installSettings.contains(kInstallSettingsKey);
        if (containsInstallSettingsKey) {
            QString newInstallSettingsPath = installSettings.value(kInstallSettingsKey).toString();
            if (QDir::isRelativePath(newInstallSettingsPath))
                newInstallSettingsPath = applicationDirPath() + '/' + newInstallSettingsPath;
            QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, newInstallSettingsPath);
        }
        scope = QSettings::SystemScope; // UserScope only the first time we check
        ++count;
    } while (containsInstallSettingsKey && count < 3);
}

static void setupAccessibility()
{
    QAccessible::installFactory(&accessiblePlainTextEditFactory);
}

static QtcSettings *createUserSettings()
{
    return new QtcSettings(QSettings::IniFormat,
                           QSettings::UserScope,
                           QLatin1String(Core::Constants::IDE_SETTINGSVARIANT_STR),
                           QLatin1String(Core::Constants::IDE_CASED_ID));
}

static void setHighDpiEnvironmentVariable()
{
    if (StyleHelper::defaultHighDpiScaleFactorRoundingPolicy()
            == Qt::HighDpiScaleFactorRoundingPolicy::Unset
        || qEnvironmentVariableIsSet(StyleHelper::C_QT_SCALE_FACTOR_ROUNDING_POLICY))
        return;

    std::unique_ptr<QtcSettings> settings(createUserSettings());

    using Policy = Qt::HighDpiScaleFactorRoundingPolicy;
    const Policy defaultPolicy = StyleHelper::defaultHighDpiScaleFactorRoundingPolicy();
    const Policy userPolicy = settings->value("Core/HighDpiScaleFactorRoundingPolicy",
                                              int(defaultPolicy)).value<Policy>();
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(userPolicy);
}

static void setRHIOpenGLVariable()
{
    QSettings installSettings(
        QSettings::IniFormat,
        QSettings::SystemScope,
        QLatin1String(Core::Constants::IDE_SETTINGSVARIANT_STR),
        QLatin1String(Core::Constants::IDE_CASED_ID));

    const QVariant value = installSettings.value("Core/RhiBackend");
    if (value.isValid())
        qputenv("QSG_RHI_BACKEND", value.toByteArray());
}

void setPixmapCacheLimit()
{
    const int originalLimit = QPixmapCache::cacheLimit();
    const qreal dpr = qApp->devicePixelRatio();
    const qreal multiplier = std::clamp(dpr * dpr, 1.0, 4.0);
    QPixmapCache::setCacheLimit(originalLimit * multiplier);
}

void loadFonts()
{
    const QDir dir(resourcePath() + "/fonts/");

    const QFileInfoList fonts = dir.entryInfoList(QStringList("*.ttf"), QDir::Files);
    for (const QFileInfo &fileInfo : fonts)
        QFontDatabase::addApplicationFont(fileInfo.absoluteFilePath());
}

struct Options
{
    QString settingsPath;
    QString installSettingsPath;
    QStringList customPluginPaths;
    QString uiLanguage;
    QString singleAppIdPostfix;
    // list of arguments that were handled and not passed to the application or plugin manager
    QStringList preAppArguments;
    // list of arguments to be passed to the application or plugin manager
    std::vector<char *> appArguments;
    std::optional<QString> userLibraryPath;
    bool hasTestOption = false;
    bool wantsCleanSettings = false;
    bool hasStyleOption = false;
};

Options parseCommandLine(int argc, char *argv[])
{
    Options options;
    auto it = argv;
    const auto end = argv + argc;
    while (it != end) {
        const auto arg = QString::fromLocal8Bit(*it);
        const bool hasNext = it + 1 != end;
        const auto nextArg = hasNext ? QString::fromLocal8Bit(*(it + 1)) : QString();

        if (arg == SETTINGS_OPTION && hasNext) {
            ++it;
            options.settingsPath = QDir::fromNativeSeparators(nextArg);
            options.preAppArguments << arg << nextArg;
        } else if (arg == INSTALL_SETTINGS_OPTION && hasNext) {
            ++it;
            options.installSettingsPath = QDir::fromNativeSeparators(nextArg);
            options.preAppArguments << arg << nextArg;
        } else if (arg == PLUGINPATH_OPTION && hasNext) {
            ++it;
            options.customPluginPaths += QDir::fromNativeSeparators(nextArg);
            options.preAppArguments << arg << nextArg;
        } else if (arg == LANGUAGE_OPTION && hasNext) {
            ++it;
            options.uiLanguage = nextArg;
            options.preAppArguments << arg << nextArg;
        } else if (arg == USER_LIBRARY_PATH_OPTION && hasNext) {
            ++it;
            options.userLibraryPath = nextArg;
            options.preAppArguments << arg << nextArg;
        } else if (arg == TEMPORARY_CLEAN_SETTINGS1 || arg == TEMPORARY_CLEAN_SETTINGS2) {
            options.wantsCleanSettings = true;
            options.preAppArguments << arg;
        } else if (arg == CLIENTID_OPTION && hasNext) {
            ++it;
            options.singleAppIdPostfix = nextArg;
            options.preAppArguments << arg << nextArg;
        } else { // arguments that are still passed on to the application
            if (arg == STYLE_OPTION)
                options.hasStyleOption = true;
            if (arg == TEST_OPTION)
                options.hasTestOption = true;
            if (arg == QML_LITE_DESIGNER_OPTION)
                options.singleAppIdPostfix = QML_LITE_DESIGNER_OPTION;
            options.appArguments.push_back(*it);
        }
        ++it;
    }

    return options;
}

class Restarter
{
public:
    Restarter(int argc, char *argv[])
    {
        Q_UNUSED(argc)
        m_executable = QString::fromLocal8Bit(argv[0]);
        m_workingPath = QDir::currentPath();
    }

    void setArguments(const QStringList &args) { m_args = args; }

    QString executable() const { return m_executable; }
    QStringList arguments() const { return m_args; }
    QString workingPath() const { return m_workingPath; }

    int restartOrExit(int exitCode)
    {
        return qApp->property("restart").toBool() ? restart(exitCode) : exitCode;
    }

    int restart(int exitCode)
    {
        QProcess::startDetached(m_executable, m_args, m_workingPath);
        return exitCode;
    }

private:
    QString m_executable;
    QStringList m_args;
    QString m_workingPath;
};

QStringList lastSessionArgument()
{
    // using insider information here is not particularly beautiful, anyhow
    const bool hasProjectExplorer = PluginManager::specExists("projectexplorer");
    return hasProjectExplorer ? QStringList({"-lastsession"}) : QStringList();
}

#ifdef ENABLE_CRASHPAD
void startCrashpad(const AppInfo &appInfo, bool crashReportingEnabled)
{
    if (!crashReportingEnabled)
        return;

    using namespace crashpad;

    // Cache directory that will store crashpad information and minidumps
    const QString databasePath = appInfo.crashReports.path();
    const QString handlerPath = (appInfo.libexec / "crashpad_handler").path();
#ifdef Q_OS_WIN
    base::FilePath database(databasePath.toStdWString());
    base::FilePath handler(HostOsInfo::withExecutableSuffix(handlerPath).toStdWString());
#elif defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
    base::FilePath database(databasePath.toStdString());
    base::FilePath handler(HostOsInfo::withExecutableSuffix(handlerPath).toStdString());
#endif

    std::unique_ptr<CrashReportDatabase> db = CrashReportDatabase::Initialize(database);
    if (db && db->GetSettings())
        db->GetSettings()->SetUploadsEnabled(crashReportingEnabled);

    // URL used to submit minidumps to
    std::string url(CRASHPAD_BACKEND_URL);

    // Optional annotations passed via --annotations to the handler
    std::map<std::string, std::string> annotations;
    annotations["app-version"] = Core::Constants::IDE_VERSION_DISPLAY;
    annotations["qt-version"] = QT_VERSION_STR;
#ifdef IDE_REVISION
    annotations["sha1"] = Core::Constants::IDE_REVISION_STR;
#endif

    CrashpadInfo::GetCrashpadInfo()->set_crashpad_handler_behavior(crashpad::TriState::kEnabled);
    if (HostOsInfo::isWindowsHost()) {
        // reduces the size of crash reports, which can be large on Windows
        CrashpadInfo::GetCrashpadInfo()
            ->set_gather_indirectly_referenced_memory(crashpad::TriState::kEnabled, 0);
    }

    // Explicitly enable Crashpad handling. This is the default in vanilla Crashpad,
    // but we use a version that only handles processes that enable it explicitly,
    // so we do not handle arbitrary subprocesses
    CrashpadInfo::GetCrashpadInfo()->set_crashpad_handler_behavior(crashpad::TriState::kEnabled);

    // Optional arguments to pass to the handler
    std::vector<std::string> arguments;
    arguments.push_back("--no-rate-limit");

    CrashpadClient *client = new CrashpadClient();
    client->StartHandler(
        handler,
        database,
        database,
        url,
        annotations,
        arguments,
        /* restartable */ true,
        /* asynchronous_start */ true);
}
#endif

#ifdef ENABLE_SENTRY
void configureSentry(const AppInfo &appInfo, bool crashReportingEnabled)
{
    if (!crashReportingEnabled)
        return;

    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options, SENTRY_DSN);
#ifdef Q_OS_WIN
    sentry_options_set_database_pathw(options, appInfo.crashReports.nativePath().toStdWString().c_str());
#else
    sentry_options_set_database_path(options, appInfo.crashReports.nativePath().toUtf8().constData());
#endif
#ifdef SENTRY_CRASHPAD_PATH
    if (const FilePath handlerpath = appInfo.libexec / "crashpad_handler"; handlerpath.exists()) {
        sentry_options_set_handler_path(options, handlerpath.nativePath().toUtf8().constData());
    } else if (const auto fallback = FilePath::fromString(SENTRY_CRASHPAD_PATH); fallback.exists()) {
        sentry_options_set_handler_path(options, fallback.nativePath().toUtf8().constData());
    } else {
        qCWarning(sentryLog) << "Failed to find crashpad_handler for Sentry crash reports.";
    }
#endif
    const QString release = QString(SENTRY_PROJECT) + "@" + QCoreApplication::applicationVersion();
    sentry_options_set_release(options, release.toUtf8().constData());
    sentry_options_set_debug(options, sentryLog().isDebugEnabled() ? 1 : 0);
    sentry_init(options);
}
#endif

class ShowInGuiHandler
{
public:
    ShowInGuiHandler()
    {
        instance = this;
        oldHandler = qInstallMessageHandler(log);
    }
    ~ShowInGuiHandler() { qInstallMessageHandler(oldHandler); };

private:
    static void log(QtMsgType type, const QMessageLogContext &context, const QString &msg)
    {
        instance->messages += msg;
        if (type == QtFatalMsg) {
            // Show some kind of GUI with collected messages before exiting.
            // For Windows, Qt already uses a dialog.
            if (HostOsInfo::isLinuxHost()) {
#if (QT_VERSION >= QT_VERSION_CHECK(6, 6, 0) && QT_VERSION < QT_VERSION_CHECK(6, 6, 1))
                // Information about potentially missing libxcb-cursor0 is printed by Qt since Qt 6.5.3 and Qt 6.6.1
                // Add it manually for other versions >= 6.5.0
                instance->messages.prepend("From 6.5.0, xcb-cursor0 or libxcb-cursor0 is needed to "
                                           "load the Qt xcb platform plugin.");
#endif
                if (QFile::exists("/usr/bin/xmessage"))
                    QProcess::startDetached("/usr/bin/xmessage", {instance->messages.join("\n")});
            } else if (HostOsInfo::isMacHost()) {
                QProcess::startDetached("/usr/bin/osascript",
                                        {"-e",
                                         "display dialog \""
                                             + instance->messages.join("\n").replace("\"", "\\\"")
                                             + "\" buttons \"OK\" with title \""
                                             + Core::Constants::IDE_DISPLAY_NAME
                                             + " Failed to Start\""});
            }
        }
        instance->oldHandler(type, context, msg);
    };

    static ShowInGuiHandler *instance;
    QStringList messages;
    QtMessageHandler oldHandler = nullptr;
};

ShowInGuiHandler *ShowInGuiHandler::instance = nullptr;

FilePath userPluginsRoot()
{
    FilePath rootPath = FilePath::fromUserInput(
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));

    if (HostOsInfo::isAnyUnixHost() && !HostOsInfo::isMacHost())
        rootPath /= "data";

    rootPath /= Core::Constants::IDE_SETTINGSVARIANT_STR;

    rootPath /= QLatin1StringView(
        HostOsInfo::isMacHost() ? Core::Constants::IDE_DISPLAY_NAME : Core::Constants::IDE_ID);
    rootPath /= "plugins";

    rootPath /= Core::Constants::IDE_VERSION_LONG;

    return rootPath;
}

FilePath userResourcePath(const QString &settingsPath, const QString &appId)
{
    const FilePath configDir = FilePath::fromUserInput(settingsPath).parentDir();
    const FilePath urp = configDir / appId;

    if (!urp.exists()) {
        if (!urp.createDir())
            qWarning() << "could not create" << urp;
    }

    return urp;
}

int main(int argc, char **argv)
{
    Restarter restarter(argc, argv);
    Environment::systemEnvironment(); // cache system environment before we do any changes

    FSEngine fileSystemEngine;

    // Manually determine various command line options
    // We can't use the regular way of the plugin manager,
    // because settings can change the way plugin manager behaves
    Options options = parseCommandLine(argc, argv);
    applicationDirPath(argv[0]);

    // Remove entries from environment variables that were set up by Qt Creator to run
    // the application (in this case, us).
    // TODO: We should be able to merge at least some of the stuff below with similar intent
    //       into a more generalized version of this.
    EnvironmentItems specialItems;
    EnvironmentItems diff;
    Environment::systemEnvironment().forEachEntry(
        [&specialItems](const QString &name, const QString &value, bool enabled) {
            if (enabled && name.startsWith("_QTC_"))
                specialItems.emplaceBack(name, value, EnvironmentItem::SetEnabled);
        });
    for (const EnvironmentItem &item : std::as_const(specialItems)) {
        const QString varName = item.name.mid(5);
        const FilePaths addedPaths
            = Environment::pathListFromValue(item.value, HostOsInfo::hostOs());
        FilePaths allPaths = Environment::systemEnvironment().pathListValue(varName);
        Utils::erase(allPaths, [&addedPaths](const FilePath &p) {
            return addedPaths.contains(p);
        });
        diff.emplaceBack(
            varName,
            Environment::valueFromPathList(allPaths, HostOsInfo::hostOs()),
            EnvironmentItem::SetEnabled);
        diff.emplaceBack(item.name, "", EnvironmentItem::Unset);
    }
    Environment::modifySystemEnvironment(diff);

    if (qEnvironmentVariableIsSet("QTC_DO_NOT_PROPAGATE_LD_PRELOAD"))
        Environment::modifySystemEnvironment({{"LD_PRELOAD", "", EnvironmentItem::Unset}});

    auto restoreEnvVarFromSquish = [](const QByteArray &squishVar, const QString &var) {
        if (qEnvironmentVariableIsSet(squishVar)) {
            Environment::modifySystemEnvironment({{var, "", EnvironmentItem::Unset}});
            const QString content = qEnvironmentVariable(squishVar);
            if (!content.isEmpty())
                Environment::modifySystemEnvironment({{var, content, EnvironmentItem::Prepend}});
        }
    };

    restoreEnvVarFromSquish("SQUISH_SHELL_ORIG_DYLD_LIBRARY_PATH", "DYLD_LIBRARY_PATH");
    restoreEnvVarFromSquish("SQUISH_ORIG_DYLD_FRAMEWORK_PATH", "DYLD_FRAMEWORK_PATH");

    if (options.userLibraryPath) {
        if ((*options.userLibraryPath).isEmpty()) {
            Environment::modifySystemEnvironment(
                {{"LD_LIBRARY_PATH", "", EnvironmentItem::Unset}});
        } else {
            Environment::modifySystemEnvironment(
                {{"LD_LIBRARY_PATH", *options.userLibraryPath, EnvironmentItem::SetEnabled}});
        }
    }

    if (HostOsInfo::isMacHost()) {
        QSurfaceFormat surfaceFormat;
        surfaceFormat.setStencilBufferSize(8);
        surfaceFormat.setDepthBufferSize(24);
        surfaceFormat.setVersion(4, 1);
        surfaceFormat.setProfile(QSurfaceFormat::CoreProfile);
        QSurfaceFormat::setDefaultFormat(surfaceFormat);
    }

    if (qEnvironmentVariableIsSet("QTCREATOR_DISABLE_NATIVE_MENUBAR")
            || qgetenv("XDG_CURRENT_DESKTOP").startsWith("Unity")) {
        QApplication::setAttribute(Qt::AA_DontUseNativeMenuBar);
    }

#if defined(QTC_FORCE_XCB)
    if (HostOsInfo::isLinuxHost() && !qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        // Enforce XCB on Linux/Gnome, if the user didn't override via QT_QPA_PLATFORM
        // This was previously done in Qt, but removed in Qt 6.3. We found that bad things can still happen,
        // like the Wayland session simply crashing when starting Qt Creator.
        // TODO: Reconsider when Qt/Wayland is reliably working on the supported distributions
        const bool hasWaylandDisplay = qEnvironmentVariableIsSet("WAYLAND_DISPLAY");
        const bool isWaylandSessionType = qgetenv("XDG_SESSION_TYPE") == "wayland";
        const QByteArray currentDesktop = qgetenv("XDG_CURRENT_DESKTOP").toLower();
        const QByteArray sessionDesktop = qgetenv("XDG_SESSION_DESKTOP").toLower();
        const bool isGnome = currentDesktop.contains("gnome") || sessionDesktop.contains("gnome");
        const bool isWayland = hasWaylandDisplay || isWaylandSessionType;
        if (isGnome && isWayland) {
            qInfo() << "Warning: Ignoring WAYLAND_DISPLAY on Gnome."
                    << "Use QT_QPA_PLATFORM=wayland to run on Wayland anyway.";
            qputenv("QT_QPA_PLATFORM", "xcb");
        }
    }
#endif

    TemporaryDirectory::setMasterTemporaryDirectory(QDir::tempPath() + "/" + Core::Constants::IDE_CASED_ID + "-XXXXXX");

#ifdef Q_OS_MACOS
    // increase the number of file that can be opened in Qt Creator.
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);

    rl.rlim_cur = qMin((rlim_t)OPEN_MAX, rl.rlim_max);
    setrlimit(RLIMIT_NOFILE, &rl);
#endif

    QScopedPointer<TemporaryDirectory> temporaryCleanSettingsDir;
    if (options.settingsPath.isEmpty() && (options.hasTestOption || options.wantsCleanSettings)) {
        temporaryCleanSettingsDir.reset(new TemporaryDirectory("qtc-test-settings"));
        if (!temporaryCleanSettingsDir->isValid())
            return 1;
        options.settingsPath = temporaryCleanSettingsDir->path().path();
    }
    if (!options.settingsPath.isEmpty())
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, options.settingsPath);

    // Must be done before any QSettings class is created
    QSettings::setDefaultFormat(QSettings::IniFormat);

    // HiDPI variables need to be set before creating QApplication.
    // Since we do not have a QApplication yet, we cannot rely on QApplication::applicationDirPath()
    // though. So we set up install settings with a educated guess here, and re-setup it later.
    setupInstallSettings(options.installSettingsPath);
    setupAccessibility();
    setHighDpiEnvironmentVariable();
    setRHIOpenGLVariable();

    SharedTools::QtSingleApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    int numberOfArguments = static_cast<int>(options.appArguments.size());

    // create a custom Qt message handler that shows messages in a bare bones UI
    // if creation of the QGuiApplication fails.
    auto handler = std::make_unique<ShowInGuiHandler>();
    const QString singleAppId = QString(Core::Constants::IDE_DISPLAY_NAME)
                                + options.singleAppIdPostfix;
    std::unique_ptr<SharedTools::QtSingleApplication> appPtr(
        SharedTools::createApplication(singleAppId, numberOfArguments, options.appArguments.data()));
    handler.reset();
    SharedTools::QtSingleApplication &app = *appPtr;
    QCoreApplication::setApplicationName(Core::Constants::IDE_CASED_ID);
    QCoreApplication::setApplicationVersion(QLatin1String(Core::Constants::IDE_VERSION_LONG));
    QCoreApplication::setOrganizationName(QLatin1String(Core::Constants::IDE_SETTINGSVARIANT_STR));
    QGuiApplication::setApplicationDisplayName(Core::Constants::IDE_DISPLAY_NAME);

    const QScopeGuard cleanup([] { ProcessReaper::deleteAll(); });

    const QStringList pluginArguments = app.arguments();

    // Re-setup install settings with QApplication::applicationDirPath() available, but
    // first read install plugin paths from original install settings, without redirection
    setupInstallSettings(options.installSettingsPath, /*redirect=*/false);
    const QStringList installPluginPaths = getInstallPluginPaths();
    // Re-setup install settings for real
    setupInstallSettings(options.installSettingsPath);
    QtcSettings *userSettings = createUserSettings();
    QtcSettings *installSettings
        = new QtcSettings(QSettings::IniFormat,
                          QSettings::SystemScope,
                          QLatin1String(Core::Constants::IDE_SETTINGSVARIANT_STR),
                          QLatin1String(Core::Constants::IDE_CASED_ID));
    // warn if -installsettings points to a place where no install settings are located
    if (!options.installSettingsPath.isEmpty() && !QFileInfo::exists(installSettings->fileName())) {
        displayError(QLatin1String("The install settings \"%1\" do not exist. The %2 option must "
                                   "point to a path with existing settings, excluding the %3 part "
                                   "of the path.")
                         .arg(QDir::toNativeSeparators(installSettings->fileName()),
                              INSTALL_SETTINGS_OPTION,
                              Core::Constants::IDE_SETTINGSVARIANT_STR));
    }

    SettingsSetup::setupSettings(userSettings, installSettings);

    setPixmapCacheLimit();
    loadFonts();

    if (Utils::HostOsInfo::isWindowsHost() && !options.hasStyleOption) {
        // The Windows 11 default style (Qt 6.7) has major issues, therefore
        // set the previous default style: "windowsvista"
        // FIXME: check newer Qt Versions
        QApplication::setStyle(QLatin1String("windowsvista"));

        // On scaling different than 100% or 200% use the "fusion" style
        qreal tmp;
        const bool fractionalDpi = !qFuzzyIsNull(std::modf(qApp->devicePixelRatio(), &tmp));
        if (fractionalDpi)
            QApplication::setStyle(QLatin1String("fusion"));
    }

    const int threadCount = QThreadPool::globalInstance()->maxThreadCount();
    QThreadPool::globalInstance()->setMaxThreadCount(qMax(4, 2 * threadCount));

    using namespace Core;
    const FilePath appDirPath = FilePath::fromUserInput(QApplication::applicationDirPath());
    AppInfo info;
    info.author = Constants::IDE_AUTHOR;
    info.copyright = Constants::IDE_COPYRIGHT;
    info.displayVersion = Constants::IDE_VERSION_DISPLAY;
    info.id = Constants::IDE_ID;
    info.revision = Constants::IDE_REVISION_STR;
    info.revisionUrl = Constants::IDE_REVISION_URL;
    info.userFileExtension = Constants::IDE_PROJECT_USER_FILE_EXTENSION;
    info.plugins = (appDirPath / RELATIVE_PLUGIN_PATH).cleanPath();
    info.userPluginsRoot = userPluginsRoot();
    info.resources = (appDirPath / RELATIVE_DATA_PATH).cleanPath();
    info.userResources = userResourcePath(userSettings->fileName(), Constants::IDE_ID);
    info.libexec = (appDirPath / RELATIVE_LIBEXEC_PATH).cleanPath();
    // sync with src\tools\qmlpuppet\qmlpuppet\qmlpuppet.cpp -> QString crashReportsPath()
    info.crashReports = info.userResources / "crashpad_reports";
    info.luaPlugins = info.resources / "lua-plugins";
    info.userLuaPlugins = info.userResources / "lua-plugins";
    Utils::Internal::setAppInfo(info);

    // Display a backtrace once a serious signal is delivered (Linux only).
    CrashHandlerSetup setupCrashHandler(
        Core::Constants::IDE_DISPLAY_NAME, CrashHandlerSetup::EnableRestart, info.libexec.path());

    // depends on AppInfo and QApplication being created
    const bool crashReportingEnabled = userSettings->value("CrashReportingEnabled", false).toBool();

#if defined(ENABLE_CRASHPAD)
    startCrashpad(info, crashReportingEnabled);
#elif defined(ENABLE_SENTRY)
    configureSentry(info, crashReportingEnabled);
#else
    Q_UNUSED(crashReportingEnabled)
#endif

    PluginManager pluginManager;
    PluginManager::setPluginIID(QLatin1String("org.qt-project.Qt.QtCreatorPlugin"));
    PluginManager::startProfiling();

    QTranslator translator;
    QTranslator qtTranslator;
    QStringList uiLanguages = QLocale::system().uiLanguages();
    const QString overrideLanguage = options.hasTestOption
                                         ? QString("C") // force built-in when running tests
                                         : userSettings->value("General/OverrideLanguage").toString();
    if (!overrideLanguage.isEmpty())
        uiLanguages.prepend(overrideLanguage);
    if (!options.uiLanguage.isEmpty())
        uiLanguages.prepend(options.uiLanguage);
    const QString &creatorTrPath = resourcePath() + "/translations";
    for (QString locale : std::as_const(uiLanguages)) {
        locale = QLocale(locale).name();
        if (translator.load("qtcreator_" + locale, creatorTrPath)) {
            const QString &qtTrPath = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
            const QString &qtTrFile = QLatin1String("qt_") + locale;
            // Binary installer puts Qt tr files into creatorTrPath
            if (qtTranslator.load(qtTrFile, qtTrPath) || qtTranslator.load(qtTrFile, creatorTrPath)) {
                app.installTranslator(&translator);
                app.installTranslator(&qtTranslator);
                app.setProperty("qtc_locale", locale);
                break;
            }
            Q_UNUSED(translator.load(QString())); // unload()
        } else if (
            locale == QLatin1String("C") /* overrideLanguage == "English" */
            || locale.startsWith(QLatin1String("en")) /* "English" is built-in */) {
            // Load any spelling fixes that might have been done temporarily after string freeze,
            // for the rest use the built-in source text
            if (translator.load("qtcreator_en", creatorTrPath))
                app.installTranslator(&translator);
            break;
        }
    }

    QByteArray overrideCodecForLocale = userSettings->value("General/OverrideCodecForLocale").toByteArray();
    if (!overrideCodecForLocale.isEmpty())
        TextEncoding::setEncodingForLocale(overrideCodecForLocale);

    app.setDesktopFileName(IDE_APP_ID);

    // Make sure we honor the system's proxy settings
    QNetworkProxyFactory::setUseSystemConfiguration(true);

    PluginManager::removePluginsAfterRestart();

    // We need to install plugins before we scan for them.
    PluginManager::installPluginsAfterRestart();

    // Load
    const QStringList pluginPaths = installPluginPaths + options.customPluginPaths;
    PluginManager::setPluginPaths(
        getPluginPaths() + Utils::transform(pluginPaths, &FilePath::fromUserInput));

    QMap<QString, QString> foundAppOptions;
    if (pluginArguments.size() > 1) {
        QMap<QString, bool> appOptions;
        appOptions.insert(QLatin1String(HELP_OPTION1), false);
        appOptions.insert(QLatin1String(HELP_OPTION2), false);
        appOptions.insert(QLatin1String(HELP_OPTION3), false);
        appOptions.insert(QLatin1String(HELP_OPTION4), false);
        appOptions.insert(QLatin1String(VERSION_OPTION), false);
        appOptions.insert(QLatin1String(VERSION_OPTION2), false);
        appOptions.insert(QLatin1String(CLIENT_OPTION), false);
        appOptions.insert(QLatin1String(PID_OPTION), true);
        appOptions.insert(QLatin1String(BLOCK_OPTION), false);
        if (Result<> res = PluginManager::parseOptions(pluginArguments, appOptions, &foundAppOptions); !res) {
            displayError(res.error());
            printHelp(QFileInfo(app.applicationFilePath()).baseName());
            return -1;
        }
    }
    restarter.setArguments(options.preAppArguments + PluginManager::argumentsForRestart()
                           + lastSessionArgument());
    // if settingspath is not provided we need to pass on the settings in use
    const QString settingspath = options.preAppArguments.contains(QLatin1String(SETTINGS_OPTION))
            ? QString() : options.settingsPath;
    const PluginManager::ProcessData processData = { restarter.executable(),
            options.preAppArguments + PluginManager::argumentsForRestart(), restarter.workingPath(),
            settingspath};
    PluginManager::setCreatorProcessData(processData);

    PluginSpec *coreplugin = PluginManager::specById(QLatin1String(corePluginIdC));
    if (!coreplugin) {
        QString nativePaths = QDir::toNativeSeparators(pluginPaths.join(QLatin1Char(',')));
        const QString reason = QCoreApplication::translate("Application", "Could not find Core plugin in %1").arg(nativePaths);
        displayError(msgCoreLoadFailure(reason));
        return 1;
    }
    if (!coreplugin->isEffectivelyEnabled()) {
        const QString reason = QCoreApplication::translate("Application", "Core plugin is disabled.");
        displayError(msgCoreLoadFailure(reason));
        return 1;
    }
    if (coreplugin->hasError()) {
        displayError(msgCoreLoadFailure(coreplugin->errorString()));
        return 1;
    }
    if (foundAppOptions.contains(QLatin1String(VERSION_OPTION))
            || foundAppOptions.contains(QLatin1String(VERSION_OPTION2))) {
        printVersion(coreplugin);
        return 0;
    }
    if (foundAppOptions.contains(QLatin1String(HELP_OPTION1))
            || foundAppOptions.contains(QLatin1String(HELP_OPTION2))
            || foundAppOptions.contains(QLatin1String(HELP_OPTION3))
            || foundAppOptions.contains(QLatin1String(HELP_OPTION4))) {
        printHelp(QFileInfo(app.applicationFilePath()).baseName());
        return 0;
    }

    qint64 pid = -1;
    if (foundAppOptions.contains(QLatin1String(PID_OPTION))) {
        QString pidString = foundAppOptions.value(QLatin1String(PID_OPTION));
        bool pidOk;
        qint64 tmpPid = pidString.toInt(&pidOk);
        if (pidOk)
            pid = tmpPid;
    }

    bool isBlock = foundAppOptions.contains(QLatin1String(BLOCK_OPTION));
    if (app.isRunning() && (pid != -1 || isBlock
                            || foundAppOptions.contains(QLatin1String(CLIENT_OPTION)))) {
        app.setBlock(isBlock);
        if (app.sendMessage(PluginManager::serializedArguments(), 5000 /*timeout*/, pid))
            return 0;

        // Message could not be send, maybe it was in the process of quitting
        if (app.isRunning(pid)) {
            // Nah app is still running, ask the user
            int button = askMsgSendFailed();
            while (button == QMessageBox::Retry) {
                if (app.sendMessage(PluginManager::serializedArguments(), 5000 /*timeout*/, pid))
                    return 0;
                if (!app.isRunning(pid)) // App quit while we were trying so start a new creator
                    button = QMessageBox::Yes;
                else
                    button = askMsgSendFailed();
            }
            if (button == QMessageBox::No)
                return -1;
        }
    }

    PluginManager::checkForProblematicPlugins();
    PluginManager::loadPlugins();
    if (coreplugin->hasError()) {
        displayError(msgCoreLoadFailure(coreplugin->errorString()));
        return 1;
    }

    // Set up remote arguments.
    QObject::connect(&app, &SharedTools::QtSingleApplication::messageReceived,
                     &pluginManager, &PluginManager::remoteArguments);

    // shutdown plugin manager on the exit
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &pluginManager, [] {
        PluginManager::shutdown();
        SettingsSetup::destroySettings();
    });

    if (Utils::HostOsInfo::isWindowsHost()) {
        // Workaround for QTBUG-130696 and QTCREATORBUG-31890
        QApplication::setEffectEnabled(Qt::UI_FadeMenu, false);

        // Disable menu animation which just looks bad
        QApplication::setEffectEnabled(Qt::UI_AnimateMenu, false);
    }

    const int exitCode = restarter.restartOrExit(app.exec());
#ifdef ENABLE_SENTRY
    sentry_close();
#endif
    return exitCode;
}
