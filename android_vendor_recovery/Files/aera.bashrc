#
# 	sample system-wide bashrc file for AERA
#
#	This file is part of the OrangeFox Recovery Project
# 	Copyright (C) 2018-2026 The OrangeFox Recovery Project
#
#	OrangeFox is free software: you can redistribute it and/or modify
#	it under the terms of the GNU General Public License as published by
#	the Free Software Foundation, either version 3 of the License, or
#	any later version.
#
#	OrangeFox is distributed in the hope that it will be useful,
#	but WITHOUT ANY WARRANTY; without even the implied warranty of
#	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#	GNU General Public License for more details.
#
# 	This software is released under GPL version 3 or any later version.
#	See <http://www.gnu.org/licenses/>.
#
# 	Please maintain this if you use this script or any part of it
#

# HOME
HOME=$(getprop "ro.aera.home")
[ -z "$HOME" ] && HOME=/sdcard/AERA
[ ! -d $HOME ] && mkdir -p -m 0777 $HOME
[ ! -d $HOME ] && HOME=/tmp
export HOME

# shell
export PS1='\s-\v \w > '

# aliases
alias cls="clear"
alias seek='find . -name "$@"'
alias dir="ls -all --color=auto"
alias rd="rmdir"
alias md="mkdir"
alias del="rm -i"
alias ren="mv -i"
alias copy="cp -i"
alias diskfree="df -Ph"
alias path="echo $PATH"
alias ver="echo -n 'AERA ' && echo -n '- ' && echo -n $(getprop ro.aera.type) && echo -n ' - ' && getprop ro.aera.release.version && cat /proc/version"
#

# go to a neutral location
cd /tmp
#
