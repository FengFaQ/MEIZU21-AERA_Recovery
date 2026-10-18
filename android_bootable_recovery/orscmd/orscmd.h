/*
		TWRP is free software: you can redistribute it and/or modify
		it under the terms of the GNU General Public License as published by
		the Free Software Foundation, either version 3 of the License, or
		(at your option) any later version.

		TWRP is distributed in the hope that it will be useful,
		but WITHOUT ANY WARRANTY; without even the implied warranty of
		MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
		GNU General Public License for more details.

		You should have received a copy of the GNU General Public License
		along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __ORSCMD_H
#define __ORSCMD_H

#define ORS_INPUT_FILE "/system/bin/orsin"
#define ORS_OUTPUT_FILE "/system/bin/orsout"

// Native AERA RPC v1 channel. Requests are JSON objects and responses are
// newline-delimited JSON events terminated by a result event.
#define AERA_RPC_INPUT_FILE "/system/bin/aerain"
#define AERA_RPC_OUTPUT_FILE "/system/bin/aeraout"
#define AERA_RPC_CANCEL_FILE "/system/bin/aeracancel"

#endif //__ORSCMD_H
