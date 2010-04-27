'''apport package hook for plymouth

Copyright 2010 Canonical Ltd.
Author: Steve Langasek <steve.langasek@ubuntu.com>
'''

from apport.hookutils import *

def add_info(report):
	attach_hardware(report)
	attach_file(report,'/proc/fb','ProcFB')
	report['DefaultPlymouth'] = command_output(['readlink', '/etc/alternatives/default.plymouth'])
	report['TextPlymouth'] = command_output(['readlink', '/etc/alternatives/text.plymouth'])
