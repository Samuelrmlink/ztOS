#!/bin/sh
# launch.sh RUNNAME CASE...: exp.sh detached as root under ztlock, log in /var/tmp/always-on/RUNNAME.log
R=$1; shift
exec sudo systemd-run --unit=ao-$R --collect -p Type=exec -E SETTLE=${SETTLE:-40} -E N=${N:-90} /bin/sh -c "/usr/local/bin/ztlock run always-on '/var/tmp/always-on/exp.sh $*' > /var/tmp/always-on/$R.log 2>&1"
