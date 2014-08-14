/******************************************************************************
**
** This file is part of libcommhistory.
**
** Copyright (C) 2013 Jolla Ltd. <matthew.vogt@jollamobile.com>
**
** This library is free software; you can redistribute it and/or modify it
** under the terms of the GNU Lesser General Public License version 2.1 as
** published by the Free Software Foundation.
**
** This library is distributed in the hope that it will be useful, but
** WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
** or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
** License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this library; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
**
******************************************************************************/

#ifndef COMMHISTORY_RECENT_EVENTS_MODEL_H
#define COMMHISTORY_RECENT_EVENTS_MODEL_H

#include "eventmodel.h"
#include "libcommhistoryexport.h"
#include <seasidecache.h>

namespace CommHistory {

class RecentContactsModelPrivate;

/*!
 * \class RecentContactsModel
 * \brief Model containing the most recent event for each of the contacts
 *        most recently communicated with.
 * e.g. phone number or IM user id
 */
class LIBCOMMHISTORY_EXPORT RecentContactsModel : public EventModel
{
    Q_OBJECT

    Q_PROPERTY(bool resolving READ resolving NOTIFY resolvingChanged)
    Q_ENUMS(RequiredPropertyType)

public:
    /*!
     * Model constructor.
     *
     * \param parent Parent object.
     */
    explicit RecentContactsModel(QObject *parent = 0);

    /*!
     * Destructor.
     */
    ~RecentContactsModel();

    /*!
     * Populate model with existing events.
     *
     * \return true if successful, otherwise false
     */
    Q_INVOKABLE bool getEvents();

    /*!
     * Returns true if the model is engaged in resolving contacts, or false if all
     * relevant contacts have been resolved.
     */
    bool resolving() const;

Q_SIGNALS:
    void resolvingChanged();

private:
    Q_DECLARE_PRIVATE(RecentContactsModel);
};

} // namespace CommHistory

#endif // COMMHISTORY_RECENT_EVENTS_MODEL_H
