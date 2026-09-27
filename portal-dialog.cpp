#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QScreen>
#include <QUrl>
#include <QUuid>
#include <QWindow>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>

// LD_PRELOAD is inherited by child processes. Snipaste launches its Wayland
// clipboard helper (wlhelper), which must not receive this QFileDialog hook or
// its QtWidgets dependency. Remove only this library from the environment as
// soon as the hook has been loaded; the main process keeps the hook in memory.
__attribute__((constructor)) static void removeSelfFromPreload()
{
    const char *value = std::getenv("LD_PRELOAD");
    if (!value || !*value) return;

    static constexpr char self[] = "libsnipaste-portal.so";
    std::string remaining;
    const char *cursor = value;
    while (*cursor) {
        while (*cursor == ':' || *cursor == ' ' || *cursor == '\t') ++cursor;
        if (!*cursor) break;
        const char *end = cursor;
        while (*end && *end != ':' && *end != ' ' && *end != '\t') ++end;
        const std::size_t length = static_cast<std::size_t>(end - cursor);
        const bool isSelf = length >= sizeof(self) - 1
            && std::memcmp(cursor + length - (sizeof(self) - 1), self,
                           sizeof(self) - 1) == 0;
        if (!isSelf) {
            if (!remaining.empty()) remaining.push_back(':');
            remaining.append(cursor, length);
        }
        cursor = end;
    }

    if (remaining.empty()) ::unsetenv("LD_PRELOAD");
    else ::setenv("LD_PRELOAD", remaining.c_str(), 1);
}

struct Pattern { uint kind; QString value; };
using Patterns = QList<Pattern>;
struct Filter { QString label; Patterns patterns; };
using Filters = QList<Filter>;
Q_DECLARE_METATYPE(Pattern)
Q_DECLARE_METATYPE(Patterns)
Q_DECLARE_METATYPE(Filter)
Q_DECLARE_METATYPE(Filters)

QDBusArgument &operator<<(QDBusArgument &a, const Pattern &p)
{
    a.beginStructure(); a << p.kind << p.value; a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, Pattern &p)
{
    a.beginStructure(); a >> p.kind >> p.value; a.endStructure(); return a;
}
QDBusArgument &operator<<(QDBusArgument &a, const Filter &f)
{
    a.beginStructure(); a << f.label << f.patterns; a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, Filter &f)
{
    a.beginStructure(); a >> f.label >> f.patterns; a.endStructure(); return a;
}

static const QString service = QStringLiteral("org.freedesktop.portal.Desktop");
static const QString requestInterface = QStringLiteral("org.freedesktop.portal.Request");

class Response final : public QObject {
    Q_OBJECT
public:
    QEventLoop loop;
    bool finished = false;
    uint code = 2;
    QVariantMap results;
public slots:
    void receive(uint response, const QVariantMap &values)
    {
        code = response;
        results = values;
        finished = true;
        loop.quit();
    }
};

struct Choice {
    bool handled = false;
    QString path;
    QString filter;
};

class PortalWindowGuard final {
    struct Window {
        QPointer<QWidget> widget;
        QPointer<QWindow> native;
        bool enabled;
        bool hiddenOverlay;
        bool wasOnTop;
    };
    QList<Window> windows;
    QPointer<QWidget> active = QApplication::activeWindow();
    QPointer<QWidget> mouseGrabber = QWidget::mouseGrabber();
    QPointer<QWidget> keyboardGrabber = QWidget::keyboardGrabber();
    bool quitting = false;
    QObject connections;
    QPointer<QWidget> parentWindow;
public:
    explicit PortalWindowGuard(QWidget *parent)
    {
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, &connections,
                         [this] { quitting = true; });
        parentWindow = parent ? parent->window() : active.data();
        while (parentWindow && (parentWindow->windowType() == Qt::Popup
                               || parentWindow->windowType() == Qt::ToolTip)) {
            parentWindow = parentWindow->parentWidget()
                ? parentWindow->parentWidget()->window() : nullptr;
        }
        // Only fullscreen capture surfaces need hiding; a pin must stay mapped.
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *native = widget->windowHandle();
            if (!widget->isVisible() || !native || !native->isVisible()) continue;
            if (widget->windowType() == Qt::Popup || widget->windowType() == Qt::ToolTip)
                continue;
            const bool fullscreen = native->windowStates().testFlag(Qt::WindowFullScreen);
            const bool bypassScreen = native->flags().testFlag(Qt::BypassWindowManagerHint)
                && native->screen() && native->geometry().contains(native->screen()->geometry());
            const bool overlay = fullscreen || bypassScreen;
            windows.append({widget, native, widget->isEnabled(), overlay,
                            native->flags().testFlag(Qt::WindowStaysOnTopHint)});
            if (!parentWindow && !overlay) parentWindow = widget;
        }
        if (mouseGrabber) mouseGrabber->releaseMouse();
        if (keyboardGrabber) keyboardGrabber->releaseKeyboard();
        for (const auto &window : windows) {
            if (window.widget && window.enabled) window.widget->setEnabled(false);
            if (!window.native) continue;
            if (window.hiddenOverlay) window.native->hide();
            // QWindow flags update X11 properties in place. QWidget::setWindowFlags
            // would hide/recreate the widget and can terminate a pin's lifecycle.
            else if (window.wasOnTop) window.native->setFlag(Qt::WindowStaysOnTopHint, false);
        }
        QGuiApplication::sync();
    }

    QString parentId() const
    {
        if (QGuiApplication::platformName() != QStringLiteral("xcb") || !parentWindow)
            return {};
        const auto *native = parentWindow->windowHandle();
        if (!native || !native->isVisible()) return {};
        return QStringLiteral("x11:") + QString::number(parentWindow->winId(), 16);
    }

    ~PortalWindowGuard()
    {
        for (const auto &window : windows) {
            if (!window.widget) continue;
            if (window.enabled) window.widget->setEnabled(true);
            // Do not resurrect windows closed by application callbacks while waiting.
            if (!quitting && window.widget && window.widget->isVisible() && window.native) {
                if (window.hiddenOverlay) window.native->show();
                else if (window.wasOnTop)
                    window.native->setFlag(Qt::WindowStaysOnTopHint, true);
            }
        }
        if (!quitting && active && active->isVisible()) active->activateWindow();
        if (!quitting && mouseGrabber && mouseGrabber->isVisible()
            && mouseGrabber->isEnabled() && !QWidget::mouseGrabber())
            mouseGrabber->grabMouse();
        if (!quitting && keyboardGrabber && keyboardGrabber->isVisible()
            && keyboardGrabber->isEnabled() && !QWidget::keyboardGrabber())
            keyboardGrabber->grabKeyboard();
    }
};

static Choice choose(QWidget *parent, const QString &caption, const QString &initial,
                     const QString &nameFilters, const QString *selectedFilter,
                     bool save, bool directory)
{
    static bool choosing = false;
    if (choosing) return {true, {}, {}};
    QScopedValueRollback<bool> choosingGuard(choosing, true);
    static const bool registered = [] {
        qDBusRegisterMetaType<Pattern>();
        qDBusRegisterMetaType<Patterns>();
        qDBusRegisterMetaType<Filter>();
        qDBusRegisterMetaType<Filters>();
        return true;
    }();
    Q_UNUSED(registered);
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return {};

    const QString token = QStringLiteral("snipaste_")
        + QUuid::createUuid().toString(QUuid::Id128);
    QString sender = bus.baseService().mid(1);
    sender.replace('.', '_');
    QString requestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/")
        + sender + '/' + token;
    Response response;
    if (!bus.connect(service, requestPath, requestInterface, QStringLiteral("Response"),
                     &response, SLOT(receive(uint,QVariantMap)))) return {};

    QDBusServiceWatcher watcher(service, bus, QDBusServiceWatcher::WatchForUnregistration);
    QObject::connect(&watcher, &QDBusServiceWatcher::serviceUnregistered,
                     &response.loop, &QEventLoop::quit);

    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    options.insert(QStringLiteral("modal"), true);
    if (!save) {
        options.insert(QStringLiteral("multiple"), false);
        options.insert(QStringLiteral("directory"), directory);
    }

    const QFileInfo info(initial.isEmpty() ? QDir::currentPath() : initial);
    const QString folder = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    QByteArray folderBytes = QFile::encodeName(folder);
    folderBytes.append('\0');
    options.insert(QStringLiteral("current_folder"), folderBytes);
    if (save && !info.isDir()) {
        options.insert(QStringLiteral("current_name"), info.fileName());
        if (info.exists()) {
            QByteArray pathBytes = QFile::encodeName(info.absoluteFilePath());
            pathBytes.append('\0');
            options.insert(QStringLiteral("current_file"), pathBytes);
        }
    }

    Filters filters;
    QStringList originals;
    const auto parts = nameFilters.split(nameFilters.contains(QStringLiteral(";;"))
        ? QStringLiteral(";;") : QStringLiteral("\n"), Qt::SkipEmptyParts);
    const QRegularExpression format(QStringLiteral("^(.*?)\\s*\\(([^()]*)\\)\\s*$"));
    for (const QString &part : parts) {
        const auto match = format.match(part);
        const QString patterns = match.hasMatch() ? match.captured(2) : part;
        Filter filter{match.hasMatch() ? match.captured(1).trimmed() : part, {}};
        if (filter.label.isEmpty()) filter.label = part;
        for (const QString &pattern : patterns.split(QRegularExpression(QStringLiteral("\\s+")),
                                                     Qt::SkipEmptyParts)) {
            filter.patterns.append({0, pattern});
        }
        if (!filter.patterns.isEmpty()) {
            filters.append(filter);
            originals.append(part);
        }
    }
    if (!directory && !filters.isEmpty()) {
        options.insert(QStringLiteral("filters"), QVariant::fromValue(filters));
        const int index = selectedFilter ? originals.indexOf(*selectedFilter) : -1;
        options.insert(QStringLiteral("current_filter"),
                       QVariant::fromValue(filters.at(index < 0 ? 0 : index)));
    }

    QPointer<QWidget> window = parent ? parent->window() : nullptr;
    PortalWindowGuard windowGuard(parent);
    const QString parentId = windowGuard.parentId();
    auto message = QDBusMessage::createMethodCall(service,
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.FileChooser"),
        save ? QStringLiteral("SaveFile") : QStringLiteral("OpenFile"));
    message << parentId << caption << options;
    const QDBusReply<QDBusObjectPath> reply = bus.call(message, QDBus::Block, 30000);
    if (!reply.isValid()) {
        qWarning("[snipaste-portal] %s", qPrintable(reply.error().message()));
        return {};
    }
    // Subscribe before the call above so a fast Response cannot be lost.
    if (reply.value().path() != requestPath) {
        bus.disconnect(service, requestPath, requestInterface, QStringLiteral("Response"),
                       &response, SLOT(receive(uint,QVariantMap)));
        requestPath = reply.value().path();
        if (!bus.connect(service, requestPath, requestInterface, QStringLiteral("Response"),
                         &response, SLOT(receive(uint,QVariantMap)))) return {};
    }

    auto closeRequest = [&] {
        bus.asyncCall(QDBusMessage::createMethodCall(service, requestPath,
            requestInterface, QStringLiteral("Close")));
        response.finished = true;
        response.code = 1;
        response.loop.quit();
    };
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, &response, closeRequest);
    if (window) QObject::connect(window, &QObject::destroyed, &response, closeRequest);
    if (!response.finished) response.loop.exec(QEventLoop::DialogExec);
    if (!response.finished) return {};
    if (response.code == 1) return {true, {}, {}};
    if (response.code != 0) return {};

    const QStringList uris = qdbus_cast<QStringList>(response.results.value(QStringLiteral("uris")));
    if (uris.isEmpty() || !QUrl(uris.first()).isLocalFile()) {
        qWarning("[snipaste-portal] Portal returned no local file; using original dialog.");
        return {};
    }
    QString chosenFilter;
    if (response.results.contains(QStringLiteral("current_filter"))) {
        const auto current = qdbus_cast<Filter>(response.results.value(QStringLiteral("current_filter")));
        for (int i = 0; i < filters.size(); ++i) {
            const auto &candidate = filters.at(i);
            bool equal = candidate.label == current.label
                && candidate.patterns.size() == current.patterns.size();
            for (int j = 0; equal && j < candidate.patterns.size(); ++j)
                equal = candidate.patterns[j].kind == current.patterns[j].kind
                    && candidate.patterns[j].value == current.patterns[j].value;
            if (equal) { chosenFilter = originals.at(i); break; }
        }
    }
    return {true, QUrl(uris.first()).toLocalFile(), chosenFilter};
}

template<typename Function>
static Function original(const char *symbol)
{
    auto function = reinterpret_cast<Function>(dlsym(RTLD_NEXT, symbol));
    if (!function) qWarning("[snipaste-portal] Cannot resolve %s: %s", symbol, dlerror());
    else qWarning("[snipaste-portal] Portal unavailable; using original dialog.");
    return function;
}

QString QFileDialog::getSaveFileName(QWidget *parent, const QString &caption,
    const QString &dir, const QString &filter, QString *selectedFilter, Options options)
{
    const auto result = choose(parent, caption, dir, filter, selectedFilter, true, false);
    if (result.handled) {
        if (selectedFilter && !result.path.isEmpty() && !result.filter.isEmpty())
            *selectedFilter = result.filter;
        return result.path;
    }
    const auto next = original<decltype(&QFileDialog::getSaveFileName)>(
        "_ZN11QFileDialog15getSaveFileNameEP7QWidgetRK7QStringS4_S4_PS2_6QFlagsINS_6OptionEE");
    return next ? next(parent, caption, dir, filter, selectedFilter, options) : QString();
}

QString QFileDialog::getOpenFileName(QWidget *parent, const QString &caption,
    const QString &dir, const QString &filter, QString *selectedFilter, Options options)
{
    const auto result = choose(parent, caption, dir, filter, selectedFilter, false, false);
    if (result.handled) {
        if (selectedFilter && !result.path.isEmpty() && !result.filter.isEmpty())
            *selectedFilter = result.filter;
        return result.path;
    }
    const auto next = original<decltype(&QFileDialog::getOpenFileName)>(
        "_ZN11QFileDialog15getOpenFileNameEP7QWidgetRK7QStringS4_S4_PS2_6QFlagsINS_6OptionEE");
    return next ? next(parent, caption, dir, filter, selectedFilter, options) : QString();
}

QString QFileDialog::getExistingDirectory(QWidget *parent, const QString &caption,
    const QString &dir, Options options)
{
    const auto result = choose(parent, caption, dir, {}, nullptr, false, true);
    if (result.handled) return result.path;
    const auto next = original<decltype(&QFileDialog::getExistingDirectory)>(
        "_ZN11QFileDialog20getExistingDirectoryEP7QWidgetRK7QStringS4_6QFlagsINS_6OptionEE");
    return next ? next(parent, caption, dir, options) : QString();
}

#include "portal-dialog.moc"
