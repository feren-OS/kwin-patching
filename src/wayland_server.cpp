/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "wayland_server.h"
#include "abstract_wayland_output.h"
#include "x11client.h"
#include "platform.h"
#include "composite.h"
#include "idle_inhibition.h"
#include "inputpanelv1integration.h"
#include "screens.h"
#include "layershellv1integration.h"
#include "main.h"
#include "xdgshellintegration.h"
#include "workspace.h"
#include "xdgshellclient.h"
#include "xdgactivationv1.h"
#include "service_utils.h"
#include "unmanaged.h"

// Client
#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/event_queue.h>
#include <KWayland/Client/registry.h>
#include <KWayland/Client/compositor.h>
#include <KWayland/Client/seat.h>
#include <KWayland/Client/datadevicemanager.h>
#include <KWayland/Client/surface.h>
// Server
#include <KWaylandServer/appmenu_interface.h>
#include <KWaylandServer/compositor_interface.h>
#include <KWaylandServer/datadevicemanager_interface.h>
#include <KWaylandServer/datasource_interface.h>
#include <KWaylandServer/display.h>
#include <KWaylandServer/dpms_interface.h>
#include <KWaylandServer/idle_interface.h>
#include <KWaylandServer/idleinhibit_v1_interface.h>
#include <KWaylandServer/linuxdmabuf_v1_interface.h>
#include <KWaylandServer/output_interface.h>
#include <KWaylandServer/plasmashell_interface.h>
#include <KWaylandServer/plasmavirtualdesktop_interface.h>
#include <KWaylandServer/plasmawindowmanagement_interface.h>
#include <KWaylandServer/pointerconstraints_v1_interface.h>
#include <KWaylandServer/pointergestures_v1_interface.h>
#include <KWaylandServer/seat_interface.h>
#include <KWaylandServer/server_decoration_interface.h>
#include <KWaylandServer/server_decoration_palette_interface.h>
#include <KWaylandServer/shadow_interface.h>
#include <KWaylandServer/subcompositor_interface.h>
#include <KWaylandServer/blur_interface.h>
#include <KWaylandServer/outputmanagement_interface.h>
#include <KWaylandServer/outputconfiguration_interface.h>
#include <KWaylandServer/xdgactivation_v1_interface.h>
#include <KWaylandServer/xdgdecoration_v1_interface.h>
#include <KWaylandServer/xdgshell_interface.h>
#include <KWaylandServer/xdgforeign_v2_interface.h>
#include <KWaylandServer/xdgoutput_v1_interface.h>
#include <KWaylandServer/keystate_interface.h>
#include <KWaylandServer/filtered_display.h>
#include <KWaylandServer/keyboard_shortcuts_inhibit_v1_interface.h>
#include <KWaylandServer/inputmethod_v1_interface.h>
#include <KWaylandServer/tablet_v2_interface.h>
#include <KWaylandServer/viewporter_interface.h>
#include <KWaylandServer/datacontroldevicemanager_v1_interface.h>
#include <KWaylandServer/primaryselectiondevicemanager_v1_interface.h>

// Qt
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QWindow>

// system
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

//screenlocker
#include <KScreenLocker/KsldApp>

using namespace KWaylandServer;

namespace KWin
{

KWIN_SINGLETON_FACTORY(WaylandServer)

class KWinDisplay : public KWaylandServer::FilteredDisplay
{
public:
    KWinDisplay(QObject *parent)
        : KWaylandServer::FilteredDisplay(parent)
    {}

    static QByteArray sha256(const QString &fileName)
    {
        QFile f(fileName);
        if (f.open(QFile::ReadOnly)) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (hash.addData(&f)) {
                return hash.result();
            }
        }
        return QByteArray();
    }

    bool isTrustedOrigin(KWaylandServer::ClientConnection *client) const {
        const auto fullPathSha = sha256(client->executablePath());
        const auto localSha = sha256(QLatin1String("/proc/") + QString::number(client->processId()) + QLatin1String("/exe"));
        const bool trusted = !localSha.isEmpty() && fullPathSha == localSha;

        if (!trusted) {
            qCWarning(KWIN_CORE) << "Could not trust" << client->executablePath() << "sha" << localSha << fullPathSha;
        }

        return trusted;
    }

    QStringList fetchRequestedInterfaces(KWaylandServer::ClientConnection *client) const {
        return KWin::fetchRequestedInterfaces(client->executablePath());
    }

    const QSet<QByteArray> interfacesBlackList = {"org_kde_kwin_remote_access_manager", "org_kde_plasma_window_management", "org_kde_kwin_fake_input", "org_kde_kwin_keystate", "zkde_screencast_unstable_v1"};

    const QSet<QByteArray> inputmethodInterfaces = { "zwp_input_panel_v1", "zwp_input_method_v1" };

    QSet<QString> m_reported;

    bool allowInterface(KWaylandServer::ClientConnection *client, const QByteArray &interfaceName) override {
        if (client->processId() == getpid()) {
            return true;
        }

        if (client != waylandServer()->inputMethodConnection() && inputmethodInterfaces.contains(interfaceName)) {
            return false;
        }

        if (!interfacesBlackList.contains(interfaceName)) {
            return true;
        }

        if (client->executablePath().isEmpty()) {
            qCWarning(KWIN_CORE) << "Could not identify process with pid" << client->processId();
            return false;
        }

        {
            auto requestedInterfaces = client->property("requestedInterfaces");
            if (requestedInterfaces.isNull()) {
                requestedInterfaces = fetchRequestedInterfaces(client);
                client->setProperty("requestedInterfaces", requestedInterfaces);
            }
            if (!requestedInterfaces.toStringList().contains(QString::fromUtf8(interfaceName))) {
                if (KWIN_CORE().isDebugEnabled()) {
                    const QString id = client->executablePath() + QLatin1Char('|') + QString::fromUtf8(interfaceName);
                    if (!m_reported.contains({id})) {
                        m_reported.insert(id);
                        qCDebug(KWIN_CORE) << "Interface" << interfaceName << "not in X-KDE-Wayland-Interfaces of" << client->executablePath();
                    }
                }
                return false;
            }
        }

        {
            auto trustedOrigin = client->property("isPrivileged");
            if (trustedOrigin.isNull()) {
                trustedOrigin = isTrustedOrigin(client);
                client->setProperty("isPrivileged", trustedOrigin);
            }

            if (!trustedOrigin.toBool()) {
                return false;
            }
        }
        qCDebug(KWIN_CORE) << "authorized" << client->executablePath() << interfaceName;
        return true;
    }
};

WaylandServer::WaylandServer(QObject *parent)
    : QObject(parent)
    , m_display(new KWinDisplay(this))
{
    qRegisterMetaType<KWaylandServer::OutputInterface::DpmsMode>();
}

WaylandServer::~WaylandServer()
{
    destroyInputMethodConnection();
}

KWaylandServer::ClientConnection *WaylandServer::xWaylandConnection() const
{
    return m_xwaylandConnection;
}

void WaylandServer::destroyInternalConnection()
{
    emit terminatingInternalClientConnection();
    if (m_internalConnection.client) {
        // delete all connections hold by plugins like e.g. widget style
        const auto connections = KWayland::Client::ConnectionThread::connections();
        for (auto c : connections) {
            if (c == m_internalConnection.client) {
                continue;
            }
            emit c->connectionDied();
        }

        delete m_internalConnection.registry;
        delete m_internalConnection.compositor;
        delete m_internalConnection.seat;
        delete m_internalConnection.ddm;
        dispatch();
        m_internalConnection.client->deleteLater();
        m_internalConnection.clientThread->quit();
        m_internalConnection.clientThread->wait();
        delete m_internalConnection.clientThread;
        m_internalConnection.client = nullptr;
        m_internalConnection.server->destroy();
        m_internalConnection.server = nullptr;
    }
}

void WaylandServer::terminateClientConnections()
{
    destroyInternalConnection();
    destroyInputMethodConnection();
    const auto connections = m_display->connections();
    for (auto it = connections.begin(); it != connections.end(); ++it) {
        (*it)->destroy();
    }
}

void WaylandServer::registerShellClient(AbstractClient *client)
{
    if (client->readyForPainting()) {
        emit shellClientAdded(client);
    } else {
        connect(client, &AbstractClient::windowShown, this, &WaylandServer::shellClientShown);
    }
    m_clients << client;
}

void WaylandServer::registerXdgToplevelClient(XdgToplevelClient *client)
{
    // TODO: Find a better way and more generic to install extensions.

    SurfaceInterface *surface = client->surface();

    if (surface->client() == m_screenLockerClientConnection) {
        ScreenLocker::KSldApp::self()->lockScreenShown();
    }

    registerShellClient(client);

    auto it = std::find_if(m_plasmaShellSurfaces.begin(), m_plasmaShellSurfaces.end(),
        [surface] (PlasmaShellSurfaceInterface *plasmaSurface) {
            return plasmaSurface->surface() == surface;
        }
    );
    if (it != m_plasmaShellSurfaces.end()) {
        client->installPlasmaShellSurface(*it);
        m_plasmaShellSurfaces.erase(it);
    }
    if (auto decoration = ServerSideDecorationInterface::get(surface)) {
        client->installServerDecoration(decoration);
    }
    if (auto decoration = XdgToplevelDecorationV1Interface::get(client->shellSurface())) {
        client->installXdgDecoration(decoration);
    }
    if (auto menu = m_appMenuManager->appMenuForSurface(surface)) {
        client->installAppMenu(menu);
    }
    if (auto palette = m_paletteManager->paletteForSurface(surface)) {
        client->installPalette(palette);
    }

    connect(m_XdgForeign, &XdgForeignV2Interface::transientChanged, client, [this](SurfaceInterface *child) {
        emit foreignTransientChanged(child);
    });
}

void WaylandServer::registerXdgGenericClient(AbstractClient *client)
{
    XdgToplevelClient *toplevelClient = qobject_cast<XdgToplevelClient *>(client);
    if (toplevelClient) {
        registerXdgToplevelClient(toplevelClient);
        return;
    }
    XdgPopupClient *popupClient = qobject_cast<XdgPopupClient *>(client);
    if (popupClient) {
        registerShellClient(popupClient);

        SurfaceInterface *surface = client->surface();
        auto it = std::find_if(m_plasmaShellSurfaces.begin(), m_plasmaShellSurfaces.end(),
            [surface] (PlasmaShellSurfaceInterface *plasmaSurface) {
                return plasmaSurface->surface() == surface;
            }
        );

        if (it != m_plasmaShellSurfaces.end()) {
            popupClient->installPlasmaShellSurface(*it);
            m_plasmaShellSurfaces.erase(it);
        }

        return;
    }
    qCDebug(KWIN_CORE) << "Received invalid xdg client:" << client->surface();
}

AbstractWaylandOutput *WaylandServer::findOutput(KWaylandServer::OutputInterface *outputIface) const
{
    AbstractWaylandOutput *outputFound = nullptr;
    const auto outputs = kwinApp()->platform()->enabledOutputs();
    for (auto output : outputs) {
        if (static_cast<AbstractWaylandOutput *>(output)->waylandOutput() == outputIface) {
            outputFound = static_cast<AbstractWaylandOutput *>(output);
        }
    }
    return outputFound;
}

bool WaylandServer::start()
{
    return m_display->start();
}

bool WaylandServer::init(const QString &socketName, InitializationFlags flags)
{
    if (!m_display->addSocketName(socketName)) {
        return false;
    }
    return init(flags);
}

bool WaylandServer::init(InitializationFlags flags)
{
    m_initFlags = flags;
    m_compositor = new CompositorInterface(m_display, m_display);
    connect(m_compositor, &CompositorInterface::surfaceCreated, this,
        [this] (SurfaceInterface *surface) {
            // check whether we have a Toplevel with the Surface's id
            Workspace *ws = Workspace::self();
            if (!ws) {
                // it's possible that a Surface gets created before Workspace is created
                return;
            }
            if (surface->client() != xWaylandConnection()) {
                // setting surface is only relevant for Xwayland clients
                return;
            }

            X11Client *client = ws->findClient([surface](const X11Client *client) {
                return client->surfaceId() == surface->id();
            });
            if (client) {
                client->setSurface(surface);
                return;
            }

            Unmanaged *unmanaged = ws->findUnmanaged([surface](const Unmanaged *unmanaged) {
                return unmanaged->surfaceId() == surface->id();
            });
            if (unmanaged) {
                unmanaged->setSurface(surface);
                return;
            }

            // The surface will be bound later when a WL_SURFACE_ID message is received.
        }
    );

    m_tabletManagerV2 = new TabletManagerV2Interface(m_display, m_display);
    m_keyboardShortcutsInhibitManager = new KeyboardShortcutsInhibitManagerV1Interface(m_display, m_display);

    auto inputPanelV1Integration = new InputPanelV1Integration(this);
    connect(inputPanelV1Integration, &InputPanelV1Integration::clientCreated,
            this, &WaylandServer::registerShellClient);

    auto xdgShellIntegration = new XdgShellIntegration(this);
    connect(xdgShellIntegration, &XdgShellIntegration::clientCreated,
            this, &WaylandServer::registerXdgGenericClient);

    auto layerShellV1Integration = new LayerShellV1Integration(this);
    connect(layerShellV1Integration, &LayerShellV1Integration::clientCreated,
            this, &WaylandServer::registerShellClient);

    m_xdgDecorationManagerV1 = new XdgDecorationManagerV1Interface(m_display, m_display);
    connect(m_xdgDecorationManagerV1, &XdgDecorationManagerV1Interface::decorationCreated, this,
        [this](XdgToplevelDecorationV1Interface *decoration) {
            if (XdgToplevelClient *toplevel = findXdgToplevelClient(decoration->toplevel()->surface())) {
                toplevel->installXdgDecoration(decoration);
            }
        }
    );

    new ViewporterInterface(m_display, m_display);
    m_display->createShm();
    m_seat = new SeatInterface(m_display, m_display);
    m_seat->create();
    new PointerGesturesV1Interface(m_display, m_display);
    new PointerConstraintsV1Interface(m_display, m_display);
    m_dataDeviceManager = new DataDeviceManagerInterface(m_display, m_display);
    new DataControlDeviceManagerV1Interface(m_display, m_display);
    new PrimarySelectionDeviceManagerV1Interface(m_display, m_display);
    m_idle = new IdleInterface(m_display, m_display);
    auto idleInhibition = new IdleInhibition(m_idle);
    connect(this, &WaylandServer::shellClientAdded, idleInhibition, &IdleInhibition::registerClient);
    new IdleInhibitManagerV1Interface(m_display, m_display);
    m_plasmaShell = new PlasmaShellInterface(m_display, m_display);
    connect(m_plasmaShell, &PlasmaShellInterface::surfaceCreated,
        [this] (PlasmaShellSurfaceInterface *surface) {
            if (XdgSurfaceClient *client = findXdgSurfaceClient(surface->surface())) {
                client->installPlasmaShellSurface(surface);
                return;
            }

            m_plasmaShellSurfaces.append(surface);
            connect(surface, &QObject::destroyed, this, [this, surface] {
                m_plasmaShellSurfaces.removeOne(surface);
            });
        }
    );
    m_appMenuManager = new AppMenuManagerInterface(m_display, m_display);
    connect(m_appMenuManager, &AppMenuManagerInterface::appMenuCreated,
        [this] (AppMenuInterface *appMenu) {
            if (XdgToplevelClient *client = findXdgToplevelClient(appMenu->surface())) {
                client->installAppMenu(appMenu);
            }
        }
    );
    m_paletteManager = new ServerSideDecorationPaletteManagerInterface(m_display, m_display);
    connect(m_paletteManager, &ServerSideDecorationPaletteManagerInterface::paletteCreated,
        [this] (ServerSideDecorationPaletteInterface *palette) {
            if (XdgToplevelClient *client = findXdgToplevelClient(palette->surface())) {
                client->installPalette(palette);
            }
        }
    );

    m_windowManagement = new PlasmaWindowManagementInterface(m_display, m_display);
    m_windowManagement->setShowingDesktopState(PlasmaWindowManagementInterface::ShowingDesktopState::Disabled);
    connect(m_windowManagement, &PlasmaWindowManagementInterface::requestChangeShowingDesktop, this,
        [] (PlasmaWindowManagementInterface::ShowingDesktopState state) {
            if (!workspace()) {
                return;
            }
            bool set = false;
            switch (state) {
            case PlasmaWindowManagementInterface::ShowingDesktopState::Disabled:
                set = false;
                break;
            case PlasmaWindowManagementInterface::ShowingDesktopState::Enabled:
                set = true;
                break;
            default:
                Q_UNREACHABLE();
                break;
            }
            if (set == workspace()->showingDesktop()) {
                return;
            }
            workspace()->setShowingDesktop(set);
        }
    );

    m_virtualDesktopManagement = new PlasmaVirtualDesktopManagementInterface(m_display, m_display);
    m_windowManagement->setPlasmaVirtualDesktopManagementInterface(m_virtualDesktopManagement);

    new ShadowManagerInterface(m_display, m_display);
    new DpmsManagerInterface(m_display, m_display);

    m_decorationManager = new ServerSideDecorationManagerInterface(m_display, m_display);
    connect(m_decorationManager, &ServerSideDecorationManagerInterface::decorationCreated, this,
        [this] (ServerSideDecorationInterface *decoration) {
            if (XdgToplevelClient *client = findXdgToplevelClient(decoration->surface())) {
                client->installServerDecoration(decoration);
            }
            connect(decoration, &ServerSideDecorationInterface::modeRequested, this,
                [decoration] (ServerSideDecorationManagerInterface::Mode mode) {
                    // always acknowledge the requested mode
                    decoration->setMode(mode);
                }
            );
        }
    );

    m_outputManagement = new OutputManagementInterface(m_display, m_display);
    connect(m_outputManagement, &OutputManagementInterface::configurationChangeRequested,
            this, [](KWaylandServer::OutputConfigurationInterface *config) {
                kwinApp()->platform()->requestOutputsChange(config);
    });
    m_outputManagement->create();

    m_xdgOutputManagerV1 = new XdgOutputManagerV1Interface(m_display, m_display);
    new SubCompositorInterface(m_display, m_display);
    m_XdgForeign = new XdgForeignV2Interface(m_display, m_display);
    m_keyState = new KeyStateInterface(m_display, m_display);
    m_inputMethod = new InputMethodV1Interface(m_display, m_display);

    auto activation = new KWaylandServer::XdgActivationV1Interface(m_display, this);
    auto init = [this, activation] {
        new XdgActivationV1Integration(activation, this);
    };
    if (Workspace::self())
        init();
    else
        connect(static_cast<Application*>(qApp), &Application::workspaceCreated, this, init);

    return true;
}


KWaylandServer::LinuxDmabufUnstableV1Interface *WaylandServer::linuxDmabuf()
{
    if (!m_linuxDmabuf) {
        m_linuxDmabuf = new LinuxDmabufUnstableV1Interface(m_display, m_display);
        m_linuxDmabuf->create();
    }
    return m_linuxDmabuf;
}

SurfaceInterface *WaylandServer::findForeignTransientForSurface(SurfaceInterface *surface)
{
    return m_XdgForeign->transientFor(surface);
}

void WaylandServer::shellClientShown(Toplevel *toplevel)
{
    AbstractClient *client = qobject_cast<AbstractClient *>(toplevel);
    if (!client) {
        qCWarning(KWIN_CORE) << "Failed to cast a Toplevel which is supposed to be an AbstractClient to AbstractClient";
        return;
    }
    disconnect(client, &AbstractClient::windowShown, this, &WaylandServer::shellClientShown);
    emit shellClientAdded(client);
}

void WaylandServer::initWorkspace()
{
    VirtualDesktopManager::self()->setVirtualDesktopManagement(m_virtualDesktopManagement);

    if (m_windowManagement) {
        connect(workspace(), &Workspace::showingDesktopChanged, this,
            [this] (bool set) {
                using namespace KWaylandServer;
                m_windowManagement->setShowingDesktopState(set ?
                    PlasmaWindowManagementInterface::ShowingDesktopState::Enabled :
                    PlasmaWindowManagementInterface::ShowingDesktopState::Disabled
                );
            }
        );

        connect(workspace(), &Workspace::workspaceInitialized, this, [this] {
            auto f = [this] () {
                QVector<quint32> ids;
                for (Toplevel *toplevel : workspace()->stackingOrder()) {
                    auto *client = qobject_cast<AbstractClient *>(toplevel);
                    if (client && client->windowManagementInterface()) {
                        ids << client->windowManagementInterface()->internalId();
                    }
                }
                m_windowManagement->setStackingOrder(ids);
            };
            f();
            connect(workspace(), &Workspace::stackingOrderChanged, this, f);
        });
    }

    if (hasScreenLockerIntegration()) {
        if (m_internalConnection.interfacesAnnounced) {
            initScreenLocker();
        } else {
            connect(m_internalConnection.registry, &KWayland::Client::Registry::interfacesAnnounced, this, &WaylandServer::initScreenLocker);
        }
    } else {
        emit initialized();
    }
}

void WaylandServer::initScreenLocker()
{
    auto *screenLockerApp = ScreenLocker::KSldApp::self();

    ScreenLocker::KSldApp::self()->setGreeterEnvironment(kwinApp()->processStartupEnvironment());
    ScreenLocker::KSldApp::self()->initialize();

    connect(ScreenLocker::KSldApp::self(), &ScreenLocker::KSldApp::aboutToLock, this,
        [this, screenLockerApp] () {
            if (m_screenLockerClientConnection) {
                // Already sent data to KScreenLocker.
                return;
            }
            int clientFd = createScreenLockerConnection();
            if (clientFd < 0) {
                return;
            }
            ScreenLocker::KSldApp::self()->setWaylandFd(clientFd);

            for (auto *seat : m_display->seats()) {
                connect(seat, &KWaylandServer::SeatInterface::timestampChanged,
                        screenLockerApp, &ScreenLocker::KSldApp::userActivity);
            }
        }
    );

    connect(ScreenLocker::KSldApp::self(), &ScreenLocker::KSldApp::unlocked, this,
        [this, screenLockerApp] () {
            if (m_screenLockerClientConnection) {
                m_screenLockerClientConnection->destroy();
                delete m_screenLockerClientConnection;
                m_screenLockerClientConnection = nullptr;
            }

            for (auto *seat : m_display->seats()) {
                disconnect(seat, &KWaylandServer::SeatInterface::timestampChanged,
                           screenLockerApp, &ScreenLocker::KSldApp::userActivity);
            }
            ScreenLocker::KSldApp::self()->setWaylandFd(-1);
        }
    );

    if (m_initFlags.testFlag(InitializationFlag::LockScreen)) {
        ScreenLocker::KSldApp::self()->lock(ScreenLocker::EstablishLock::Immediate);
    }
    emit initialized();
}

WaylandServer::SocketPairConnection WaylandServer::createConnection()
{
    SocketPairConnection ret;
    int sx[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sx) < 0) {
        qCWarning(KWIN_CORE) << "Could not create socket";
        return ret;
    }
    ret.connection = m_display->createClient(sx[0]);
    ret.fd = sx[1];
    return ret;
}

int WaylandServer::createScreenLockerConnection()
{
    const auto socket = createConnection();
    if (!socket.connection) {
        return -1;
    }
    m_screenLockerClientConnection = socket.connection;
    connect(m_screenLockerClientConnection, &KWaylandServer::ClientConnection::disconnected,
            this, [this] { m_screenLockerClientConnection = nullptr; });
    return socket.fd;
}

int WaylandServer::createXWaylandConnection()
{
    const auto socket = createConnection();
    if (!socket.connection) {
        return -1;
    }
    m_xwaylandConnection = socket.connection;
    return socket.fd;
}

void WaylandServer::destroyXWaylandConnection()
{
    if (!m_xwaylandConnection) {
        return;
    }
    m_xwaylandConnection->destroy();
    m_xwaylandConnection = nullptr;
}

int WaylandServer::createInputMethodConnection()
{
    const auto socket = createConnection();
    if (!socket.connection) {
        return -1;
    }
    m_inputMethodServerConnection = socket.connection;
    return socket.fd;
}

void WaylandServer::destroyInputMethodConnection()
{
    if (!m_inputMethodServerConnection) {
        return;
    }
    m_inputMethodServerConnection->destroy();
    m_inputMethodServerConnection = nullptr;
}

void WaylandServer::createInternalConnection()
{
    const auto socket = createConnection();
    if (!socket.connection) {
        return;
    }
    m_internalConnection.server = socket.connection;
    using namespace KWayland::Client;
    m_internalConnection.client = new ConnectionThread();
    m_internalConnection.client->setSocketFd(socket.fd);
    m_internalConnection.clientThread = new QThread;
    m_internalConnection.client->moveToThread(m_internalConnection.clientThread);
    m_internalConnection.clientThread->start();

    connect(m_internalConnection.client, &ConnectionThread::connected, this,
        [this] {
            Registry *registry = new Registry(this);
            EventQueue *eventQueue = new EventQueue(registry);
            eventQueue->setup(m_internalConnection.client);
            registry->setEventQueue(eventQueue);
            registry->create(m_internalConnection.client);
            m_internalConnection.registry = registry;
            connect(registry, &Registry::interfacesAnnounced, this,
                [this, registry] {
                    m_internalConnection.interfacesAnnounced = true;

                    const auto compInterface = registry->interface(Registry::Interface::Compositor);
                    if (compInterface.name != 0) {
                        m_internalConnection.compositor = registry->createCompositor(compInterface.name, compInterface.version, this);
                    }
                    const auto seatInterface = registry->interface(Registry::Interface::Seat);
                    if (seatInterface.name != 0) {
                        m_internalConnection.seat = registry->createSeat(seatInterface.name, seatInterface.version, this);
                    }
                    const auto ddmInterface = registry->interface(Registry::Interface::DataDeviceManager);
                    if (ddmInterface.name != 0) {
                        m_internalConnection.ddm = registry->createDataDeviceManager(ddmInterface.name, ddmInterface.version, this);
                    }
                }
            );
            registry->setup();
        }
    );
    m_internalConnection.client->initConnection();
}

void WaylandServer::removeClient(AbstractClient *c)
{
    m_clients.removeAll(c);
    emit shellClientRemoved(c);
}

void WaylandServer::dispatch()
{
    if (!m_display) {
        return;
    }
    if (m_internalConnection.server) {
        m_internalConnection.server->flush();
    }
    m_display->dispatchEvents();
}

static AbstractClient *findClientInList(const QList<AbstractClient *> &clients, const KWaylandServer::SurfaceInterface *surface)
{
    auto it = std::find_if(clients.begin(), clients.end(),
        [surface] (AbstractClient *c) {
            return c->surface() == surface;
        }
    );
    if (it == clients.end()) {
        return nullptr;
    }
    return *it;
}

AbstractClient *WaylandServer::findClient(const KWaylandServer::SurfaceInterface *surface) const
{
    if (!surface) {
        return nullptr;
    }
    if (AbstractClient *c = findClientInList(m_clients, surface)) {
        return c;
    }
    return nullptr;
}

XdgToplevelClient *WaylandServer::findXdgToplevelClient(SurfaceInterface *surface) const
{
    return qobject_cast<XdgToplevelClient *>(findClient(surface));
}

XdgSurfaceClient *WaylandServer::findXdgSurfaceClient(SurfaceInterface *surface) const
{
    return qobject_cast<XdgSurfaceClient *>(findClient(surface));
}

bool WaylandServer::isScreenLocked() const
{
    if (!hasScreenLockerIntegration()) {
        return false;
    }
    return ScreenLocker::KSldApp::self()->lockState() == ScreenLocker::KSldApp::Locked ||
           ScreenLocker::KSldApp::self()->lockState() == ScreenLocker::KSldApp::AcquiringLock;
}

bool WaylandServer::hasScreenLockerIntegration() const
{
    return !m_initFlags.testFlag(InitializationFlag::NoLockScreenIntegration);
}

bool WaylandServer::hasGlobalShortcutSupport() const
{
    return !m_initFlags.testFlag(InitializationFlag::NoGlobalShortcuts);
}

void WaylandServer::simulateUserActivity()
{
    if (m_idle) {
        m_idle->simulateUserActivity();
    }
}

void WaylandServer::updateKeyState(KWin::Xkb::LEDs leds)
{
    if (!m_keyState)
        return;

    m_keyState->setState(KeyStateInterface::Key::CapsLock, leds & KWin::Xkb::LED::CapsLock ? KeyStateInterface::State::Locked : KeyStateInterface::State::Unlocked);
    m_keyState->setState(KeyStateInterface::Key::NumLock, leds & KWin::Xkb::LED::NumLock ? KeyStateInterface::State::Locked : KeyStateInterface::State::Unlocked);
    m_keyState->setState(KeyStateInterface::Key::ScrollLock, leds & KWin::Xkb::LED::ScrollLock ? KeyStateInterface::State::Locked : KeyStateInterface::State::Unlocked);
}

bool WaylandServer::isKeyboardShortcutsInhibited() const
{
    auto surface = seat()->focusedKeyboardSurface();
    if (surface) {
        auto inhibitor = keyboardShortcutsInhibitManager()->findInhibitor(surface, seat());
        return inhibitor && inhibitor->isActive();
    }
    return false;
}

QString WaylandServer::socketName() const
{
    const QStringList socketNames = display()->socketNames();
    if (!socketNames.isEmpty()) {
        return socketNames.first();
    }
    return QString();
}

}
