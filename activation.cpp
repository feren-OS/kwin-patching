/*****************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 1999, 2000 Matthias Ettrich <ettrich@kde.org>
Copyright (C) 2003 Lubos Lunak <l.lunak@kde.org>

You can Freely distribute this program under the GNU General Public
License. See the file "COPYING" for the exact licensing terms.
******************************************************************/

/*

 This file contains things relevant to window activation and focus
 stealing prevention.

*/

#include "client.h"
#include "workspace.h"

#include <qpopupmenu.h>
#include <kxerrorhandler.h>
#include <kstartupinfo.h>

#include "notifications.h"
#include "atoms.h"

extern Time qt_x_time;

namespace KWinInternal
{

/*
 Prevention of focus stealing:

 KWin tries to prevent unwanted changes of focus, that would result
 from mapping a new window. Also, some nasty applications may try
 to force focus change even in cases when ICCCM 4.2.7 doesn't allow it
 (e.g. they may try to activate their main window because the user
 definitely "needs" to see something happened - misusing
 of QWidget::setActiveWindow() may be such case).

 There are 4 ways how a window may become active:
 - the user changes the active window (e.g. focus follows mouse, clicking
   on some window's titlebar) - the change of focus will
   be done by KWin, so there's nothing to solve in this case
 - the change of active window will be requested using the _NET_ACTIVE_WINDOW
   message (handled in RootInfo::changeActiveWindow()) - such requests
   will be obeyed, because this request is meant mainly for e.g. taskbar
   asking the WM to change the active window as a result of some user action.
   Normal applications should use this request only rarely in special cases.
   See also below the discussion of _NET_ACTIVE_WINDOW_TRANSFER.
 - the change of active window will be done by performing XSetInputFocus()
   on a window that's not currently active. ICCCM 4.2.7 describes when
   the application may perform change of input focus. In order to handle
   misbehaving applications, KWin will try to detect focus changes to
   windows that don't belong to currently active application, and restore
   focus back to the currently active window, instead of activating the window
   that got focus (unfortunately there's no way to FocusChangeRedirect similar
   to e.g. SubstructureRedirect, so there will be short time when the focus
   will be changed). The check itself that's done is
   Workspace::allowClientActivation() (see below).
 - a new window will be mapped - this is the most complicated case. If
   the new window belongs to the currently active application, it may be safely
   mapped on top and activated. The same if there's no active window,
   or the active window is the desktop. These checks are done by
   Workspace::allowClientActivation().
    Following checks need to compare times. One time is the timestamp
   of last user action in the currently active window, the other time is
   the timestamp of the action that originally caused mapping of the new window
   (e.g. when the application was started). If the first time is newer than
   the second one, the window will not be activated, as that indicates
   futher user actions took place after the action leading to this new
   mapped window. This check is done by Workspace::allowClientActivation().
    There are several ways how to get the timestamp of action that caused
   the new mapped window (done in Client::readUserTimeMapTimestamp()) :
     - the window may have the _NET_WM_USER_TIME property. This way
       the application may either explicitly request that the window is not
       activated (by using 0 timestamp), or the property contains the time
       of last user action in the application.
     - KWin itself tries to detect time of last user action in every window,
       by watching KeyPress and ButtonPress events on windows. This way some
       events may be missed (if they don't propagate to the toplevel window),
       but it's good as a fallback for applications that don't provide
       _NET_WM_USER_TIME, and missing some events may at most lead
       to unwanted focus stealing.
     - the timestamp may come from application startup notification.
       Application startup notification, if it exists for the new mapped window,
       should include time of the user action that caused it.
     - if there's no timestamp available, it's checked whether the new window
       belongs to some already running application - if yes, the timestamp
       will be 0 (i.e. refuse activation)
     - if the window is from session restored window, the timestamp will
       be 0 too, unless this application was the active one at the time
       when the session was saved, in which case the window will be
       activated if there wasn't any user interaction since the time
       KWin was started.
     - as the last resort, the _KDE_NET_USER_CREATION_TIME timestamp
       is used. For every toplevel window that is created (see CreateNotify
       handling), this property is set to the at that time current time.
       Since at this time it's known that the new window doesn't belong
       to any existing application (better said, the application doesn't
       have any other window mapped), it is either the very first window
       of the application, or its the only window of the application
       that was hidden before. The latter case is handled by removing
       the property from windows before withdrawing them, making
       the timestamp empty for next mapping of the window. In the sooner
       case, the timestamp will be used. This helps in case when
       an application is launched without application startup notification,
       it creates its mainwindow, and starts its initialization (that
       may possibly take long time). The timestamp used will be older
       than any user action done after launching this application.
     - if no timestamp is found at all, the window is activated.
    The check whether two windows belong to the same application (same
   process) is done in Client::belongToSameApplication(). Not 100% reliable,
   but hopefully 99,99% reliable.

 As a somewhat special case, window activation is always enabled when
 session saving is in progress. When session saving, the session
 manager allows only one application to interact with the user.
 Not allowing window activation in such case would result in e.g. dialogs
 not becoming active, so focus stealing prevention would cause here
 more harm than good.

 Windows that attempted to become active but KWin prevented this will
 be marked as demanding user attention. They'll get
 the _NET_WM_STATE_DEMANDS_ATTENTION state, and the taskbar should mark
 them specially (blink, etc.). The state will be reset when the window
 eventually really becomes active.

 There are one more ways how a window can become obstrusive, window stealing
 focus: By showing above the active window, by either raising itself,
 or by moving itself on the active desktop.
     - KWin will refuse raising non-active window above the active one,
         unless they belong to the same application. Applications shouldn't
         raise their windows anyway (unless the app wants to raise one
         of its windows above another of its windows).
     - KWin activates windows moved to the current desktop (as that seems
         logical from the user's point of view, after sending the window
         there directly from KWin, or e.g. using pager). This means
         applications shouldn't send their windows to another desktop
         (SELI TODO - but what if they do?)

 Special cases I can think of:
    - konqueror reusing, i.e. kfmclient tells running Konqueror instance
        to open new window
        - without focus stealing prevention - no problem
        - with ASN (application startup notification) - ASN is forwarded,
            and because it's newer than the instance's user timestamp,
            it takes precedence
        - without ASN - user timestamp needs to be reset, otherwise it would
            be used, and it's old; moreover this new window mustn't be detected
            as window belonging to already running application, or it wouldn't
            be activated - see Client::sameAppWindowRoleMatch() for the (rather ugly)
            hack
    - konqueror preloading, i.e. window is created in advance, and kfmclient
        tells this Konqueror instance to show it later
        - without focus stealing prevention - no problem
        - with ASN - ASN is forwarded, and because it's newer than the instance's
            user timestamp, it takes precedence
        - without ASN - user timestamp needs to be reset, otherwise it would
            be used, and it's old; also, creation timestamp is changed to
            the time the instance starts (re-)initializing the window,
            this ensures creation timestamp will still work somewhat even in this case
    - KUniqueApplication - when the window is already visible, and the new instance
        wants it to activate
        - without focus stealing prevention - _NET_ACTIVE_WINDOW - no problem
        - with ASN - ASN is forwarded, and set on the already visible window, KWin
            treats the window as new with that ASN
        - without ASN - _NET_ACTIVE_WINDOW as application request is used,
                and there's no really usable timestamp, only timestamp
                from the time the (new) application instance was started,
                so KWin will activate the window *sigh*
                - the bad thing here is that there's absolutely no chance to recognize
                    the case of starting this KUniqueApp from Konsole (and thus wanting
                    the already visible window to become active) from the case
                    when something started this KUniqueApp without ASN (in which case
                    the already visible window shouldn't become active)
                - the only solution is using ASN for starting applications, at least silent
                    (i.e. without feedback)
    - when one application wants to activate another application's window (e.g. KMail
        activating already running KAddressBook window ?)
        - without focus stealing prevention - _NET_ACTIVE_WINDOW - no problem
        - with ASN - can't be here, it's the KUniqueApp case then
        - without ASN - _NET_ACTIVE_WINDOW as application request should be used,
            KWin will activate the new window depending on the timestamp and
            whether it belongs to the currently active application

 _NET_ACTIVE_WINDOW usage:
 data.l[0]= 1 ->app request
          = 2 ->pager request
          = 0 - backwards compatibility
 data.l[1]= timestamp
*/


//****************************************
// Workspace
//****************************************


/*!
  Informs the workspace about the active client, i.e. the client that
  has the focus (or None if no client has the focus). This functions
  is called by the client itself that gets focus. It has no other
  effect than fixing the focus chain and the return value of
  activeClient(). And of course, to propagate the active client to the
  world.
 */
void Workspace::setActiveClient( Client* c, allowed_t )
    {
    if ( active_client == c )
        return;
    if( popup && popup_client != c && set_active_client_recursion == 0 ) 
        {
        popup->close();
        popup_client = 0;
        }
    StackingUpdatesBlocker blocker( this );
    ++set_active_client_recursion;
    if( active_client != NULL )
        { // note that this may call setActiveClient( NULL ), therefore the recursion counter
        active_client->setActive( false );
        }
    active_client = c;
    Q_ASSERT( c == NULL || c->isActive());
    if( active_client != NULL )
        last_active_client = active_client;
    if ( active_client ) 
        {
        focus_chain.remove( c );
        if ( c->wantsTabFocus() )
            focus_chain.append( c );
        active_client->demandAttention( false );
        }

    updateCurrentTopMenu();
    updateToolWindows( false );

    updateStackingOrder(); // e.g. fullscreens have different layer when active/not-active

    rootInfo->setActiveWindow( active_client? active_client->window() : 0 );
    updateColormap();
    --set_active_client_recursion;
    }

/*!
  Tries to activate the client \a c. This function performs what you
  expect when clicking the respective entry in a taskbar: showing and
  raising the client (this may imply switching to the another virtual
  desktop) and putting the focus onto it. Once X really gave focus to
  the client window as requested, the client itself will call
  setActiveClient() and the operation is complete. This may not happen
  with certain focus policies, though.

  \sa stActiveClient(), requestFocus()
 */
void Workspace::activateClient( Client* c, bool force )
    {
    if( c == NULL )
        {
        setActiveClient( NULL, Allowed );
        return;
        }
    raiseClient( c );
    if (!c->isOnDesktop(currentDesktop()) )
        {
        ++block_focus;
        setCurrentDesktop( c->desktop() );
        --block_focus;
        // popupinfo->showInfo( desktopName(currentDesktop()) ); // AK - not sure
        }
    if( c->isMinimized())
        c->unminimize();

    if( options->focusPolicyIsReasonable())
        requestFocus( c, force );

    c->updateUserTime();
    }

/*!
  Tries to activate the client by asking X for the input focus. This
  function does not perform any show, raise or desktop switching. See
  Workspace::activateClient() instead.

  \sa Workspace::activateClient()
 */
void Workspace::requestFocus( Client* c, bool force )
    { // the 'if( c == active_client ) return;' optimization mustn't be done here
    if (!focusChangeEnabled() && ( c != active_client) )
        return;

    //TODO will be different for non-root clients. (subclassing?)
    if ( !c ) 
        {
        focusToNull();
        return;
        }

    if( !c->isOnCurrentDesktop()) // shouldn't happen, call activateClient() if needed
        {
        kdWarning( 1212 ) << "requestFocus: not on current desktop" << endl;
        return;
        }

    Client* modal = c->findModal();
    if( modal != NULL && modal != c )	
        { 
        if( !modal->isOnDesktop( c->desktop()))
            modal->setDesktop( c->desktop());
        requestFocus( modal, force );
        return;
        }
    if ( c->isShown( false ) ) 
        {
        c->takeFocus( force, Allowed );
        should_get_focus.append( c );
        focus_chain.remove( c );
        if ( c->wantsTabFocus() )
            focus_chain.append( c );
        }
    else if ( c->isShade() && c->wantsInput()) 
        {
        // client cannot accept focus, but at least the window should be active (window menu, et. al. )
        c->setActive( true );
        focusToNull();
        }
    }

/*!
  Informs the workspace that the client \a c has been hidden. If it
  was the active client (or to-become the active client),
  the workspace activates another one.

  \a c may already be destroyed
 */
void Workspace::clientHidden( Client* c )
    {
    assert( !c->isShown( true ) || !c->isOnCurrentDesktop());
    activateNextClient( c );
    }

// deactivates 'c' and activates next client    
void Workspace::activateNextClient( Client* c )
    {
    // if 'c' is not the active or the to-become active one, do nothing
    if( !( c == active_client
            || ( should_get_focus.count() > 0 && c == should_get_focus.last())))
        return;
    if( popup )
        popup->close();
    if( c == active_client )
        setActiveClient( NULL, Allowed );
    should_get_focus.remove( c );
    if( focusChangeEnabled())
        {
        if ( c->wantsTabFocus() && focus_chain.contains( c ) )
            {
            focus_chain.remove( c );
            focus_chain.prepend( c );
            }
        if ( options->focusPolicyIsReasonable())
            { // search the focus_chain for a client to transfer focus to
          // if 'c' is transient, transfer focus to the first suitable mainwindow
            Client* get_focus = NULL;
            const ClientList mainwindows = c->mainClients();
            for( ClientList::ConstIterator it = focus_chain.fromLast();
                 it != focus_chain.end();
                 --it )
                {
                if( !(*it)->isShown( false ) || !(*it)->isOnCurrentDesktop())
                    continue;
                if( mainwindows.contains( *it ))
                    {
                    get_focus = *it;
                    break;
                    }
                if( get_focus == NULL )
                    get_focus = *it;
                }
            if( get_focus == NULL )
                get_focus = findDesktop( true, currentDesktop());
            if( get_focus != NULL )
                requestFocus( get_focus );
            else
                focusToNull();
            }
        }
    else
        // if blocking focus, move focus to the desktop later if needed
        // in order to avoid flickering
        focusToNull();
    }


void Workspace::gotFocusIn( const Client* c )
    {
    if( should_get_focus.contains( const_cast< Client* >( c )))
        { // remove also all sooner elements that should have got FocusIn,
      // but didn't for some reason (and also won't anymore, because they were sooner)
        while( should_get_focus.first() != c )
            should_get_focus.pop_front();
        }
    }


// focus_in -> the window got FocusIn event
// session_active -> the window was active when saving session
bool Workspace::allowClientActivation( const Client* c, Time time, bool focus_in, bool session_active )
    {
    // options->focusStealingPreventionLevel :
    // 0 - none    - old KWin behaviour, new windows always get focus
    // 1 - low     - focus stealing prevention is applied normally, when unsure, activation is allowed
    // 2 - normal  - focus stealing prevention is applied normally, when unsure, activation is not allowed,
    //              this is the default
    // 3 - high    - new window gets focus only if it belongs to the active application,
    //              or when no window is currently active
    // 4 - extreme - no window gets focus without user intervention
    if( session_saving
        && options->focusStealingPreventionLevel <= 3 ) // <= normal
        {
        return true;
        }
    Client* ac = mostRecentlyActivatedClient();
    if( focus_in )
        {
        if( should_get_focus.contains( const_cast< Client* >( c )))
            return true; // FocusIn was result of KWin's action
        // Before getting FocusIn, the active Client already
        // got FocusOut, and therefore got deactivated.
        ac = last_active_client;
        }
    if( options->focusStealingPreventionLevel == 0 ) // none
        return true;
    if( options->focusStealingPreventionLevel == 5 ) // extreme
        return false;
    if( ac == NULL || ac->isDesktop())
        {
        kdDebug( 1212 ) << "Activation: No client active, allowing" << endl;
        return true; // no active client -> always allow
        }
    if( options->ignoreFocusStealingClasses.contains(QString::fromLatin1(c->resourceClass())))
        return true;
    if( time == 0 ) // explicitly asked not to get focus
        return false;
    // TODO window urgency  -> return true?
    if( Client::belongToSameApplication( c, ac, true ))
        {
        kdDebug( 1212 ) << "Activation: Belongs to active application" << endl;
        return true;
        }
    if( options->focusStealingPreventionLevel == 4 ) // high
        return false;
    if( time == -1U )  // no time known
        if( session_active )
            return !was_user_interaction; // see Client::readUserTimeMapTimestamp()
        else
        {
        kdDebug() << "Activation: No timestamp at all" << endl;
        if( options->focusStealingPreventionLevel == 1 ) // low
            return true;
        // no timestamp at all, don't activate - because there's also creation timestamp
        // done on CreateNotify, this case should happen only in case application
        // maps again already used window, i.e. this won't happen after app startup
        return false; 
        }
    // options->focusStealingPreventionLevel == 2 // normal
    Time user_time = ac->userTime();
    kdDebug( 1212 ) << "Activation, compared:" << time << ":" << user_time
        << ":" << ( timestampCompare( time, user_time ) >= 0 ) << endl;
    return timestampCompare( time, user_time ) >= 0; // time >= user_time
    }

// basically the same like allowClientActivation(), this time allowing
// a window to be fully raised upon its own request (XRaiseWindow),
// if refused, it will be raised only on top of windows belonging
// to the same application
bool Workspace::allowFullClientRaising( const Client* c )
    {
    if( session_saving
        && options->focusStealingPreventionLevel <= 3 ) // <= normal
        {
        return true;
        }
    Client* ac = mostRecentlyActivatedClient();
    if( options->focusStealingPreventionLevel == 0 ) // none
        return true;
    if( options->focusStealingPreventionLevel == 5 ) // extreme
        return false;
    if( ac == NULL || ac->isDesktop())
        {
        kdDebug( 1212 ) << "Raising: No client active, allowing" << endl;
        return true; // no active client -> always allow
        }
    if( options->ignoreFocusStealingClasses.contains(QString::fromLatin1(c->resourceClass())))
        return true;
    // TODO window urgency  -> return true?
    if( Client::belongToSameApplication( c, ac, true ))
        {
        kdDebug( 1212 ) << "Raising: Belongs to active application" << endl;
        return true;
        }
    if( options->focusStealingPreventionLevel == 4 ) // high
        return false;
    if( !c->hasUserTimeSupport())
        {
        kdDebug() << "Raising: No support" << endl;
        if( options->focusStealingPreventionLevel == 1 ) // low
            return true;
        }
    // options->focusStealingPreventionLevel == 2 // normal
    kdDebug() << "Raising: Refusing" << endl;
    return false;
    }

// called from Client after FocusIn that wasn't initiated by KWin and the client
// wasn't allowed to activate
void Workspace::restoreFocus()
    {
    // this updateXTime() is necessary - as FocusIn events don't have
    // a timestamp *sigh*, kwin's timestamp would be older than the timestamp
    // that was used by whoever caused the focus change, and therefore
    // the attempt to restore the focus would fail due to old timestamp
    updateXTime();
    if( should_get_focus.count() > 0 )
        requestFocus( should_get_focus.last());
    else if( last_active_client )
        requestFocus( last_active_client );
    }

void Workspace::clientAttentionChanged( Client* c, bool set )
    {
    if( set )
        {
        attention_chain.remove( c );
        attention_chain.prepend( c );
        }
    else
        attention_chain.remove( c );
    }

// This is used when a client should be shown active immediately after requestFocus(),
// without waiting for the matching FocusIn that will really make the window the active one.
// Used only in special cases, e.g. for MouseActivateRaiseandMove with transparent windows,
bool Workspace::fakeRequestedActivity( Client* c )
    {
    if( should_get_focus.count() > 0 && should_get_focus.last() == c )
        {
        if( c->isActive())
            return false;
        c->setActive( true );
        return true;
        }
    return false;
    }

void Workspace::unfakeActivity( Client* c )
    {
    if( should_get_focus.count() > 0 && should_get_focus.last() == c )
        { // TODO this will cause flicker, and probably is not needed
        if( last_active_client != NULL )
            last_active_client->setActive( true );
        else
            c->setActive( false );
        }
    }


//********************************************
// Client
//********************************************

/*!
  Updates the user time (time of last action in the active window).
  This is called inside  kwin for every action with the window
  that qualifies for user interaction (clicking on it, activate it
  externally, etc.).
 */
void Client::updateUserTime( Time time )
    {
    if( time == CurrentTime )
        time = qt_x_time;
    if( time != -1U
        && ( user_time == CurrentTime
            || timestampCompare( time, user_time ) > 0 )) // time > user_time
        user_time = time;
    }

Time Client::readUserCreationTime() const
    {
    long result = -1; // Time == -1 means none
    Atom type;
    int format, status;
    unsigned long nitems = 0;
    unsigned long extra = 0;
    unsigned char *data = 0;
    KXErrorHandler handler; // ignore errors?
    status = XGetWindowProperty( qt_xdisplay(), window(),
        atoms->kde_net_wm_user_creation_time, 0, 10000, FALSE, XA_CARDINAL,
        &type, &format, &nitems, &extra, &data );
    if (status  == Success )
        {
        if (data && nitems > 0)
            result = *((long*) data);
        XFree(data);
        }
    return result;       
    }

void Client::demandAttention( bool set )
    {
    if( isActive())
        set = false;
    info->setState( set ? NET::DemandsAttention : 0, NET::DemandsAttention );
    workspace()->clientAttentionChanged( this, set );
    }

// TODO I probably shouldn't be lazy here and do it without the macro, so that people can read it
KWIN_COMPARE_PREDICATE( SameApplicationActiveHackPredicate, const Client*,
    // ignore already existing splashes, toolbars, utilities, menus and topmenus,
    // as the app may show those before the main window
    !cl->isSplash() && !cl->isToolbar() && !cl->isTopMenu() && !cl->isUtility() && !cl->isMenu()
    && Client::belongToSameApplication( cl, value, true ) && cl != value);

Time Client::readUserTimeMapTimestamp( const KStartupInfoData* asn_data,
    const SessionInfo* session ) const
    {
    Time time = info->userTime();
    kdDebug( 1212 ) << "User timestamp, initial:" << time << endl;
    // newer ASN timestamp always replaces user timestamp, unless user timestamp is 0
    // helps e.g. with konqy reusing
    if( asn_data != NULL && time != 0
        && ( time == -1U
            || ( asn_data->timestamp() != -1U
                && timestampCompare( asn_data->timestamp(), time ) > 0 )))
        time = asn_data->timestamp();
    kdDebug( 1212 ) << "User timestamp, ASN:" << time << endl;
    if( time == -1U )
        { // The window doesn't have any timestamp.
      // If it's the first window for its application
      // (i.e. there's no other window from the same app),
      // use the _KDE_NET_WM_USER_CREATION_TIME trick.
      // Otherwise, refuse activation of a window
      // from already running application if this application
      // is not the active one.
        Client* act = workspace()->mostRecentlyActivatedClient();
        if( act != NULL && !belongToSameApplication( act, this, true ))
            {
            bool first_window = true;
            if( isTransient())
                {
                if( act->hasTransient( this, true ))
                    ; // is transient for currently active window, even though it's not
                      // the same app (e.g. kcookiejar dialog) -> allow activation
                else if( groupTransient() &&
                    findClientInList( mainClients(), SameApplicationActiveHackPredicate( this )) == NULL )
                    ; // standalone transient
                else
                    first_window = false;
                }
            else
                {
                if( workspace()->findClient( SameApplicationActiveHackPredicate( this )))
                    first_window = false;
                }
            if( !first_window )
                {
                kdDebug( 1212 ) << "User timestamp, already exists:" << 0 << endl;
                return 0; // refuse activation
                }
            }
        // Creation time would just mess things up during session startup,
        // as possibly many apps are started up at the same time.
        // If there's no active window yet, no timestamp will be needed,
        // as plain Workspace::allowClientActivation() will return true
        // in such case. And if there's already active window,
        // it's better not to activate the new one.
        // Unless it was the active window at the time
        // of session saving and there was no user interaction yet,
        // this check will be done in Workspace::allowClientActiovationTimestamp().
        if( session && !session->fake )
            return -1U;
        time = readUserCreationTime();
        }
    kdDebug( 1212 ) << "User timestamp, final:" << time << endl;
    return time;
    }


/*!
  Sets the client's active state to \a act.

  This function does only change the visual appearance of the client,
  it does not change the focus setting. Use
  Workspace::activateClient() or Workspace::requestFocus() instead.

  If a client receives or looses the focus, it calls setActive() on
  its own.

 */
void Client::setActive( bool act)
    {
    if ( active == act )
        return;
    active = act;
    workspace()->setActiveClient( act ? this : NULL, Allowed );

    if ( active )
        Notify::raise( Notify::Activate );

    if( !active )
        cancelAutoRaise();

    if( !active && shade_mode == ShadeActivated )
        setShade( ShadeNormal );

    StackingUpdatesBlocker blocker( workspace());
    workspace()->updateClientLayer( this ); // active windows may get different layer
    // TODO optimize? mainClients() may be a bit expensive
    ClientList mainclients = mainClients();
    for( ClientList::ConstIterator it = mainclients.begin();
         it != mainclients.end();
         ++it )
        if( (*it)->isFullScreen()) // fullscreens go high even if their transient is active
            workspace()->updateClientLayer( *it );
    if( decoration != NULL )
        decoration->activeChange();
    updateMouseGrab();
    updateUrgency(); // demand attention again if it's still urgent
    }

void Client::startupIdChanged()
    {
    KStartupInfoData asn_data;
    bool asn_valid = workspace()->checkStartupNotification( this, asn_data );
    if( !asn_valid )
        return;
    if( asn_data.desktop() != 0 )
        workspace()->sendClientToDesktop( this, asn_data.desktop(), true );
    if( asn_data.timestamp() != -1U )
        {
        bool activate = workspace()->allowClientActivation( this, asn_data.timestamp());
        if( asn_data.desktop() != 0 && !isOnCurrentDesktop())
            activate = false; // it was started on different desktop than current one
        if( activate )
            workspace()->activateClient( this );
        else
            demandAttention();
        }
    }

void Client::updateUrgency()
    {
    if( urgency )
        demandAttention();
    }
    
} // namespace
