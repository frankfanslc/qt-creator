/****************************************************************************
**
** Copyright (C) 2019 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "studiowelcomeplugin.h"
#include "examplecheckout.h"

#include "qdsnewdialog.h"

#include <app/app_version.h>

#include <coreplugin/coreconstants.h>
#include <coreplugin/dialogs/restartdialog.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/helpmanager.h>
#include <coreplugin/icore.h>
#include <coreplugin/imode.h>
#include <coreplugin/modemanager.h>

#include <extensionsystem/pluginmanager.h>
#include <extensionsystem/pluginspec.h>

#include <projectexplorer/jsonwizard/jsonwizardfactory.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/projectmanager.h>

#include <qmlprojectmanager/qmlproject.h>

#include <qmldesigner/qmldesignerplugin.h>
#include <qmldesigner/components/componentcore/theme.h>

#include <utils/checkablemessagebox.h>
#include <utils/icon.h>
#include <utils/infobar.h>
#include <utils/stringutils.h>
#include <utils/theme/theme.h>
#include <utils/hostosinfo.h>

#include <QAbstractListModel>
#include <QApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFontDatabase>
#include <QPointer>
#include <QShortcut>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWidget>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace StudioWelcome {
namespace Internal {

static bool useNewWelcomePage()
{
    QSettings *settings = Core::ICore::settings();
    const QString newWelcomePageEntry = "QML/Designer/NewWelcomePage"; //entry from qml settings

    return settings->value(newWelcomePageEntry, false).toBool();
}

const char DO_NOT_SHOW_SPLASHSCREEN_AGAIN_KEY[] = "StudioSplashScreen";

const char DETAILED_USAGE_STATISTICS[] = "DetailedUsageStatistics";
const char STATISTICS_COLLECTION_MODE[] = "StatisticsCollectionMode";
const char NO_TELEMETRY[] = "NoTelemetry";
const char CRASH_REPORTER_SETTING[] = "CrashReportingEnabled";

QPointer<QQuickWidget> s_view = nullptr;
static StudioWelcomePlugin *s_pluginInstance = nullptr;

std::unique_ptr<QSettings> makeUserFeedbackSettings()
{
    QStringList domain = QCoreApplication::organizationDomain().split(QLatin1Char('.'));
    std::reverse(domain.begin(), domain.end());
    QString productId = domain.join(QLatin1String("."));
    if (!productId.isEmpty())
        productId += ".";
    productId += QCoreApplication::applicationName();

    QString organization;
    if (Utils::HostOsInfo::isMacHost()) {
        organization = QCoreApplication::organizationDomain().isEmpty()
                           ? QCoreApplication::organizationName()
                           : QCoreApplication::organizationDomain();
    } else {
        organization = QCoreApplication::organizationName().isEmpty()
                           ? QCoreApplication::organizationDomain()
                           : QCoreApplication::organizationName();
    }

    std::unique_ptr<QSettings> settings(new QSettings(organization, "UserFeedback." + productId));
    settings->beginGroup("UserFeedback");
    return settings;
}

class UsageStatisticPluginModel : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool usageStatisticEnabled MEMBER m_usageStatisticEnabled NOTIFY usageStatisticChanged)
    Q_PROPERTY(bool crashReporterEnabled MEMBER m_crashReporterEnabled NOTIFY crashReporterEnabledChanged)

public:
    explicit UsageStatisticPluginModel(QObject *parent = nullptr)
        : QObject(parent)
    {
        setupModel();
    }

    void setupModel()
    {
        auto settings = makeUserFeedbackSettings();
        QVariant value = settings->value(STATISTICS_COLLECTION_MODE);
        m_usageStatisticEnabled = value.isValid() && value.toString() == DETAILED_USAGE_STATISTICS;

        m_crashReporterEnabled = Core::ICore::settings()->value(CRASH_REPORTER_SETTING, false).toBool();

        emit usageStatisticChanged();
        emit crashReporterEnabledChanged();
    }

    Q_INVOKABLE void setCrashReporterEnabled(bool b)
    {
        if (m_crashReporterEnabled == b)
            return;

        Core::ICore::settings()->setValue(CRASH_REPORTER_SETTING, b);

        s_pluginInstance->pauseRemoveSplashTimer();

        const QString restartText = tr("The change will take effect after restart.");
        Core::RestartDialog restartDialog(Core::ICore::dialogParent(), restartText);
        restartDialog.exec();

        s_pluginInstance->resumeRemoveSplashTimer();
        setupModel();
    }

    Q_INVOKABLE void setTelemetryEnabled(bool b)
    {
        if (m_usageStatisticEnabled == b)
            return;

        auto settings = makeUserFeedbackSettings();

        settings->setValue(STATISTICS_COLLECTION_MODE, b ? DETAILED_USAGE_STATISTICS : NO_TELEMETRY);

        // pause remove splash timer while dialog is open otherwise splash crashes upon removal
        s_pluginInstance->pauseRemoveSplashTimer();

        const QString restartText = tr("The change will take effect after restart.");
        Core::RestartDialog restartDialog(Core::ICore::dialogParent(), restartText);
        restartDialog.exec();

        s_pluginInstance->resumeRemoveSplashTimer();
        setupModel();
    }

signals:
    void usageStatisticChanged();
    void crashReporterEnabledChanged();

private:
    bool m_usageStatisticEnabled = false;
    bool m_crashReporterEnabled = false;
};

class ProjectModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum { FilePathRole = Qt::UserRole + 1, PrettyFilePathRole, PreviewUrl, TagData, Description };

    Q_PROPERTY(bool communityVersion MEMBER m_communityVersion NOTIFY communityVersionChanged)
    Q_PROPERTY(bool enterpriseVersion MEMBER m_enterpriseVersion NOTIFY enterpriseVersionChanged)

    explicit ProjectModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void createProject()
    {
        ProjectExplorer::ProjectExplorerPlugin::openNewProjectDialog();
    }

    Q_INVOKABLE void openProject()
    {
        ProjectExplorer::ProjectExplorerPlugin::openOpenProjectDialog();
    }

    Q_INVOKABLE void openProjectAt(int row)
    {
        const QString projectFile = data(index(row, 0), ProjectModel::FilePathRole).toString();
        if (QFileInfo::exists(projectFile))
            ProjectExplorer::ProjectExplorerPlugin::openProjectWelcomePage(projectFile);
    }

    Q_INVOKABLE int get(int) { return -1; }

    Q_INVOKABLE void showHelp()
    {
        QDesktopServices::openUrl(QUrl("qthelp://org.qt-project.qtcreator/doc/index.html"));
    }

    Q_INVOKABLE void openExample(const QString &example,
                                 const QString &formFile,
                                 const QString &url,
                                 const QString &explicitQmlproject,
                                 const QString &tempFile,
                                 const QString &completeBaseName)
    {
        if (!url.isEmpty()) {
            ExampleCheckout *checkout = new ExampleCheckout;
            checkout->checkoutExample(QUrl::fromUserInput(url), tempFile, completeBaseName);
            connect(checkout,
                    &ExampleCheckout::finishedSucessfully,
                    this,
                    [checkout, formFile, example, explicitQmlproject]() {
                        const QString exampleFolder = checkout->extractionFolder() + "/" + example
                                                      + "/";

                        QString projectFile = exampleFolder + example + ".qmlproject";

                        if (!explicitQmlproject.isEmpty())
                            projectFile = exampleFolder + explicitQmlproject;

                        ProjectExplorer::ProjectExplorerPlugin::openProjectWelcomePage(projectFile);

                        const QString qmlFile = QFileInfo(projectFile).dir().absolutePath() + "/"
                                                + formFile;

                        Core::EditorManager::openEditor(Utils::FilePath::fromString(qmlFile));
                    });
            return;
        }

        const Utils::FilePath projectFile = Core::ICore::resourcePath("examples")
                                            / example / example + ".qmlproject";
        ProjectExplorer::ProjectExplorerPlugin::openProjectWelcomePage(projectFile.toString());
        const Utils::FilePath qmlFile = Core::ICore::resourcePath("examples")
                                            / example / formFile;

        Core::EditorManager::openEditor(qmlFile);
    }
public slots:
    void resetProjects();

signals:
    void communityVersionChanged();
    void enterpriseVersionChanged();

private:
    void setupVersion();

    bool m_communityVersion = true;
    bool m_enterpriseVersion = false;
};

void ProjectModel::setupVersion()
{
    const ExtensionSystem::PluginSpec *pluginSpec = Utils::findOrDefault(
        ExtensionSystem::PluginManager::plugins(),
        Utils::equal(&ExtensionSystem::PluginSpec::name, QString("LicenseChecker")));

    if (!pluginSpec)
        return;

    ExtensionSystem::IPlugin *plugin = pluginSpec->plugin();

    if (!plugin)
        return;

    m_communityVersion = false;

    bool retVal = false;
    bool success = QMetaObject::invokeMethod(plugin,
                                             "qdsEnterpriseLicense",
                                             Qt::DirectConnection,
                                             Q_RETURN_ARG(bool, retVal));

    if (!success) {
        qWarning("Check for Qt Design Studio Enterprise License failed.");
        return;
    }
    if (!retVal) {
        qWarning("No Qt Design Studio Enterprise License. Disabling asset importer.");
        return;
    }
    m_enterpriseVersion = true;
}

ProjectModel::ProjectModel(QObject *parent)
    : QAbstractListModel(parent)
{
    connect(ProjectExplorer::ProjectExplorerPlugin::instance(),
            &ProjectExplorer::ProjectExplorerPlugin::recentProjectsChanged,
            this,
            &ProjectModel::resetProjects);

    setupVersion();
}

int ProjectModel::rowCount(const QModelIndex &) const
{
    return ProjectExplorer::ProjectExplorerPlugin::recentProjects().count();
}

QString getQDSVersion(const QString &projectFilePath)
{
    const QString defaultReturn = "";
    Utils::FileReader reader;
    if (!reader.fetch(Utils::FilePath::fromString(projectFilePath)))
        return defaultReturn;

    const QByteArray data = reader.data();

    QRegularExpression regexp(R"x(qdsVersion: "(.*)")x");
    QRegularExpressionMatch match = regexp.match(QString::fromUtf8(data));

    if (!match.hasMatch())
        return defaultReturn;

    return ProjectModel::tr("Created with Qt Design Studio version: %1").arg(match.captured(1));
}

QString getMainQmlFile(const QString &projectFilePath)
{
    const QString defaultReturn = "content/App.qml";
    Utils::FileReader reader;
    if (!reader.fetch(Utils::FilePath::fromString(projectFilePath)))
            return defaultReturn;

    const QByteArray data = reader.data();

    QRegularExpression regexp(R"x(mainFile: "(.*)")x");
    QRegularExpressionMatch match = regexp.match(QString::fromUtf8(data));

    if (!match.hasMatch())
        return defaultReturn;

    return match.captured(1);
}

QString appQmlFile(const QString &projectFilePath)
{
    return QFileInfo(projectFilePath).dir().absolutePath() + "/" +  getMainQmlFile(projectFilePath);
}

static QString fromCamelCase(const QString &s) {

   const QRegularExpression regExp1 {"(.)([A-Z][a-z]+)"};
   const QRegularExpression regExp2 {"([a-z0-9])([A-Z])"};
   QString result = s;
   result.replace(regExp1, "\\1 \\2");
   result.replace(regExp2, "\\1 \\2");
   result = result.left(1).toUpper() + result.mid(1);
   return result;
}

static QString resolutionFromConstants(const QString &projectFilePath)
{
    const QFileInfo fileInfo(projectFilePath);
    const QString fileName = fileInfo.dir().absolutePath()
            + "/"  + "imports" + "/" + fileInfo.baseName() + "/Constants.qml";

    Utils::FileReader reader;
    if (!reader.fetch(Utils::FilePath::fromString(fileName)))
        return {};

    const QByteArray data = reader.data();

    const QRegularExpression regexpWidth(R"x(readonly\s+property\s+int\s+width:\s+(\d*))x");
    const QRegularExpression regexpHeight(R"x(readonly\s+property\s+int\s+height:\s+(\d*))x");

    int width = -1;
    int height = -1;

    QRegularExpressionMatch match = regexpHeight.match(QString::fromUtf8(data));
    if (match.hasMatch())
        height = match.captured(1).toInt();

    match = regexpWidth.match(QString::fromUtf8(data));
    if (match.hasMatch())
        width = match.captured(1).toInt();

    if (width > 0 && height > 0)
        return ProjectModel::tr("Resolution: %1x%2").arg(width).arg(height);

    return {};
}

static QString description(const QString &projectFilePath)
{

    const QString created = ProjectModel::tr("Created: %1").arg(
            QFileInfo(projectFilePath).fileTime(QFileDevice::FileBirthTime).toString());
    const QString lastEdited =  ProjectModel::tr("Last Edited: %1").arg(
            QFileInfo(projectFilePath).fileTime(QFileDevice::FileModificationTime).toString());

    return fromCamelCase(QFileInfo(projectFilePath).baseName()) + "\n\n" + created + "\n" + lastEdited
            + "\n" + resolutionFromConstants(projectFilePath)
            + "\n" + getQDSVersion(projectFilePath);
}

static QString tags(const QString &projectFilePath)
{
    QStringList ret;
    const QString defaultReturn = "content/App.qml";
    Utils::FileReader reader;
    if (!reader.fetch(Utils::FilePath::fromString(projectFilePath)))
            return defaultReturn;

    const QByteArray data = reader.data();

    bool mcu = data.contains("qtForMCUs: true");

    if (data.contains("qt6Project: true"))
        ret.append("Qt 6");
    else if (mcu)
        ret.append("Qt For MCU");
    else
        ret.append("Qt 5");

    return ret.join(",");
}

QVariant ProjectModel::data(const QModelIndex &index, int role) const
{
    QPair<QString, QString> data = ProjectExplorer::ProjectExplorerPlugin::recentProjects().at(
        index.row());
    switch (role) {
    case Qt::DisplayRole:
        return data.second;
        break;
    case FilePathRole:
        return data.first;
    case PrettyFilePathRole:
        return Utils::withTildeHomePath(data.first);
    case PreviewUrl:
        return QVariant(QStringLiteral("image://project_preview/") + appQmlFile(data.first));
    case TagData:
        return tags(data.first);
    case Description:
        return description(data.first);
    default:
        return QVariant();
    }

    return QVariant();
}

QHash<int, QByteArray> ProjectModel::roleNames() const
{
    QHash<int, QByteArray> roleNames;
    roleNames[Qt::DisplayRole] = "displayName";
    roleNames[FilePathRole] = "filePath";
    roleNames[PrettyFilePathRole] = "prettyFilePath";
    roleNames[PreviewUrl] = "previewUrl";
    roleNames[TagData] = "tagData";
    roleNames[Description] = "description";
    return roleNames;
}

void ProjectModel::resetProjects()
{
    beginResetModel();
    endResetModel();
}

class WelcomeMode : public Core::IMode
{
    Q_OBJECT
public:
    WelcomeMode();
    ~WelcomeMode() override;

private:
    QQuickWidget *m_modeWidget = nullptr;
};

void StudioWelcomePlugin::closeSplashScreen()
{
    if (!s_view.isNull()) {
        const bool doNotShowAgain = s_view->rootObject()->property("doNotShowAgain").toBool();
        if (doNotShowAgain)
            Utils::CheckableMessageBox::doNotAskAgain(Core::ICore::settings(),
                                                      DO_NOT_SHOW_SPLASHSCREEN_AGAIN_KEY);

        s_view->deleteLater();
    }
}

void StudioWelcomePlugin::showSystemSettings()
{
    Core::ICore::infoBar()->removeInfo("WarnCrashReporting");
    Core::ICore::infoBar()->globallySuppressInfo("WarnCrashReporting");

    // pause remove splash timer while settings dialog is open otherwise splash crashes upon removal
    pauseRemoveSplashTimer();
    Core::ICore::showOptionsDialog(Core::Constants::SETTINGS_ID_SYSTEM);
    resumeRemoveSplashTimer();
}

StudioWelcomePlugin::StudioWelcomePlugin()
{
    s_pluginInstance = this;
}

StudioWelcomePlugin::~StudioWelcomePlugin()
{
    delete m_welcomeMode;
}

bool StudioWelcomePlugin::initialize(const QStringList &arguments, QString *errorString)
{
    Q_UNUSED(arguments)
    Q_UNUSED(errorString)

    qmlRegisterType<ProjectModel>("projectmodel", 1, 0, "ProjectModel");
    qmlRegisterType<UsageStatisticPluginModel>("usagestatistics", 1, 0, "UsageStatisticModel");

    m_welcomeMode = new WelcomeMode;

    m_removeSplashTimer.setSingleShot(true);
    const QString splashScreenTimeoutEntry = "QML/Designer/splashScreenTimeout";
    m_removeSplashTimer.setInterval(
        Core::ICore::settings()->value(splashScreenTimeoutEntry, 15000).toInt());
    connect(&m_removeSplashTimer, &QTimer::timeout, this, [this] { closeSplashScreen(); });
    return true;
}

static bool showSplashScreen()
{
    const QString lastQDSVersionEntry = "QML/Designer/lastQDSVersion";

    QSettings *settings = Core::ICore::settings();

    const QString lastQDSVersion = settings->value(lastQDSVersionEntry).toString();


    const QString currentVersion = Core::Constants::IDE_VERSION_DISPLAY;

    if (currentVersion != lastQDSVersion) {
        settings->setValue(lastQDSVersionEntry, currentVersion);
        return true;
    }

    return Utils::CheckableMessageBox::shouldAskAgain(Core::ICore::settings(),
                                                      DO_NOT_SHOW_SPLASHSCREEN_AGAIN_KEY);
}

void StudioWelcomePlugin::extensionsInitialized()
{
    Core::ModeManager::activateMode(m_welcomeMode->id());

    // Enable QDS new project dialog and QDS wizards
    if (QmlProjectManager::QmlProject::isQtDesignStudio()) {
        ProjectExplorer::JsonWizardFactory::clearWizardPaths();
        ProjectExplorer::JsonWizardFactory::addWizardPath(
            Core::ICore::resourcePath("qmldesigner/studio_templates"));

        Core::ICore::setNewDialogFactory([](QWidget *parent) { return new QdsNewDialog(parent); });
    }

    if (showSplashScreen()) {
        connect(Core::ICore::instance(), &Core::ICore::coreOpened, this, [this] {
            s_view = new QQuickWidget(Core::ICore::dialogParent());
            s_view->setResizeMode(QQuickWidget::SizeRootObjectToView);
            s_view->setWindowFlag(Qt::SplashScreen, true);
            s_view->setWindowModality(Qt::ApplicationModal);
            s_view->engine()->addImportPath("qrc:/studiofonts");
#ifdef QT_DEBUG
            s_view->engine()->addImportPath(QLatin1String(STUDIO_QML_PATH) + "splashscreen/imports");
            s_view->setSource(
                QUrl::fromLocalFile(QLatin1String(STUDIO_QML_PATH) + "splashscreen/main.qml"));
#else
            s_view->engine()->addImportPath("qrc:/qml/splashscreen/imports");
            s_view->setSource(QUrl("qrc:/qml/splashscreen/main.qml"));
#endif


            QTC_ASSERT(s_view->rootObject(),
                       qWarning() << "The StudioWelcomePlugin has a runtime depdendency on "
                                     "qt/qtquicktimeline.";
                       return );

            connect(s_view->rootObject(), SIGNAL(closeClicked()), this, SLOT(closeSplashScreen()));
            connect(s_view->rootObject(),
                    SIGNAL(configureClicked()),
                    this,
                    SLOT(showSystemSettings()));

            s_view->show();
            s_view->raise();

            m_removeSplashTimer.start();
        });
    }
}

bool StudioWelcomePlugin::delayedInitialize()
{
    if (s_view.isNull())
        return false;

    QTC_ASSERT(s_view->rootObject(), return true);

#ifdef ENABLE_CRASHPAD
    const bool crashReportingEnabled = true;
    const bool crashReportingOn = Core::ICore::settings()->value(CRASH_REPORTER_SETTING, false).toBool();
#else
    const bool crashReportingEnabled = false;
    const bool crashReportingOn = false;
#endif

    QMetaObject::invokeMethod(s_view->rootObject(), "onPluginInitialized",
            Q_ARG(bool, crashReportingEnabled), Q_ARG(bool, crashReportingOn));

    return false;
}

void StudioWelcomePlugin::pauseRemoveSplashTimer()
{
    if (m_removeSplashTimer.isActive()) {
        m_removeSplashRemainingTime = m_removeSplashTimer.remainingTime(); // milliseconds
        m_removeSplashTimer.stop();
    }
}

void StudioWelcomePlugin::resumeRemoveSplashTimer()
{
    if (!m_removeSplashTimer.isActive())
        m_removeSplashTimer.start(m_removeSplashRemainingTime);
}

WelcomeMode::WelcomeMode()
{
    setDisplayName(tr("Studio"));

    const Utils::Icon FLAT({{":/studiowelcome/images/mode_welcome_mask.png",
                      Utils::Theme::IconsBaseColor}});
    const Utils::Icon FLAT_ACTIVE({{":/studiowelcome/images/mode_welcome_mask.png",
                             Utils::Theme::IconsModeWelcomeActiveColor}});
    setIcon(Utils::Icon::modeIcon(FLAT, FLAT, FLAT_ACTIVE));

    setPriority(Core::Constants::P_MODE_WELCOME);
    setId(Core::Constants::MODE_WELCOME);
    setContextHelp("Qt Design Studio Manual");
    setContext(Core::Context(Core::Constants::C_WELCOME_MODE));

    QFontDatabase::addApplicationFont(":/studiofonts/TitilliumWeb-Regular.ttf");
    ExampleCheckout::registerTypes();

    m_modeWidget = new QQuickWidget;
    m_modeWidget->setMinimumSize(1024, 768);
    m_modeWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    QmlDesigner::Theme::setupTheme(m_modeWidget->engine());
    m_modeWidget->engine()->addImportPath("qrc:/studiofonts");

    QmlDesigner::QmlDesignerPlugin::registerPreviewImageProvider(m_modeWidget->engine());

    m_modeWidget->engine()->setOutputWarningsToStandardError(false);

    if (!useNewWelcomePage()) {

#ifdef QT_DEBUG
        m_modeWidget->engine()->addImportPath(QLatin1String(STUDIO_QML_PATH)
                                              + "welcomepage/imports");
        m_modeWidget->setSource(QUrl::fromLocalFile(QLatin1String(STUDIO_QML_PATH)
                                                    + "welcomepage/main.qml"));
#else
        m_modeWidget->engine()->addImportPath("qrc:/qml/welcomepage/imports");
        m_modeWidget->setSource(QUrl("qrc:/qml/welcomepage/main.qml"));
#endif
    } else {

        m_modeWidget->engine()->addImportPath(Core::ICore::resourcePath("qmldesigner/propertyEditorQmlSources/imports").toString());

        const QString welcomePagePath = Core::ICore::resourcePath("qmldesigner/welcomepage").toString();
        m_modeWidget->engine()->addImportPath(welcomePagePath + "/imports");
        m_modeWidget->setSource(QUrl::fromLocalFile(welcomePagePath + "/main.qml"));

        QShortcut *updateShortcut = nullptr;
        if (Utils::HostOsInfo::isMacHost())
            updateShortcut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_F5), m_modeWidget);
        else
            updateShortcut = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_F5), m_modeWidget);
        connect(updateShortcut, &QShortcut::activated, this, [this, welcomePagePath](){
            m_modeWidget->setSource(QUrl::fromLocalFile(welcomePagePath + "/main.qml"));
        });
    }

    setWidget(m_modeWidget);

    QStringList designStudioQchPathes
        = {Core::HelpManager::documentationPath() + "/qtdesignstudio.qch",
           Core::HelpManager::documentationPath() + "/qtquick.qch",
           Core::HelpManager::documentationPath() + "/qtquickcontrols.qch",
           Core::HelpManager::documentationPath() + "/qtquicktimeline.qch",
           Core::HelpManager::documentationPath() + "/qtquick3d.qch",
           Core::HelpManager::documentationPath() + "/qtqml.qch"};

    Core::HelpManager::registerDocumentation(
                Utils::filtered(designStudioQchPathes,
                                [](const QString &path) { return QFileInfo::exists(path); }));
}

WelcomeMode::~WelcomeMode()
{
    delete m_modeWidget;
}

} // namespace Internal
} // namespace StudioWelcome

#include "studiowelcomeplugin.moc"
