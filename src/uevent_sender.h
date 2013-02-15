/*
 * Copyright (C) 2012 Canonical Ltd.
 * Author: Martin Pitt <martin.pitt@ubuntu.com>
 *
 * umockdev is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * umockdev is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __UEVENT_SENDER_H
#    define __UEVENT_SENDER_H

typedef struct _uevent_sender uevent_sender;

uevent_sender *uevent_sender_open(const char *rootpath);
void uevent_sender_close(uevent_sender * sender);
void uevent_sender_send(uevent_sender * sender, const char *devpath, const char *action);

#endif				/* __UEVENT_SENDER_H */
