#
#	sample .bashrc file for AERA
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
export SHELL=$(which bash)
export HISTFILE=$HOME/.bash_history
export PS1='\s-\v \w > '

# if running inside the AERA terminal
[ -n "$ANDROID_SOCKET_recovery" ] && export TERM=pcansi

# aliases
alias cls="clear"
alias seek='find . -type d -path ./proc -prune -o -name "$@"'
alias dirp="ls -all --color=auto -t | more"
alias dirt="ls -all --color=auto -t"
alias dirs="ls -all --color=auto -S"
alias dir="ls -all --color=auto"
alias rd="rmdir"
alias md="mkdir"
alias del="rm -i"
alias ren="mv -i"
alias copy="cp -i"
alias q="exit"
alias diskfree="df -Ph"
alias path="echo $PATH"
alias mem="cat /proc/meminfo && free"
alias ver="cat /proc/version"
alias makediff="diff -u -d -w -B"
alias makediff_recurse="diff -U3 -d -w -rN"

# go to a neutral location
cd /tmp
#
