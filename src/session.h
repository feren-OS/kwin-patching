/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <kwinglobals.h>

#include <QObject>
#include <QString>

namespace KWin
{

/**
 * The Session class represents the session controlled by the compositor.
 *
 * The Session class provides information about the virtual terminal where the compositor
 * is running and a way to open files that require special privileges, e.g. DRM devices or
 * input devices.
 */
class KWIN_EXPORT Session : public QObject
{
    Q_OBJECT

public:
    enum class Type {
        Direct,
        Noop,
        ConsoleKit,
        Logind,
    };

    static Session *create(QObject *parent = nullptr);
    static Session *create(Type type, QObject *parent = nullptr);

    /**
     * Returns @c true if the session is active; otherwise returns @c false.
     */
    virtual bool isActive() const = 0;

    /**
     * Returns the seat name for the Session.
     */
    virtual QString seat() const = 0;

    /**
     * Returns the terminal controlled by the Session.
     */
    virtual uint terminal() const = 0;

    /**
     * Opens the file with the specified @a fileName. Returns the file descriptor
     * of the file or @a -1 if an error has occurred.
     */
    virtual int openRestricted(const QString &fileName) = 0;

    /**
     * Closes a file that has been opened using the openRestricted() function.
     */
    virtual void closeRestricted(int fileDescriptor) = 0;

    /**
     * Switches to the specified virtual @a terminal.
     */
    virtual void switchTo(uint terminal) = 0;

Q_SIGNALS:
    /**
     * This signal is emitted when the session is resuming from suspend.
     */
    void awoke();
    /**
     * This signal is emitted when the active state of the session has changed.
     */
    void activeChanged(bool active);

protected:
    explicit Session(QObject *parent = nullptr);
};

} // namespace KWin
