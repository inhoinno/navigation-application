/**
* @licence app begin@
* SPDX-License-Identifier: MPL-2.0
*
*
* \file HMIBgImage.qml
*
* \brief This file is part of the navigation hmi.
*
* \author Martin Schaller <martin.schaller@it-schaller.de>
*
* \version 1.0
*
* This Source Code Form is subject to the terms of the
* Mozilla Public License (MPL), v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at http://mozilla.org/MPL/2.0/.
*
* For further information see http://www.genivi.org/.
*
* List of changes:
* <date>, <name>, <description of change>
*
* @licence end@
*/
import QtQuick 1.0

BorderImage {
	property string image;
	source:"images/"+image+".png";
	anchors { fill: parent; topMargin: parent.headlineHeight}
}