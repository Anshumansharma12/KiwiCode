#!/usr/bin/env python
#
# Copyright 2018 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import optparse
import os
import sys
import zipfile

SRC_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__),
    '..', '..', '..'))
sys.path.append(os.path.join(SRC_DIR, 'build', 'android', 'gyp'))

from util import build_utils

def GenerateService(i):
  template = """// THIS FILE IS GENERATED BY generate_child_service.py

package org.chromium.content.app;

/**
 * This is needed to register multiple SandboxedProcess services so that we
 * can have more than one sandboxed process.
 */
public class SandboxedProcessService{0} extends SandboxedProcessService {{
}}"""
  return template.format(str(i))


def DoMain(argv):
  usage = 'usage: %prog [number] [output]'
  parser = optparse.OptionParser(usage=usage)

  options, args = parser.parse_args(argv)

  if len(args) != 2:
    parser.error('Need to specify number and output_dir')
  number, output = args
  number = int(number)

  path_template = "org/chromium/content/app/SandboxedProcessService{0}.java"
  with build_utils.AtomicOutput(output) as f:
    with zipfile.ZipFile(f, 'w', zipfile.ZIP_STORED) as srcjar:
      for i in range(number):
        build_utils.AddToZipHermetic(srcjar,
                                     path_template.format(i),
                                     data=GenerateService(i))

if __name__ == '__main__':
  DoMain(sys.argv[1:])
