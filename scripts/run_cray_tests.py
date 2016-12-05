#!/usr/bin/env python
#
# Copyright (c) 2015-2016 Cray Inc.  All rights reserved.
#
# This software is available to you under a choice of one of two
# licenses.  You may choose to be licensed under the terms of the GNU
# General Public License (GPL) Version 2, available from the file
# COPYING in the main directory of this source tree, or the
# BSD license below:
#
#     Redistribution and use in source and binary forms, with or
#     without modification, are permitted provided that the following
#     conditions are met:
#
#      - Redistributions of source code must retain the above
#        copyright notice, this list of conditions and the following
#        disclaimer.
#
#      - Redistributions in binary form must reproduce the above
#        copyright notice, this list of conditions and the following
#        disclaimer in the documentation and/or other materials
#        provided with the distribution.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AWV
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
# BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
# ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#

#
# This script runs the craytests suite.
#
# TODO:
# - move test list to separate file
# - add per test .yaml configuration file for options
#
import argparse, subprocess
import sys, socket
import os, select, fcntl, time # for timeout stuff
from signal import SIGALRM

testlist = [ ['name=./rdm_pingpong', 'nnodes=2', 'cpus_per_task=1', 'args=-t1'],
             ['name=./rdm_pingpong', 'nnodes=2', 'cpus_per_task=2', 'args=-t2'],
             ['name=./rdm_pingpong', 'nnodes=2', 'cpus_per_task=4', 'args=-t4'],
             ['name=./rdm_pingpong', 'nnodes=2', 'cpus_per_task=8', 'args=-t8'],
             ['name=./rdm_pingpong', 'nnodes=2', 'cpus_per_task=16', 'args=-t16'],
             ['name=./rdm_pingpong', 'nnodes=2', 'cpus_per_task=24', 'args=-t24'],
             ['name=./rdm_one_sided', 'nnodes=2', 'cpus_per_task=1', 'args=-t1'],
             ['name=./rdm_one_sided', 'nnodes=2', 'cpus_per_task=2', 'args=-t2'],
             ['name=./rdm_one_sided', 'nnodes=2', 'cpus_per_task=4', 'args=-t4'],
             ['name=./rdm_one_sided', 'nnodes=2', 'cpus_per_task=8', 'args=-t8'],
             ['name=./rdm_one_sided', 'nnodes=2', 'cpus_per_task=16', 'args=-t16'],
             ['name=./rdm_one_sided', 'nnodes=2', 'cpus_per_task=24', 'args=-t24'],
             ['name=./rdm_mbw_mr', 'nnodes=2', 'ntasks=2', 'cpu_bind=none', 'timeout=600'],
             ['name=./random_access', 'nnodes=2', 'ntasks=2', 'cpu_bind=none']
             ]

class craytests:
    def __init__(self, _name, _args, _provider, _timeout,
                 _nnodes, _ntasks, _cpus_per_task, _nthreads, _launcher=None,
                 _nodelist=None, _cpu_bind=None):
        self.name = _name
        self.args = _args
        self.provider = _provider
        self.nnodes = _nnodes
        self.ntasks = _ntasks
        self.cpus_per_task = _cpus_per_task
        self.nthreads = _nthreads
        self.timeout = _timeout
        self.launcher = _launcher
        self.nodelist = _nodelist
        self.cpu_bind = _cpu_bind

    def __str__(self):
        s = 'craytests object:\n'
        s += '\tname: '+self.name+'\n'
        if self.args != None:
            s += '\targs: '+self.args+'\n'
        else:
            s += '\targs: None\n'
        if self.provider != None:
            s += '\tprovider: '+self.provider+'\n'
        else:
            s += '\tprovider: default\n'
        s += '\tnnodes: '+self.nnodes+'\n'
        s += '\tntask: '+self.ntasks+'\n'
        s += '\tncpus_per_tasks: '+self.cpus_per_task+'\n'
        s += '\tnthreads: '+self.nthreads+'\n'
        s += '\ttimeout: '+str(self.timeout)+'\n'
        if self.launcher != None:
            s += '\tlaucher: '+self.launcher+'\n'
        else:
            s += '\tlauncher: None\n'
        if self.nodelist != None:
            s += '\tnodelist: '+self.nodelist+'\n'
        else:
            s += '\tnodelist: None\n'
        if self.cpu_bind != None:
            s += '\tcpu_bind: '+self.cpu_bind+'\n'
        else:
            s += '\tcpu_bind: None\n'
        return s

    def start(self):
        cmd = list()
        if self.launcher == 'srun':
            cmd += ['srun', '--nodes='+self.nnodes,
                    '--exclusive',
                    '-t'+self.formattedTimeout(),
                    '--ntasks-per-node='+self.ntasks,
                    '--cpus-per-task='+self.cpus_per_task ]
            if self.nthreads != None:
                cmd += [ '--cpus-per-task='+self.nthreads ]
            if self.nodelist != None:
                cmd += [ '-nodelist='+self.nodelist ]
            if self.cpu_bind != None:
                cmd += [ '--cpu_bind='+self.cpu_bind ]
        elif self.launcher == 'aprun':
            cmd += ['aprun', '-n'+self.nnodes,
                    '-t'+str(self.timeout),
                    '-N'+self.ntasks,
                    '-d'+self.cpus_per_task ]
            if self.nthreads != None:
                cmd += [ '-d'+self.nthreads ]
            if self.nodelist != None:
                cmd += [ '-L'+self.nodelist ]
        cmd += [self.name]
        if self.provider != None:
            cmd += [ '-p'+self.provider.strip() ]
        if self.args != None:
            cmd += self.args.split()

        self.cmd = ' '.join(cmd)
        sys.stdout.write(('Starting: %s\n')%(self.cmd))

        try:
            self.start_time = time.time()
            self.p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.end_time = self.start_time+self.timeout
            return 0
        except (OSError, ValueError) as ex:
            sys.stdout.write(('Failed to start: %s\n')%(str(ex)))
            return -1

    def formattedTimeout(self):
        if self.launcher == 'srun':
            return ('%02d:%02d:%02d')%(self.timeout/3600, (self.timeout%3600)/60, self.timeout%60)
        else:
            return str(self.timeout)
            
    #
    # Newer versions of python subprocess support a time out for
    # wait() and functions that call wait(), e.g. communicate().  For
    # now, implement our own.  This version waits on one or more
    # streams, so maybe it's preferable anyways.
    #
    # Don't want to send SIGALRM, as it will may not respond if
    # holding certain locks
    #
    class ReadTimeoutException(Exception):
        def __init__(self, message):
            super(fabtest.ReadTimeoutException, self).__init__(message)
            sys.stdout.write(('%s\n')%(message))

    @staticmethod
    def waitall(plist):
        num_procs = 0
        streams = list()
        no_timeout = list()
        done = list()
        for proc in plist:
            if proc.launcher == None:
                stream = proc.p.stdout
                flags = fcntl.fcntl(stream.fileno(), fcntl.F_GETFL)
                flags |= os.O_NONBLOCK
                fcntl.fcntl(stream.fileno(), fcntl.F_SETFL, flags)
                streams.append(stream)
                done.append(False)
            else:
                streams.append(None)
                no_timeout.append([num_procs, proc])
                done.append(True)
            num_procs += 1
        # output buffers
        buffers = ['']*num_procs
        # just the streams were waiting on
        sstreams = filter(None, streams)
        # not really SIGALRM, but pretend it is
        retvals = [-SIGALRM]*num_procs
        max_end_time = max(p.end_time for p in plist)
        try:
            while sum(d for d in done) != num_procs:
                now = time.time()
                ready_set = select.select(sstreams, [], [], max(0, max_end_time-now))[0]
                for i in range(num_procs):
                    if streams[i] == None:
                        continue
                    if streams[i] in ready_set:
                        bytes = streams[i].read()
                        if len(bytes) == 0:
                            done[i] = True
                            retvals[i] = plist[i].p.wait()
                            continue
                        buffers[i] += bytes
                    if plist[i].end_time <= now:
                        raise fabtest.ReadTimeoutException('Timeout: process '+str(i))
        except fabtest.ReadTimeoutException:
            for proc in plist:
                try:
                    proc.p.kill()
                except:
                    pass

        # Now wait for procs who's timeout is enforced by the launcher
        for p in no_timeout:
            i = p[0]
            buffers[i] += p[1].p.communicate()[0]
            retvals[i] = p[1].p.returncode

        # Note that if one of the processes exits with failure early
        # on, we still wait the entire timeout duration.

        return (retvals, buffers)


ct_default_timeout = 60

def _main():
    parser = argparse.ArgumentParser(description='Run the Cray test suite.',
                                     formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument('-l', '--launcher', dest='launcher', default=None,
                        choices=['aprun', 'srun', None],
                        help='Launcher mechnism')
    parser.add_argument('--nodelist', dest='nodelist', default=None,
                        help='List of nodes to execute on')
    parser.add_argument('--cpu_bind', dest='cpu_bind', default=None,
                        help='CPU binding option')
    parser.add_argument('-p', '--provider', dest='provider', default=None,
                        help='provider (e.g., gni)')
    parser.add_argument('-j', '--jobs', dest='jobs', action='store', type=int,
                        default=1, help='Number of jobs to run simultaneously')

    args = parser.parse_args()

    launcher = args.launcher
    nodelist = args.nodelist
    cpu_bind = args.cpu_bind
    provider = args.provider
    max_jobs = args.jobs
    timeout = ct_default_timeout

    ctlist = list()
    for tparams in testlist:
        tname = None
        targs = None
        tnnodes = '1'
        tntasks = '1'
        tcpus_per_task = '1'
        tnthreads = None
        ttimeout = timeout
        tnodelist = nodelist
        tcpu_bind = cpu_bind
        tprovider = provider
        for p in tparams:
            (param, val) = p.split('=')
            if param == 'name':
                tname = val
            elif param == 'args':
                targs = val
            elif param == 'nnodes':
                tnnodes = val
            elif param == 'ntasks':
                tntasks = val
            elif param == 'cpus_per_task':
                tcpus_per_task = val
            elif param == 'nthreads':
                tnthreads = val
            elif param == 'timeout':
                ttimeout = int(val)
            elif param == 'provider':
                tprovider = val
            elif param == 'nodelist':
                tnodelist = val
            elif param == 'cpu_bind':
                tcpu_bind = val
            else:
                sys.stdout.write('Ignoring unrecogized parameter type: %s\n'%(param))
        if tname == None:
            sys.stdout.write('Skipping unnamed test configuration\n')
            continue

        ct = craytests(tname, targs, tprovider, ttimeout, tnnodes, tntasks, tcpus_per_task, tnthreads, launcher, tnodelist, tcpu_bind)
        if ct == None:
            sys.stdout.write('Failed to create test \'%s\'\n'%(tname))
            return -1
        ctlist.append(ct)

    plist = list()
    failed_tests = list()
    run_tests = 0
    job_num = 0
    for (i, ct) in enumerate(ctlist):
        if ct.start() != 0:
            sys.stdout.write('Skipping %s\n'%(ct.name))
            continue
        plist.append(ct)
        job_num += 1
        run_tests += 1
        if (job_num == max_jobs) or (i == len(ctlist)-1):
            (retvals, output) = craytests.waitall(plist)
            for i,retval in enumerate(retvals):
                sys.stdout.write('###\n### %s\n###\n'%(plist[i].cmd))
                sys.stdout.write(output[i])
                sys.stdout.write(('###\n### EXIT STATUS: %d\n###\n\n')%(retval))
                if retval != 0:
                    failed_tests.append((plist[i].name, retval))
            job_num = 0
            plist = list()

    sys.stdout.write('###\n### %d PASSSED\n'%(run_tests))
    sys.stdout.write('### %d FAILED\n'%(len(failed_tests)))
    sys.stdout.write('### %d NOT RUN\n'%(len(testlist)-run_tests))
    if len(failed_tests) != 0:
        sys.stdout.write('###\n### FAILED TESTS\n###\n')
        for t in failed_tests:
            sys.stdout.write('\t%s: %d\n'%(t[0], t[1]))
        return -1

    return 0

if __name__ == '__main__':
    sys.exit(_main())

