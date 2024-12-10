#
# Regular cron jobs for the suricata package
#
0 4	* * *	root	[ -x /usr/bin/suricata_maintenance ] && /usr/bin/suricata_maintenance
