/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2013 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#ifndef KWIN_SCENE_QPAINTER_BACKEND_H
#define KWIN_SCENE_QPAINTER_BACKEND_H

class QImage;
class QRegion;
class QSize;
class QString;

namespace KWin
{

class QPainterBackend
{
public:
    virtual ~QPainterBackend();
    virtual void endFrame(int screenId, int mask, const QRegion &damage) = 0;
    virtual void beginFrame(int screenId) = 0;
    /**
     * @brief React on screen geometry changes.
     *
     * Default implementation does nothing. Override if specific functionality is required.
     *
     * @param size The new screen size
     */
    virtual void screenGeometryChanged(const QSize &size);
    /**
     * @brief Whether the creation of the Backend failed.
     *
     * The SceneQPainter should test whether the Backend got constructed correctly. If this method
     * returns @c true, the SceneQPainter should not try to start the rendering.
     *
     * @return bool @c true if the creation of the Backend failed, @c false otherwise.
     */
    bool isFailed() const {
        return m_failed;
    }
    /**
     * Overload for the case that there is a different buffer per screen.
     * Default implementation just calls buffer.
     * @param screenId The id of the screen as used in Screens
     * @todo Get a better identifier for screen then a counter variable
     */
    virtual QImage *bufferForScreen(int screenId) = 0;
    virtual bool needsFullRepaint(int screenId) const = 0;

protected:
    QPainterBackend();
    /**
     * @brief Sets the backend initialization to failed.
     *
     * This method should be called by the concrete subclass in case the initialization failed.
     * The given @p reason is logged as a warning.
     *
     * @param reason The reason why the initialization failed.
     */
    void setFailed(const QString &reason);

private:
    bool m_failed;
};

} // KWin

#endif
